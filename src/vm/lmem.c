// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lmem.h"

#include "lstate.h"
#include "ldo.h"
#include "ldebug.h"

#include <string.h>

/*
 * Luau heap uses a size-segregated page structure, with individual pages and large allocations
 * allocated using system heap (via frealloc callback).
 *
 * frealloc callback serves as a general, if slow, allocation callback that can allocate, free or
 * resize allocations:
 *
 *    void* frealloc(void* ud, void* ptr, size_t oldsize, size_t newsize);
 *
 * frealloc(ud, NULL, 0, x) creates a new block of size x
 * frealloc(ud, p, x, 0) frees the block p (must return NULL)
 * frealloc(ud, NULL, 0, 0) does nothing, equivalent to free(NULL)
 *
 * frealloc returns NULL if it cannot create or reallocate the area
 * (any reallocation to an equal or smaller size cannot fail!)
 *
 * On top of this, Luau implements heap storage which is split into two types of allocations:
 *
 * - GCO, short for "garbage collected objects"
 * - other objects (for example, arrays stored inside table objects)
 *
 * The heap layout for these two allocation types is a bit different.
 *
 * All GCO are allocated in pages, which is a block of memory of ~16K in size that has a page header
 * (lua_page_t). Each page contains 1..N blocks of the same size, where N is selected to fill the page
 * completely. This amortizes the allocation cost and increases locality. Each GCO block starts with
 * the GC header (gc_header_t) which contains the object type, mark bits and other GC metadata. If the
 * GCO block is free (not used), then it must have the type set to TNIL; in this case the block can
 * be part of the per-page free list, the link for that list is stored after the header (freegcolink).
 *
 * Importantly, the GCO block doesn't have any back references to the page it's allocated in, so it's
 * impossible to free it in isolation - GCO blocks are freed by sweeping the pages they belong to,
 * using luaM_freegco which must specify the page; this is called by page sweeper that traverses the
 * entire page's worth of objects. For this reason it's also important that freed GCO blocks keep the
 * GC header intact and accessible (with type = NIL) so that the sweeper can access it.
 *
 * Some GCOs are too large to fit in a 16K page without excessive fragmentation (the size threshold is
 * currently 512 bytes); in this case, we allocate a dedicated small page with just a single block's worth
 * storage space, but that requires allocating an extra page header. In effect large GCOs are a little bit
 * less memory efficient, but this allows us to uniformly sweep small and large GCOs using page lists.
 *
 * All GCO pages are linked in a large intrusive linked list (global_state_t.allgcopages). Additionally,
 * for each block size there's a page free list that contains pages that have at least one free block
 * (global_state_t.freegcopages). This free list is used to make sure object allocation is O(1).
 *
 * When LUAU_ASSERTENABLED is enabled, all non-GCO pages are also linked in a list (global_state_t.allpages).
 * Because this list is not strictly required for runtime operations, it is only tracked for the purposes of
 * debugging. While overhead of linking those pages together is very small, unnecessary operations are avoided.
 *
 * Compared to GCOs, regular allocations have two important differences: they can be freed in isolation,
 * and they don't start with a GC header. Because of this, each allocation is prefixed with block metadata,
 * which contains the pointer to the page for allocated blocks, and the pointer to the next free block
 * inside the page for freed blocks.
 * For regular allocations that are too large to fit in a page (using the same threshold of 512 bytes),
 * we don't allocate a separate page, instead simply using frealloc to allocate a vanilla block of memory.
 *
 * Just like GCO pages, we store a page free list (global_state_t.freepages) that allows O(1) allocation;
 * there is no global list for non-GCO pages since we never need to traverse them directly.
 *
 * In both cases, we pick the page by computing the size class from the block size which rounds the block
 * size up to reduce the chance that we'll allocate pages that have very few allocated blocks. The size
 * class strategy is determined by size_class_config_t constructor.
 *
 * Note that when the last block in a page is freed, we immediately free the page with frealloc - the
 * memory manager doesn't currently attempt to keep unused memory around. This can result in excessive
 * allocation traffic and can be mitigated by adding a page cache in the future.
 *
 * For both GCO and non-GCO pages, the per-page block allocation combines bump pointer style allocation
 * (lua_page_t.freeNext) and per-page free list (lua_page_t.freeList). We use the bump allocator to allocate
 * the contents of the page, and the free list for further reuse; this allows shorter page setup times
 * which results in less variance between allocation cost, as well as tighter sweep bounds for newly
 * allocated pages.
 */

