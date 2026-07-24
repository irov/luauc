// This file is part of the luauc C port of the Luau programming language
// and is licensed under the MIT License; see LICENSE.txt for details.
#include "lua.h"
#include "lualib.h"

#include <stdio.h>
#include <stdlib.h>

static int __fail_with_lua_error(lua_State* L, const char* operation)
{
    const char* message = lua_tostring(L, -1);
    fprintf(stderr, "%s: %s\n", operation, message ? message : "(non-string error)");
    return 1;
}

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s bytecode-file\n", argv[0]);
        return 2;
    }

    FILE* file = fopen(argv[1], "rb");
    if (!file)
    {
        perror(argv[1]);
        return 2;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return 2;
    }

    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return 2;
    }

    char* bytecode = (char*)malloc((size_t)length);
    if (!bytecode)
    {
        fclose(file);
        return 2;
    }

    if (fread(bytecode, 1, (size_t)length, file) != (size_t)length)
    {
        free(bytecode);
        fclose(file);
        return 2;
    }
    fclose(file);

    lua_State* L = luaL_newstate();
    if (!L)
    {
        free(bytecode);
        return 2;
    }

    luaL_openlibs(L);

    int result = luau_load(L, "=vm_smoke.luau", bytecode, (size_t)length, 0);
    free(bytecode);
    if (result != 0)
    {
        result = __fail_with_lua_error(L, "luau_load");
        lua_close(L);
        return result;
    }

    result = lua_pcall(L, 0, 0, 0);
    if (result != 0)
    {
        result = __fail_with_lua_error(L, "lua_pcall");
        lua_close(L);
        return result;
    }

    lua_close(L);
    return 0;
}
