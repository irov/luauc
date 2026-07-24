// This file is part of the luauc C port of the Luau programming language
// and is licensed under the MIT License; see LICENSE.txt for details.
#include "luauc_runtime.h"

luauc_assert_handler_t luauc_assert_handler = NULL;

int luauc_assert_call_handler(const char* expression, const char* file, int line, const char* function)
{
    if (luauc_assert_handler != NULL)
        return luauc_assert_handler(expression, file, line, function);

    return 1;
}