#if defined(LUAU_ENABLE_ASAN)
#include <sanitizer/asan_interface.h>
#define ASAN_POISON_MEMORY_REGION(addr, size) __asan_poison_memory_region((addr), (size))
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) __asan_unpoison_memory_region((addr), (size))
#else
#define ASAN_POISON_MEMORY_REGION(addr, size) (void)0
#define ASAN_UNPOISON_MEMORY_REGION(addr, size) (void)0
#endif

/*
 * The sizes of most Luau objects aren't crucial for code correctness, but they are crucial for memory efficiency
 * To prevent some of them accidentally growing and us losing memory without realizing it, we're going to lock
 * the sizes of all critical structures down.
 */
#if defined(__APPLE__)
#define ABISWITCH(x64, ms32, gcc32) (sizeof(void*) == 8 ? x64 : gcc32)
#elif defined(__i386__) && defined(__MINGW32__) && !defined(__MINGW64__)
#define ABISWITCH(x64, ms32, gcc32) (ms32)
#elif defined(__i386__) && !defined(_MSC_VER)
#define ABISWITCH(x64, ms32, gcc32) (gcc32)
#else
// Android somehow uses a similar ABI to MSVC, *not* to iOS...
#define ABISWITCH(x64, ms32, gcc32) (sizeof(void*) == 8 ? x64 : ms32)
#endif

#if LUA_VECTOR_SIZE == 4
_Static_assert(sizeof(tvalue_t) == ABISWITCH(24, 24, 24), "size mismatch for value");
_Static_assert(sizeof(lua_node_t) == ABISWITCH(48, 48, 48), "size mismatch for table entry");
#else
_Static_assert(sizeof(tvalue_t) == ABISWITCH(16, 16, 16), "size mismatch for value");
_Static_assert(sizeof(lua_node_t) == ABISWITCH(32, 32, 32), "size mismatch for table entry");
#endif

_Static_assert(offsetof(tstring_t, data) == ABISWITCH(24, 20, 20), "size mismatch for string header");
_Static_assert(sizeof(lua_table_t) == ABISWITCH(48, 32, 32), "size mismatch for table header");
_Static_assert(offsetof(luauc_vm_buffer_t, data) == ABISWITCH(8, 8, 8), "size mismatch for buffer header");

// The userdata is designed to provide 16 byte alignment for 16 byte and larger userdata sizes
_Static_assert(offsetof(udata_t, data) == 16, "data must be at precise offset provide proper alignment");

// Effective limit on object size to use paged allocation.
#define LUAUC_MAX_SMALL_SIZE_USED 1024u
#define LUAUC_LARGE_PAGE_THRESHOLD 512

// constant factor to reduce our page sizes by, to increase the chances that pages we allocate will
// allow external allocators to allocate them without wasting space due to rounding introduced by their heap meta data
#define LUAUC_EXTERNAL_ALLOCATOR_METADATA_REDUCTION 24
#define LUAUC_SMALL_PAGE_SIZE (16 * 1024 - LUAUC_EXTERNAL_ALLOCATOR_METADATA_REDUCTION)
#define LUAUC_LARGE_PAGE_SIZE (32 * 1024 - LUAUC_EXTERNAL_ALLOCATOR_METADATA_REDUCTION)

#define LUAUC_BLOCK_HEADER (sizeof(double) > sizeof(void*) ? sizeof(double) : sizeof(void*))
#define LUAUC_GCO_LINK_OFFSET ((sizeof(gc_header_t) + sizeof(void*) - 1) & ~(sizeof(void*) - 1))

typedef struct size_class_config_t
{
    int sizeOfClass[LUA_SIZECLASSES];
    int classCount;
} size_class_config_t;

static const size_class_config_t __kSizeClassConfig = {
    {8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 208, 224, 240, 256, 288, 320, 352, 384, 416, 448, 480,
        512, 576, 640, 704, 768, 832, 896, 960, 1024},
    36};

_Static_assert(LUA_SIZECLASSES >= 36, "LUA_SIZECLASSES does not fit the allocator size classes");

static int __luauc_sizeclass(size_t size)
{
    if (size == 0 || size > LUAUC_MAX_SMALL_SIZE_USED)
        return -1;
    if (size <= 56)
        return (int)((size + 7) / 8) - 1;
    if (size <= 240)
        return 7 + (int)((size - 49) / 16);
    if (size <= 480)
        return 19 + (int)((size - 225) / 32);
    return 27 + (int)((size - 449) / 64);
}

