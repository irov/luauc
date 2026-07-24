// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lstate.h"

#include "ltable.h"
#include "lstring.h"
#include "lfunc.h"
#include "lmem.h"
#include "lgc.h"
#include "ldo.h"
#include "ldebug.h"
#include "ludata.h"

#include <string.h>

LUAU_FASTFLAG(LuauDirectFieldGet)

/*
** Main thread combines a thread state and the global state
*/
typedef struct lua_state_block_t
{
    lua_State l;
    global_state_t g;
} lua_state_block_t;

static void __stack_init(lua_State* L1, lua_State* L)
{
    // initialize call_info_t array
    L1->base_ci = luaM_newarray(L, BASIC_CI_SIZE, call_info_t, L1->memcat);
    L1->ci = L1->base_ci;
    L1->size_ci = BASIC_CI_SIZE;
    L1->end_ci = L1->base_ci + L1->size_ci - 1;
    // initialize stack array
    L1->stack = luaM_newarray(L, BASIC_STACK_SIZE + EXTRA_STACK, tvalue_t, L1->memcat);
    L1->stacksize = BASIC_STACK_SIZE + EXTRA_STACK;
    tvalue_t* stack = L1->stack;
    for (int i = 0; i < BASIC_STACK_SIZE + EXTRA_STACK; i++)
        setnilvalue(stack + i); // erase new stack
    L1->top = stack;
    L1->stack_last = stack + (L1->stacksize - EXTRA_STACK);
    // initialize first ci
    L1->ci->func = L1->top;
    L1->ci->p = NULL;
    setnilvalue(L1->top++); // `function' entry for this `ci'
    L1->base = L1->ci->base = L1->top;
    L1->ci->top = L1->top + LUA_MINSTACK;
}

static void __freestack(lua_State* L, lua_State* L1)
{
    luaM_freearray(L, L1->base_ci, L1->size_ci, call_info_t, L1->memcat);
    luaM_freearray(L, L1->stack, L1->stacksize, tvalue_t, L1->memcat);
}

/*
** open parts that may cause memory-allocation errors
*/
static void __f_luaopen(lua_State* L, void* ud)
{
    (void)ud;
    global_state_t* g = L->global;
    __stack_init(L, L);                             // init stack
    L->gt = luaH_new(L, 0, 2);                    // table of globals
    sethvalue(L, registry(L), luaH_new(L, 0, 2)); // registry
    luaS_resize(L, LUA_MINSTRTABSIZE);            // initial size of string table
    luaT_init(L);
    luaS_fix(luaS_newliteral(L, LUA_MEMERRMSG)); // pin to make sure we can always throw this error
    luaS_fix(luaS_newliteral(L, LUA_ERRERRMSG)); // pin to make sure we can always throw this error
    g->GCthreshold = 4 * g->totalbytes;
}

static void __preinit_state(lua_State* L, global_state_t* g)
{
    L->global = g;
    L->stack = NULL;
    L->stacksize = 0;
    L->gt = NULL;
    L->openupval = NULL;
    L->size_ci = 0;
    L->nCcalls = L->baseCcalls = 0;
    L->status = 0;
    L->base_ci = L->ci = NULL;
    L->namecall = NULL;
    L->cachedslot = 0;
    L->singlestep = false;
    L->isactive = false;
    L->activememcat = 0;
    L->userdata = NULL;
}

static void __close_state(lua_State* L)
{
    global_state_t* g = L->global;
    luaF_close(L, L->stack); // close all upvalues for this thread
    luaC_freeall(L);         // collect all objects
    LUAU_ASSERT(g->strt.nuse == 0);
    luaM_freearray(L, L->global->strt.hash, L->global->strt.size, tstring_t*, 0);
    __freestack(L, L);
    for (int i = 0; i < LUA_SIZECLASSES; i++)
    {
        LUAU_ASSERT(g->freepages[i] == NULL);
        LUAU_ASSERT(g->freegcopages[i] == NULL);
    }
    LUAU_ASSERT(g->allgcopages == NULL);
    LUAU_ASSERT(g->totalbytes == sizeof(lua_state_block_t));
    LUAU_ASSERT(g->memcatbytes[0] == sizeof(lua_state_block_t));
    for (int i = 1; i < LUA_MEMORY_CATEGORIES; i++)
        LUAU_ASSERT(g->memcatbytes[i] == 0);

    if (L->global->ecb.close)
        L->global->ecb.close(L);

    (*g->frealloc)(g->ud, L, sizeof(lua_state_block_t), 0);
}

lua_State* luaE_newthread(lua_State* L)
{
    lua_State* L1 = luaM_newgco(L, lua_State, sizeof(lua_State), L->activememcat);
    luaC_init(L, L1, LUA_TTHREAD);
    __preinit_state(L1, L->global);
    L1->activememcat = L->activememcat; // inherit the active memory category
    __stack_init(L1, L);                  // init stack
    L1->gt = L->gt;                     // share table of globals
    L1->singlestep = L->singlestep;
    LUAU_ASSERT(iswhite(obj2gco(L1)));
    return L1;
}

void luaE_freethread(lua_State* L, lua_State* L1, lua_page_t* page)
{
    global_state_t* g = L->global;
    if (g->cb.userthread)
        g->cb.userthread(NULL, L1);

    __freestack(L, L1);
    luaM_freegco(L, L1, sizeof(lua_State), L1->memcat, page);
}

