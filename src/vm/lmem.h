// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#ifndef LUAUC_LMEM_H
#define LUAUC_LMEM_H

#include "lua.h"

#include <stdbool.h>

typedef struct lua_page_t lua_page_t;
typedef union gc_object_t gc_object_t;

#define luaM_newgco(L, t, size, memcat) cast_to(t*, luaM_newgco_(L, size, memcat))
#define luaM_freegco(L, p, size, memcat, page) luaM_freegco_(L, obj2gco(p), size, memcat, page)

LUAI_FUNC void* luaM_new_(lua_State* L, size_t nsize, uint8_t memcat);
LUAI_FUNC gc_object_t* luaM_newgco_(lua_State* L, size_t nsize, uint8_t memcat);
LUAI_FUNC void luaM_free_(lua_State* L, void* block, size_t osize, uint8_t memcat);
LUAI_FUNC void luaM_freegco_(lua_State* L, gc_object_t* block, size_t osize, uint8_t memcat, lua_page_t* page);
LUAI_FUNC void* luaM_realloc_(lua_State* L, void* block, size_t osize, size_t nsize, uint8_t memcat);

LUA_NORETURN LUAI_FUNC void luaM_toobig(lua_State* L);

static inline size_t __luaM_arraysize(lua_State* L, size_t count, size_t element_size)
{
    if (element_size != 0 && count > SIZE_MAX / element_size)
        luaM_toobig(L);
    return count * element_size;
}

#define luaM_arraysize_(L, n, e) __luaM_arraysize((L), (size_t)(n), (size_t)(e))
#define luaM_newarray(L, n, t, memcat) cast_to(t*, luaM_new_(L, luaM_arraysize_(L, n, sizeof(t)), memcat))
#define luaM_freearray(L, b, n, t, memcat) luaM_free_(L, (b), (n) * sizeof(t), memcat)
#define luaM_reallocarray(L, v, oldn, n, t, memcat) \
    ((v) = cast_to(t*, luaM_realloc_(L, v, (oldn) * sizeof(t), luaM_arraysize_(L, n, sizeof(t)), memcat)))

LUAI_FUNC void luaM_getpagewalkinfo(lua_page_t* page, char** start, char** end, int* busyBlocks, int* blockSize);
LUAI_FUNC void luaM_getpageinfo(lua_page_t* page, int* pageBlocks, int* busyBlocks, int* blockSize, int* pageSize);
LUAI_FUNC lua_page_t* luaM_getnextpage(lua_page_t* page);

LUAI_FUNC void luaM_visitpage(lua_page_t* page, void* context, bool (*visitor)(void* context, lua_page_t* page, gc_object_t* gco));
LUAI_FUNC void luaM_visitgco(lua_State* L, void* context, bool (*visitor)(void* context, lua_page_t* page, gc_object_t* gco));

#endif