// size class for a block of size sz; returns -1 for size=0 because empty allocations take no space
#define sizeclass(sz) __luauc_sizeclass(sz)

// metadata for a block is stored in the first pointer of the block
#define metadata(block) (*(void**)(block))
#define freegcolink(block) (*(void**)((char*)block + LUAUC_GCO_LINK_OFFSET))

#if defined(LUAU_ASSERTENABLED)
#define debugpageset(x) (x)
#else
#define debugpageset(x) NULL
#endif

struct lua_page_t
{
    // list of pages with free blocks
    lua_page_t* prev;
    lua_page_t* next;

    // list of all pages
    lua_page_t* listprev;
    lua_page_t* listnext;

    int pageSize;  // page size in bytes, including page header
    int blockSize; // block size in bytes, including block header (for non-GCO)

    void* freeList; // next free block in this page; linked with metadata()/freegcolink()
    int freeNext;   // next free block offset in this page, in bytes; when negative, freeList is used instead
    int busyBlocks; // number of blocks allocated out of this page

    // provide additional padding based on current object size to provide 16 byte alignment of data
    // later static_assert checks that this requirement is held
    char padding[sizeof(void*) == 8 ? 8 : 12];

    char data[1];
};

_Static_assert(offsetof(lua_page_t, data) % 16 == 0, "data must be 16 byte aligned to provide properly aligned allocation of userdata objects");

LUA_NORETURN void luaM_toobig(lua_State* L)
{
    luaG_runerror(L, "memory allocation error: block too big");
}

static lua_page_t* __newpage(lua_State* L, lua_page_t** pageset, int pageSize, int blockSize, int blockCount)
{
    global_state_t* g = L->global;

    LUAU_ASSERT(pageSize - (int)(offsetof(lua_page_t, data)) >= blockSize * blockCount);

    lua_page_t* page = (lua_page_t*)(*g->frealloc)(g->ud, NULL, 0, pageSize);
    if (!page)
        luaD_throw(L, LUA_ERRMEM);

    ASAN_POISON_MEMORY_REGION(page->data, blockSize * blockCount);

    // setup page header
    page->prev = NULL;
    page->next = NULL;

    page->listprev = NULL;
    page->listnext = NULL;

    page->pageSize = pageSize;
    page->blockSize = blockSize;

    // note: we start with the last block in the page and move downward
    // either order would work, but that way we don't need to store the block count in the page
    // additionally, GC stores objects in singly linked lists, and this way the GC lists end up in increasing pointer order
    page->freeList = NULL;
    page->freeNext = (blockCount - 1) * blockSize;
    page->busyBlocks = 0;

    if (pageset)
    {
        page->listnext = *pageset;
        if (page->listnext)
            page->listnext->listprev = page;
        *pageset = page;
    }

    return page;
}

// this is part of a cold path in newblock and newgcoblock
// it is marked as noinline to prevent it from being inlined into those functions
// if it is inlined, then the compiler may determine those functions are "too big" to be profitably inlined, which results in reduced performance
LUAU_NOINLINE static lua_page_t* __newclasspage(lua_State* L, lua_page_t** freepageset, lua_page_t** pageset, int sizeClass, bool storeMetadata)
{
    int sizeOfClass = __kSizeClassConfig.sizeOfClass[sizeClass];
    int pageSize = sizeOfClass > LUAUC_LARGE_PAGE_THRESHOLD ? LUAUC_LARGE_PAGE_SIZE : LUAUC_SMALL_PAGE_SIZE;
    int blockSize = sizeOfClass + (storeMetadata ? (int)LUAUC_BLOCK_HEADER : 0);
    int blockCount = (pageSize - offsetof(lua_page_t, data)) / blockSize;

    lua_page_t* page = __newpage(L, pageset, pageSize, blockSize, blockCount);

    // prepend a page to page freelist (which is empty because we only ever allocate a new page when it is!)
    LUAU_ASSERT(!freepageset[sizeClass]);
    freepageset[sizeClass] = page;

    return page;
}

static void __freepage(lua_State* L, lua_page_t** pageset, lua_page_t* page)
{
    global_state_t* g = L->global;

    if (pageset)
    {
        // remove page from alllist
        if (page->listnext)
            page->listnext->listprev = page->listprev;

        if (page->listprev)
            page->listprev->listnext = page->listnext;
        else if (*pageset == page)
            *pageset = page->listnext;
    }

    // so long
    (*g->frealloc)(g->ud, page, page->pageSize, 0);
}