void lua_resetthread(lua_State* L)
{
    api_check(L, !L->isactive);
    api_check(L, L->status != LUA_OK || L->ci == L->base_ci);

    // close upvalues before clearing anything
    luaF_close(L, L->stack);

    // clear call frames
    call_info_t* ci = L->base_ci;
    ci->p = NULL;
    ci->func = L->stack;
    ci->base = ci->func + 1;
    ci->top = ci->base + LUA_MINSTACK;
    setnilvalue(ci->func);
    L->ci = ci;
    if (L->size_ci != BASIC_CI_SIZE)
        luaD_reallocCI(L, BASIC_CI_SIZE);
    // clear thread state
    L->status = LUA_OK;
    L->base = L->ci->base;
    L->top = L->ci->base;
    L->nCcalls = L->baseCcalls = 0;
    // clear thread stack
    if (L->stacksize != BASIC_STACK_SIZE + EXTRA_STACK)
        luaD_reallocstack(L, BASIC_STACK_SIZE, 0);
    for (int i = 0; i < L->stacksize; i++)
        setnilvalue(L->stack + i);
}

int lua_isthreadreset(lua_State* L)
{
    return L->ci == L->base_ci && L->base == L->top && L->status == LUA_OK;
}

lua_State* lua_newstate(lua_Alloc f, void* ud)
{
    int i;
    lua_State* L;
    global_state_t* g;
    void* l = (*f)(ud, NULL, 0, sizeof(lua_state_block_t));
    if (l == NULL)
        return NULL;
    L = (lua_State*)l;
    g = &((lua_state_block_t*)L)->g;
    L->tt = LUA_TTHREAD;
    L->marked = g->currentwhite = bit2mask(WHITE0BIT, FIXEDBIT);
    L->memcat = 0;
    __preinit_state(L, g);
    g->frealloc = f;
    g->ud = ud;
    g->mainthread = L;
    g->uvhead.u.open.prev = &g->uvhead;
    g->uvhead.u.open.next = &g->uvhead;
    g->GCthreshold = 0; // mark it as unfinished state
    g->registryfree = 0;
    g->errorjmp = NULL;
    g->rngstate = 0;
    g->ptrenckey[0] = 1;
    g->ptrenckey[1] = 0;
    g->ptrenckey[2] = 0;
    g->ptrenckey[3] = 0;
    g->strt.size = 0;
    g->strt.nuse = 0;
    g->strt.hash = NULL;
    setnilvalue(&g->pseudotemp);
    setnilvalue(registry(L));
    g->gcstate = GCSpause;
    g->gray = NULL;
    g->grayagain = NULL;
    g->weak = NULL;
    g->totalbytes = sizeof(lua_state_block_t);
    g->gcgoal = LUAI_GCGOAL;
    g->gcstepmul = LUAI_GCSTEPMUL;
    g->gcstepsize = LUAI_GCSTEPSIZE << 10;

    for (i = 0; i < LUA_SIZECLASSES; i++)
    {
        g->freepages[i] = NULL;
        g->freegcopages[i] = NULL;
    }

    g->allpages = NULL;
    g->allgcopages = NULL;
    g->sweepgcopage = NULL;

    for (i = 0; i < LUA_T_COUNT; i++)
        g->mt[i] = NULL;

    for (i = 0; i < LUA_UTAG_LIMIT; i++)
    {
        g->udatagc[i] = NULL;
        g->udatamt[i] = NULL;
    }

    for (i = 0; i < UTAG_INTERNAL_LIMIT; i++)
    {
        lua_udata_direct_access_data_t* udatadirect = &L->global->udatadirect[i];

        setnilvalue(&udatadirect->indextm);
        setnilvalue(&udatadirect->newindextm);
        setnilvalue(&udatadirect->namecalltm);
        udatadirect->index = NULL;
        udatadirect->newindex = NULL;
        udatadirect->namecall = NULL;
    }

    for (i = 0; i < LUA_LUTAG_LIMIT; i++)
        g->lightuserdataname[i] = NULL;

    for (i = 0; i < UTAG_INTERNAL_LIMIT; i++)
        g->udatadirectfields[i] = NULL;

    for (i = 0; i < LUA_MEMORY_CATEGORIES; i++)
        g->memcatbytes[i] = 0;

    g->memcatbytes[0] = sizeof(lua_state_block_t);

    memset(&g->cb, 0, sizeof(g->cb));

    memset(&g->ecb, 0, sizeof(g->ecb));

    memset(g->ecbdata, 0, LUA_EXECUTION_CALLBACK_STORAGE * sizeof(g->ecbdata[0]));

    memset(&g->gcstats, 0, sizeof(g->gcstats));
    g->lastprotoid = 1;

#ifdef LUAI_GCMETRICS
    memset(&g->gcmetrics, 0, sizeof(g->gcmetrics));
#endif

    if (luaD_rawrunprotected(L, __f_luaopen, NULL) != 0)
    {
        // memory allocation error: free partial state
        __close_state(L);
        L = NULL;
    }
    return L;
}

void lua_close(lua_State* L)
{
    L = L->global->mainthread; // only the main thread can be closed
    luaF_close(L, L->stack);   // close all upvalues for this thread
    __close_state(L);
}
