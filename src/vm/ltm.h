// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#ifndef LUAUC_LTM_H
#define LUAUC_LTM_H

#include "lobject.h"

/*
 * WARNING: if you change the order of this enumeration,
 * grep "ORDER TM"
 */
// clang-format off
typedef enum
{
    TM_INDEX,
    TM_NEWINDEX,
    TM_MODE,
    TM_NAMECALL,
    TM_CALL,
    TM_ITER,
    TM_LEN,

    TM_EQ, // last tag method with `fast' access

    TM_ADD,
    TM_SUB,
    TM_MUL,
    TM_DIV,
    TM_IDIV,
    TM_MOD,
    TM_POW,
    TM_UNM,

    TM_LT,
    TM_LE,
    TM_CONCAT,
    TM_TYPE,
    TM_METATABLE,

    TM_N // number of elements in the enum
} tag_method_t;
// clang-format on

#define gfasttm(g, et, e) ((et) == NULL ? NULL : ((et)->tmcache & (1u << (e))) ? NULL : luaT_gettm(et, e, (g)->tmname[e]))

#define fasttm(l, et, e) gfasttm(l->global, et, e)
#define fastnotm(et, e) ((et) == NULL || ((et)->tmcache & (1u << (e))))

LUAI_DATA const char* const luaT_typenames[];
LUAI_DATA const char* const luaT_eventname[];

LUAI_FUNC const tvalue_t* luaT_gettm(lua_table_t* events, tag_method_t event, tstring_t* ename);
LUAI_FUNC const tvalue_t* luaT_gettmbyobj(lua_State* L, const tvalue_t* o, tag_method_t event);

LUAI_FUNC const tstring_t* luaT_objtypenamestr(lua_State* L, const tvalue_t* o);
LUAI_FUNC const char* luaT_objtypename(lua_State* L, const tvalue_t* o);

LUAI_FUNC void luaT_init(lua_State* L);

#endif