static void __freeclasspage(lua_State* L, lua_page_t** freepageset, lua_page_t** pageset, lua_page_t* page, int sizeClass)
{
    // remove page from freelist
    if (page->next)
        page->next->prev = page->prev;

    if (page->prev)
        page->prev->next = page->next;
    else if (freepageset[sizeClass] == page)
        freepageset[sizeClass] = page->next;

    __freepage(L, pageset, page);
}

static void* __newblock(lua_State* L, int sizeClass)
{
    global_state_t* g = L->global;
    lua_page_t* page = g->freepages[sizeClass];

    // slow path: no page in the freelist, allocate a new one
    if (!page)
        page = __newclasspage(L, g->freepages, debugpageset(&g->allpages), sizeClass, true);

    LUAU_ASSERT(!page->prev);
    LUAU_ASSERT(page->freeList || page->freeNext >= 0);
    LUAU_ASSERT(((size_t)(page->blockSize)) == (size_t)__kSizeClassConfig.sizeOfClass[sizeClass] + LUAUC_BLOCK_HEADER);

    void* block;

    if (page->freeNext >= 0)
    {
        block = &page->data + page->freeNext;
        ASAN_UNPOISON_MEMORY_REGION(block, page->blockSize);

        page->freeNext -= page->blockSize;
        page->busyBlocks++;
    }
    else
    {
        block = page->freeList;
        ASAN_UNPOISON_MEMORY_REGION(block, page->blockSize);

        page->freeList = metadata(block);
        page->busyBlocks++;
    }

    // the first word in a block point back to the page
    metadata(block) = page;

    // if we allocate the last block out of a page, we need to remove it from free list
    if (!page->freeList && page->freeNext < 0)
    {
        g->freepages[sizeClass] = page->next;
        if (page->next)
            page->next->prev = NULL;
        page->next = NULL;
    }

    // the user data is right after the metadata
    return (char*)block + LUAUC_BLOCK_HEADER;
}

static void* __newgcoblock(lua_State* L, int sizeClass)
{
    global_state_t* g = L->global;
    lua_page_t* page = g->freegcopages[sizeClass];

    // slow path: no page in the freelist, allocate a new one
    if (!page)
        page = __newclasspage(L, g->freegcopages, &g->allgcopages, sizeClass, false);

    LUAU_ASSERT(!page->prev);
    LUAU_ASSERT(page->freeList || page->freeNext >= 0);
    LUAU_ASSERT(page->blockSize == __kSizeClassConfig.sizeOfClass[sizeClass]);

    void* block;

    if (page->freeNext >= 0)
    {
        block = &page->data + page->freeNext;
        ASAN_UNPOISON_MEMORY_REGION(block, page->blockSize);

        page->freeNext -= page->blockSize;
        page->busyBlocks++;
    }
    else
    {
        block = page->freeList;
        ASAN_UNPOISON_MEMORY_REGION((char*)block + sizeof(gc_header_t), page->blockSize - sizeof(gc_header_t));

        // when separate block metadata is not used, free list link is stored inside the block data itself
        page->freeList = freegcolink(block);
        page->busyBlocks++;
    }

    // if we allocate the last block out of a page, we need to remove it from free list
    if (!page->freeList && page->freeNext < 0)
    {
        g->freegcopages[sizeClass] = page->next;
        if (page->next)
            page->next->prev = NULL;
        page->next = NULL;
    }

    return block;
}

static void __freeblock(lua_State* L, int sizeClass, void* block)
{
    global_state_t* g = L->global;

    // the user data is right after the metadata
    LUAU_ASSERT(block);
    block = (char*)block - LUAUC_BLOCK_HEADER;

    lua_page_t* page = (lua_page_t*)metadata(block);
    LUAU_ASSERT(page && page->busyBlocks > 0);
    LUAU_ASSERT(((size_t)(page->blockSize)) == (size_t)__kSizeClassConfig.sizeOfClass[sizeClass] + LUAUC_BLOCK_HEADER);
    LUAU_ASSERT((char*)block >= page->data && (char*)block < (char*)page + page->pageSize);

    // if the page wasn't in the page free list, it should be now since it got a block!
    if (!page->freeList && page->freeNext < 0)
    {
        LUAU_ASSERT(!page->prev);
        LUAU_ASSERT(!page->next);

        page->next = g->freepages[sizeClass];
        if (page->next)
            page->next->prev = page;
        g->freepages[sizeClass] = page;
    }

    // add the block to the free list inside the page
    metadata(block) = page->freeList;
    page->freeList = block;

    ASAN_POISON_MEMORY_REGION(block, page->blockSize);

    page->busyBlocks--;

    // if it's the last block in the page, we don't need the page
    if (page->busyBlocks == 0)
        __freeclasspage(L, g->freepages, debugpageset(&g->allpages), page, sizeClass);
}

