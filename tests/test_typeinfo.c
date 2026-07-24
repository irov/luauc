// This file is part of the luauc C port of the Luau programming language
// and is licensed under the MIT License; see LICENSE.txt for details.
#include "lua.h"
#include "luacode.h"
#include "lualib.h"

#include "luauc_bytecode.h"
#include "lobject.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t __read_varuint(const unsigned char* data, size_t size, size_t* offset)
{
    uint32_t result = 0;
    unsigned int shift = 0;

    while (*offset < size && shift < 32)
    {
        unsigned char byte = data[(*offset)++];
        result |= (uint32_t)(byte & 127u) << shift;
        if ((byte & 128u) == 0)
            return result;
        shift += 7;
    }
    return UINT32_MAX;
}

static lua_State* __compile_and_run(
    const char* source,
    const char* const* userdata_types,
    const char* vector_type
)
{
    lua_CompileOptions options;
    lua_State* state;
    char* bytecode;
    size_t bytecode_size = 0;
    int status;

    memset(&options, 0, sizeof(options));
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    options.typeInfoLevel = 1;
    options.userdataTypes = userdata_types;
    options.vectorType = vector_type;
    bytecode = luau_compile(source, strlen(source), &options, &bytecode_size);
    if (bytecode == NULL || bytecode_size == 0 || (unsigned char)bytecode[0] == 0)
    {
        free(bytecode);
        return NULL;
    }
    state = luaL_newstate();
    if (state == NULL)
    {
        free(bytecode);
        return NULL;
    }
    status = luau_load(state, "=typeinfo", bytecode, bytecode_size, 0);
    free(bytecode);
    if (status == 0)
        status = lua_pcall(state, 0, 1, 0);
    if (status != 0 || lua_type(state, -1) != LUA_TFUNCTION)
    {
        lua_close(state);
        return NULL;
    }
    return state;
}

static const proto_t* __result_proto(lua_State* state)
{
    const closure_t* closure = (const closure_t*)lua_topointer(state, -1);
    return closure != NULL && !closure->isC ? closure->l.p : NULL;
}

static int __test_function_arguments(void)
{
    static const unsigned char __expected[] = {
        4, 0, 0, LBC_TYPE_FUNCTION, 2, LBC_TYPE_NUMBER,
        LBC_TYPE_STRING | LBC_TYPE_OPTIONAL_BIT
    };
    lua_State* state = __compile_and_run(
        "return function(x: number, y: string?) return x end", NULL, NULL
    );
    const proto_t* proto;
    int result;
    if (state == NULL)
        return 0;
    proto = __result_proto(state);
    result = proto != NULL && proto->sizetypeinfo == (int)sizeof(__expected) &&
        memcmp(proto->typeinfo, __expected, sizeof(__expected)) == 0;
    lua_close(state);
    return result;
}

static int __test_typed_local(void)
{
    lua_State* state = __compile_and_run(
        "return function(x: number)\n"
        "  local y: string = \"value\"\n"
        "  return x\n"
        "end",
        NULL,
        NULL
    );
    const proto_t* proto;
    int result;
    if (state == NULL)
        return 0;
    proto = __result_proto(state);
    result = proto != NULL && proto->sizetypeinfo >= 10 &&
        proto->typeinfo[0] == 3 && proto->typeinfo[1] == 0 &&
        proto->typeinfo[2] == 1 && proto->typeinfo[3] == LBC_TYPE_FUNCTION &&
        proto->typeinfo[4] == 1 && proto->typeinfo[5] == LBC_TYPE_NUMBER &&
        proto->typeinfo[6] == LBC_TYPE_STRING && proto->typeinfo[7] == 1;
    lua_close(state);
    return result;
}

