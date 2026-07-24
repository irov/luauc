// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#ifndef LUAUC_LUDATA_H
#define LUAUC_LUDATA_H

#include "lobject.h"

// special tag value is used for user data with inline dtors
#define UTAG_IDTOR LUA_UTAG_LIMIT

// special tag value is used for newproxy-created user data (all other user data objects are host-exposed)
#define UTAG_PROXY (LUA_UTAG_LIMIT + 1)

// must be updated if more internal tags are added
#define UTAG_INTERNAL_LIMIT (UTAG_PROXY + 1)

// userdata larger than 16 bytes will be extended to guarantee 16 byte alignment of subsequent blocks
#define sizeudata(len) (offsetof(udata_t, data) + (len > 16 ? ((len + 15) & ~15) : len))

LUAI_FUNC udata_t* luaU_newudata(lua_State* L, size_t s, int tag);
LUAI_FUNC void luaU_freeudata(lua_State* L, udata_t* u, struct lua_page_t* page);

#endif