static void __freegcoblock(lua_State* L, int sizeClass, void* block, lua_page_t* page)
{
    LUAU_ASSERT(page && page->busyBlocks > 0);
    LUAU_ASSERT(page->blockSize == __kSizeClassConfig.sizeOfClass[sizeClass]);
    LUAU_ASSERT((char*)block >= page->data && (char*)block < (char*)page + page->pageSize);

    global_state_t* g = L->global;

    // if the page wasn't in the page free list, it should be now since it got a block!
    if (!page->freeList && page->freeNext < 0)
    {
        LUAU_ASSERT(!page->prev);
        LUAU_ASSERT(!page->next);

        page->next = g->freegcopages[sizeClass];
        if (page->next)
            page->next->prev = page;
        g->freegcopages[sizeClass] = page;
    }

    // when separate block metadata is not used, free list link is stored inside the block data itself
    freegcolink(block) = page->freeList;
    page->freeList = block;

    ASAN_POISON_MEMORY_REGION((char*)block + sizeof(gc_header_t), page->blockSize - sizeof(gc_header_t));

    page->busyBlocks--;

    // if it's the last block in the page, we don't need the page
    if (page->busyBlocks == 0)
        __freeclasspage(L, g->freegcopages, &g->allgcopages, page, sizeClass);
}

void* luaM_new_(lua_State* L, size_t nsize, uint8_t memcat)
{
    global_state_t* g = L->global;

    int nclass = sizeclass(nsize);

    void* block = nclass >= 0 ? __newblock(L, nclass) : (*g->frealloc)(g->ud, NULL, 0, nsize);
    if (block == NULL && nsize > 0)
        luaD_throw(L, LUA_ERRMEM);

    g->totalbytes += nsize;
    g->memcatbytes[memcat] += nsize;

    if (LUAU_UNLIKELY(!!g->cb.onallocate))
    {
        g->cb.onallocate(L, 0, nsize);
    }

    return block;
}

gc_object_t* luaM_newgco_(lua_State* L, size_t nsize, uint8_t memcat)
{
    // we need to accommodate space for link for free blocks (freegcolink)
    LUAU_ASSERT(nsize >= LUAUC_GCO_LINK_OFFSET + sizeof(void*));

    global_state_t* g = L->global;

    int nclass = sizeclass(nsize);

    void* block = NULL;

    if (nclass >= 0)
    {
        block = __newgcoblock(L, nclass);
    }
    else
    {
        lua_page_t* page = __newpage(L, &g->allgcopages, offsetof(lua_page_t, data) + ((int)(nsize)), ((int)(nsize)), 1);

        block = &page->data;
        ASAN_UNPOISON_MEMORY_REGION(block, page->blockSize);

        page->freeNext -= page->blockSize;
        page->busyBlocks++;
    }

    if (block == NULL && nsize > 0)
        luaD_throw(L, LUA_ERRMEM);

    g->totalbytes += nsize;
    g->memcatbytes[memcat] += nsize;

    if (LUAU_UNLIKELY(!!g->cb.onallocate))
    {
        g->cb.onallocate(L, 0, nsize);
    }

    return (gc_object_t*)block;
}

void luaM_free_(lua_State* L, void* block, size_t osize, uint8_t memcat)
{
    global_state_t* g = L->global;
    LUAU_ASSERT((osize == 0) == (block == NULL));

    int oclass = sizeclass(osize);

    if (oclass >= 0)
        __freeblock(L, oclass, block);
    else
        (*g->frealloc)(g->ud, block, osize, 0);

    g->totalbytes -= osize;
    g->memcatbytes[memcat] -= osize;
}

