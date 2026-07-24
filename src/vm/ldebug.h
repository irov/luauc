// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#ifndef LUAUC_LDEBUG_H
#define LUAUC_LDEBUG_H

#include "lstate.h"

#define pcRel(pc, p) ((pc) && (pc) != (p)->code ? cast_to(int, (pc) - (p)->code) - 1 : 0)

#define luaG_typeerror(L, o, opname) luaG_typeerrorL(L, o, opname)
#define luaG_forerror(L, o, what) luaG_forerrorL(L, o, what)
#define luaG_runerror(L, ...) luaG_runerrorL(L, __VA_ARGS__)

#define LUA_MEMERRMSG "not enough memory"
#define LUA_ERRERRMSG "error in error handling"

LUA_NORETURN LUAI_FUNC void luaG_typeerrorL(lua_State* L, const tvalue_t* o, const char* opname);
LUA_NORETURN LUAI_FUNC void luaG_forerrorL(lua_State* L, const tvalue_t* o, const char* what);
LUA_NORETURN LUAI_FUNC void luaG_concaterror(lua_State* L, StkId p1, StkId p2);
LUA_NORETURN LUAI_FUNC void luaG_aritherror(lua_State* L, const tvalue_t* p1, const tvalue_t* p2, tag_method_t op);
LUA_NORETURN LUAI_FUNC void luaG_ordererror(lua_State* L, const tvalue_t* p1, const tvalue_t* p2, tag_method_t op);
LUA_NORETURN LUAI_FUNC void luaG_indexerror(lua_State* L, const tvalue_t* p1, const tvalue_t* p2);
LUA_NORETURN LUAI_FUNC void luaG_methoderror(lua_State* L, const tvalue_t* p1, const tstring_t* p2);
LUA_NORETURN LUAI_FUNC void luaG_missingmembererror(lua_State* L, const tvalue_t* p1, const tvalue_t* p2);
LUA_NORETURN LUAI_FUNC void luaG_readonlyerror(lua_State* L);

LUA_NORETURN LUAI_FUNC LUA_PRINTF_ATTR(2, 3) void luaG_runerrorL(lua_State* L, const char* fmt, ...);
LUAI_FUNC void luaG_pusherror(lua_State* L, const char* error);

LUAI_FUNC void luaG_breakpoint(lua_State* L, proto_t* p, int line, bool enable);
LUAI_FUNC bool luaG_onbreak(lua_State* L);

LUAI_FUNC int luaG_getline(proto_t* p, int pc);

LUAI_FUNC int luaG_isnative(lua_State* L, int level);
LUAI_FUNC int luaG_hasnative(lua_State* L, int level);

#endif
