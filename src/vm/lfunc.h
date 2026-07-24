// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#ifndef LUAUC_LFUNC_H
#define LUAUC_LFUNC_H

#include "lobject.h"

#define sizeCclosure(n) (offsetof(closure_t, c.upvals) + sizeof(tvalue_t) * (n))
#define sizeLclosure(n) (offsetof(closure_t, l.uprefs) + sizeof(tvalue_t) * (n))
#define getproto(cl) ((cl)->isC ? NULL : (FFlag_LuauPromoteProto && cl->l.p->optimized ? __luaF_promoteproto(cl) : (cl)->l.p))

LUAI_FUNC proto_t* luaF_newproto(lua_State* L);
LUAI_FUNC closure_t* luaF_newLclosure(lua_State* L, int nelems, lua_table_t* e, proto_t* p);
LUAI_FUNC closure_t* luaF_newCclosure(lua_State* L, int nelems, lua_table_t* e);
LUAI_FUNC upvalue_t* luaF_findupval(lua_State* L, StkId level);
LUAI_FUNC void luaF_close(lua_State* L, StkId level);
LUAI_FUNC void luaF_closeupval(lua_State* L, upvalue_t* uv, bool dead);
LUAI_FUNC void luaF_freeproto(lua_State* L, proto_t* f, struct lua_page_t* page);
LUAI_FUNC void luaF_freeclosure(lua_State* L, closure_t* c, struct lua_page_t* page);
LUAI_FUNC void luaF_freeupval(lua_State* L, upvalue_t* uv, struct lua_page_t* page);
LUAI_FUNC const local_var_t* luaF_getlocal(const proto_t* func, int local_number, int pc);
LUAI_FUNC const local_var_t* luaF_findlocal(const proto_t* func, int local_reg, int pc);
// A feedback slot is sealed when luaF_recordhit returns false.
LUAI_FUNC bool luaF_recordhit(lua_State* L, closure_t* func, closure_t* target, uint32_t slotid);
// Define it in header to force inlining
static inline proto_t* __luaF_promoteproto(closure_t* cl)
{
    LUAU_ASSERT(!cl->isC);
    while (cl->l.p->optimized != NULL)
    {
        cl->l.p = cl->l.p->optimized;
        cl->stacksize = cl->l.p->maxstacksize;
    }
    return cl->l.p;
}

#endif