void luaM_freegco_(lua_State* L, gc_object_t* block, size_t osize, uint8_t memcat, lua_page_t* page)
{
    global_state_t* g = L->global;
    LUAU_ASSERT((osize == 0) == (block == NULL));

    int oclass = sizeclass(osize);

    if (oclass >= 0)
    {
        block->gch.tt = LUA_TNIL;

        __freegcoblock(L, oclass, block, page);
    }
    else
    {
        LUAU_ASSERT(page->busyBlocks == 1);
        LUAU_ASSERT(((size_t)(page->blockSize)) == osize);
        LUAU_ASSERT((void*)block == page->data);

        __freepage(L, &g->allgcopages, page);
    }

    g->totalbytes -= osize;
    g->memcatbytes[memcat] -= osize;
}

void* luaM_realloc_(lua_State* L, void* block, size_t osize, size_t nsize, uint8_t memcat)
{
    global_state_t* g = L->global;
    LUAU_ASSERT((osize == 0) == (block == NULL));

    int nclass = sizeclass(nsize);
    int oclass = sizeclass(osize);
    void* result;

    // if either block needs to be allocated using a block allocator, we can't use realloc directly
    if (nclass >= 0 || oclass >= 0)
    {
        result = nclass >= 0 ? __newblock(L, nclass) : (*g->frealloc)(g->ud, NULL, 0, nsize);
        if (result == NULL && nsize > 0)
            luaD_throw(L, LUA_ERRMEM);

        if (osize > 0 && nsize > 0)
            memcpy(result, block, osize < nsize ? osize : nsize);

        if (oclass >= 0)
            __freeblock(L, oclass, block);
        else
            (*g->frealloc)(g->ud, block, osize, 0);
    }
    else
    {
        result = (*g->frealloc)(g->ud, block, osize, nsize);
        if (result == NULL && nsize > 0)
            luaD_throw(L, LUA_ERRMEM);
    }

    LUAU_ASSERT((nsize == 0) == (result == NULL));
    g->totalbytes = (g->totalbytes - osize) + nsize;
    g->memcatbytes[memcat] += nsize - osize;

    if (LUAU_UNLIKELY(!!g->cb.onallocate))
    {
        g->cb.onallocate(L, osize, nsize);
    }

    return result;
}

void luaM_getpagewalkinfo(lua_page_t* page, char** start, char** end, int* busyBlocks, int* blockSize)
{
    int blockCount = (page->pageSize - offsetof(lua_page_t, data)) / page->blockSize;

    LUAU_ASSERT(page->freeNext >= -page->blockSize && page->freeNext <= (blockCount - 1) * page->blockSize);

    char* data = page->data; // silences ubsan when indexing page->data

    *start = data + page->freeNext + page->blockSize;
    *end = data + blockCount * page->blockSize;
    *busyBlocks = page->busyBlocks;
    *blockSize = page->blockSize;
}

void luaM_getpageinfo(lua_page_t* page, int* pageBlocks, int* busyBlocks, int* blockSize, int* pageSize)
{
    *pageBlocks = (page->pageSize - offsetof(lua_page_t, data)) / page->blockSize;
    *busyBlocks = page->busyBlocks;
    *blockSize = page->blockSize;
    *pageSize = page->pageSize;
}

lua_page_t* luaM_getnextpage(lua_page_t* page)
{
    return page->listnext;
}

void luaM_visitpage(lua_page_t* page, void* context, bool (*visitor)(void* context, lua_page_t* page, gc_object_t* gco))
{
    char* start;
    char* end;
    int busyBlocks;
    int blockSize;
    luaM_getpagewalkinfo(page, &start, &end, &busyBlocks, &blockSize);

    for (char* pos = start; pos != end; pos += blockSize)
    {
        gc_object_t* gco = (gc_object_t*)pos;

        // skip memory blocks that are already freed
        if (gco->gch.tt == LUA_TNIL)
            continue;

        // when true is returned it means that the element was deleted
        if (visitor(context, page, gco))
        {
            LUAU_ASSERT(busyBlocks > 0);

            // if the last block was removed, page would be removed as well
            if (--busyBlocks == 0)
                break;
        }
    }
}

void luaM_visitgco(lua_State* L, void* context, bool (*visitor)(void* context, lua_page_t* page, gc_object_t* gco))
{
    global_state_t* g = L->global;

    for (lua_page_t* curr = g->allgcopages; curr;)
    {
        lua_page_t* next = curr->listnext; // block visit might destroy the page

        luaM_visitpage(curr, context, visitor);

        curr = next;
    }
}
