// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details

#include "lapi.h"
#include "lobject.h"
#include "lua.h"
#include "lualib.h"
#include "lstate.h"


static int __class_isinstance(lua_State* L)
{
    luaL_checkany(L, 1);
    luaL_checktype(L, 2, LUA_TCLASS);
    const tvalue_t* inst = luaA_toobject(L, 1);
    const tvalue_t* obj = luaA_toobject(L, 2);
    const luauc_class_t* lclass = classvalue(obj);
    bool isInstance = ttisobject(inst) && objectvalue(inst)->lclass == lclass;
    lua_pushboolean(L, isInstance);
    return 1;
}

static int __class_classof(lua_State* L)
{
    luaL_checkany(L, 1);
    if (!lua_isobject(L, 1))
    {
        lua_pushnil(L);
        return 1;
    }
    const tvalue_t* inst = luaA_toobject(L, 1);
    const luauc_object_t* ci = objectvalue(inst);
    luaA_pushclass(L, ci->lclass);
    return 1;
}

static const luaL_Reg __classlib[] = {
    {"isinstance", __class_isinstance},
    {"classof", __class_classof},
    {NULL, NULL},
};

/*
** Open class library
*/
int luaopen_class(lua_State* L)
{
    luaL_register(L, LUA_CLASSLIBNAME, __classlib);
    return 1;
}
