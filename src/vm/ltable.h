// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#ifndef LUAUC_LTABLE_H
#define LUAUC_LTABLE_H

#include "lobject.h"

#define gnode(t, i) (&(t)->node[i])
#define gkey(n) (&(n)->key)
#define gval(n) (&(n)->val)
#define gnext(n) ((n)->key.next)

#define gval2slot(t, v) (int)(cast_to(lua_node_t*, ((const tvalue_t*)(v))) - t->node)

// reset cache of absent metamethods, cache is updated in luaT_gettm
#define invalidateTMcache(t) t->tmcache = 0

LUAI_FUNC const tvalue_t* luaH_getnum(lua_table_t* t, int key);
LUAI_FUNC tvalue_t* luaH_setnum(lua_State* L, lua_table_t* t, int key);
LUAI_FUNC const tvalue_t* luaH_getstr(lua_table_t* t, tstring_t* key);
LUAI_FUNC tvalue_t* luaH_setstr(lua_State* L, lua_table_t* t, tstring_t* key);
LUAI_FUNC const tvalue_t* luaH_getp(lua_table_t* t, void* key, int tag);
LUAI_FUNC tvalue_t* luaH_setp(lua_State* L, lua_table_t* t, void* key, int tag);
LUAI_FUNC const tvalue_t* luaH_get(lua_table_t* t, const tvalue_t* key);
LUAI_FUNC tvalue_t* luaH_set(lua_State* L, lua_table_t* t, const tvalue_t* key);
LUAI_FUNC tvalue_t* luaH_newkey(lua_State* L, lua_table_t* t, const tvalue_t* key);
LUAI_FUNC lua_table_t* luaH_new(lua_State* L, int narray, int lnhash);
LUAI_FUNC void luaH_resizearray(lua_State* L, lua_table_t* t, int nasize);
LUAI_FUNC void luaH_resizehash(lua_State* L, lua_table_t* t, int nhsize);
LUAI_FUNC void luaH_free(lua_State* L, lua_table_t* t, struct lua_page_t* page);
LUAI_FUNC int luaH_next(lua_State* L, lua_table_t* t, StkId key);
LUAI_FUNC int luaH_getn(lua_table_t* t);
LUAI_FUNC lua_table_t* luaH_clone(lua_State* L, lua_table_t* tt);
LUAI_FUNC void luaH_clear(lua_table_t* tt);

#define luaH_setslot(L, t, slot, key) (invalidateTMcache(t), (slot == luaO_nilobject ? luaH_newkey(L, t, key) : cast_to(tvalue_t*, slot)))

extern const lua_node_t luaH_dummynode;

#endif
