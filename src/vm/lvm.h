// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#ifndef LUAUC_LVM_H
#define LUAUC_LVM_H

#include "lobject.h"
#include "ltm.h"

#define tostring(L, o) ((ttype(o) == LUA_TSTRING) || (luaV_tostring(L, o)))

#define tonumber(o, n) (ttype(o) == LUA_TNUMBER || (((o) = luaV_tonumber(o, n)) != NULL))

#define equalobj(L, o1, o2) (ttype(o1) == ttype(o2) && luaV_equalval(L, o1, o2))

LUAI_FUNC int luaV_strcmp(const tstring_t* ls, const tstring_t* rs);
LUAI_FUNC int luaV_lessthan(lua_State* L, const tvalue_t* l, const tvalue_t* r);
LUAI_FUNC int luaV_lessequal(lua_State* L, const tvalue_t* l, const tvalue_t* r);
LUAI_FUNC int luaV_equalval(lua_State* L, const tvalue_t* t1, const tvalue_t* t2);

LUAI_FUNC void luaV_doarithimpl(lua_State* L, StkId ra, const tvalue_t* rb, const tvalue_t* rc, tag_method_t op);

LUAI_FUNC void luaV_dolen(lua_State* L, StkId ra, const tvalue_t* rb);
LUAI_FUNC const tvalue_t* luaV_tonumber(const tvalue_t* obj, tvalue_t* n);
LUAI_FUNC const float* luaV_tovector(const tvalue_t* obj);
LUAI_FUNC int luaV_tostring(lua_State* L, StkId obj);
LUAI_FUNC void luaV_gettable(lua_State* L, const tvalue_t* t, tvalue_t* key, StkId val);
LUAI_FUNC void luaV_settable(lua_State* L, const tvalue_t* t, tvalue_t* key, StkId val);
LUAI_FUNC void luaV_concat(lua_State* L, int total, int last);
LUAI_FUNC void luaV_getimport(lua_State* L, lua_table_t* env, tvalue_t* k, StkId res, uint32_t id, bool propagatenil);
LUAI_FUNC void luaV_prepareFORN(lua_State* L, StkId plimit, StkId pstep, StkId pinit);
LUAI_FUNC void luaV_callTM(lua_State* L, int nparams, int res);
LUAI_FUNC void luaV_tryfuncTM(lua_State* L, StkId func);

LUAI_FUNC void luau_execute(lua_State* L);
LUAI_FUNC void luau_finishop(lua_State* L);
LUAI_FUNC int luau_precall(lua_State* L, struct tvalue_t* func, int nresults);
LUAI_FUNC void luau_poscall(lua_State* L, StkId first);
LUAI_FUNC void luau_callhook(lua_State* L, lua_Hook hook, void* userdata);

#endif
