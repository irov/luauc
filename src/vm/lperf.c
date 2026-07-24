// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// This code is based on Lua 5.x implementation licensed under MIT License; see lua_LICENSE.txt for details
#include "lua.h"

#include <time.h>

double lua_clock(void)
{
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) == TIME_UTC)
        return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;

    return (double)clock() / (double)CLOCKS_PER_SEC;
}