static int __test_typed_upvalue(void)
{
    lua_State* state = __compile_and_run(
        "local x: number = 1\n"
        "return function() return x end",
        NULL,
        NULL
    );
    const proto_t* proto;
    static const unsigned char __expected[] = {0, 1, 0, LBC_TYPE_NUMBER};
    int result;
    if (state == NULL)
        return 0;
    proto = __result_proto(state);
    result = proto != NULL && proto->sizetypeinfo == (int)sizeof(__expected) &&
        memcmp(proto->typeinfo, __expected, sizeof(__expected)) == 0;
    lua_close(state);
    return result;
}

static int __test_configured_types(void)
{
    static const char* const __userdata_types[] = {"HostObject", NULL};
    static const unsigned char __userdata_expected[] = {
        3, 0, 0, LBC_TYPE_FUNCTION, 1, LBC_TYPE_USERDATA
    };
    static const unsigned char __vector_expected[] = {
        3, 0, 0, LBC_TYPE_FUNCTION, 1, LBC_TYPE_VECTOR
    };
    lua_State* state = __compile_and_run(
        "return function(x: HostObject) return x end",
        __userdata_types,
        NULL
    );
    const proto_t* proto;
    int result;
    if (state == NULL)
        return 0;
    proto = __result_proto(state);
    result = proto != NULL &&
        proto->sizetypeinfo == (int)sizeof(__userdata_expected) &&
        memcmp(proto->typeinfo, __userdata_expected, sizeof(__userdata_expected)) == 0;
    lua_close(state);
    if (!result)
        return 0;

    state = __compile_and_run(
        "return function(x: HostVector) return x end",
        NULL,
        "HostVector"
    );
    if (state == NULL)
        return 0;
    proto = __result_proto(state);
    result = proto != NULL &&
        proto->sizetypeinfo == (int)sizeof(__vector_expected) &&
        memcmp(proto->typeinfo, __vector_expected, sizeof(__vector_expected)) == 0;
    lua_close(state);
    return result;
}

static int __test_userdata_option_order(void)
{
    static const char* const __userdata_types[] = {
        "UnusedHostObject", "UsedHostObject", NULL
    };
    static const char __source[] =
        "local ignored = UnusedHostObject\n"
        "return function(x: UsedHostObject) return x end";
    lua_CompileOptions options;
    char* bytecode;
    size_t bytecode_size = 0;
    size_t offset = 2;
    uint32_t string_count;
    uint32_t index;
    int result = 0;

    memset(&options, 0, sizeof(options));
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    options.typeInfoLevel = 1;
    options.userdataTypes = __userdata_types;
    bytecode = luau_compile(
        __source, sizeof(__source) - 1, &options, &bytecode_size
    );
    if (bytecode == NULL || bytecode_size < 3 ||
        (unsigned char)bytecode[0] != LBC_VERSION_TARGET ||
        (unsigned char)bytecode[1] != LBC_TYPE_VERSION_TARGET)
        goto cleanup;

    string_count = __read_varuint(
        (const unsigned char*)bytecode, bytecode_size, &offset
    );
    if (string_count == UINT32_MAX)
        goto cleanup;
    for (index = 0; index < string_count; ++index)
    {
        uint32_t length = __read_varuint(
            (const unsigned char*)bytecode, bytecode_size, &offset
        );
        if (length == UINT32_MAX || length > bytecode_size - offset)
            goto cleanup;
        offset += length;
    }

    result = offset < bytecode_size && (unsigned char)bytecode[offset] == 2;
    if (!result)
        fprintf(
            stderr,
            "userdata mapping index mismatch at %zu: got %u, expected 2\n",
            offset,
            offset < bytecode_size ? (unsigned char)bytecode[offset] : 0u
        );

cleanup:
    free(bytecode);
    return result;
}

int main(void)
{
    if (!__test_function_arguments())
        return 1;
    if (!__test_typed_local())
        return 2;
    if (!__test_typed_upvalue())
        return 3;
    if (!__test_configured_types())
        return 4;
    if (!__test_userdata_option_order())
        return 5;
    return 0;
}
