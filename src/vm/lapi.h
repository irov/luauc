// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#ifndef LUAUC_LAPI_H
#define LUAUC_LAPI_H

#include "lobject.h"

LUAI_FUNC const tvalue_t* luaA_toobject(lua_State* L, int idx);
LUAI_FUNC void luaA_pushvalue(lua_State* L, const tvalue_t* o);
LUAI_FUNC void luaA_pushclass(lua_State* L, luauc_class_t* lclass);

#endif
