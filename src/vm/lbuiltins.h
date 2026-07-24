// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#ifndef LUAUC_LBUILTINS_H
#define LUAUC_LBUILTINS_H

#include "lobject.h"

typedef int (*luau_FastFunction)(lua_State* L, StkId res, tvalue_t* arg0, int nresults, StkId args, int nparams);

extern const luau_FastFunction luauF_table[256];

#endif
