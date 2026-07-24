// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "luacode.h"

#include "ast/luauc_parser.h"
#include "bytecode/luauc_bytecode_builder.h"
#include "compiler/luauc_compile_constant.h"
#include "compiler/luauc_compiler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char* __luauc_make_formatted_error(
    unsigned int line, const char* message, size_t* outsize
)
{
    int length = snprintf(NULL, 0, ":%u: %s", line, message);
    char* buffer;
    unsigned char* result;
    if (length < 0)
        return NULL;
    buffer = (char*)malloc((size_t)length + 1);
    if (buffer == NULL)
        return NULL;
    snprintf(buffer, (size_t)length + 1, ":%u: %s", line, message);
    result = luauc_bytecode_get_error(buffer, (size_t)length, outsize);
    free(buffer);
    return result;
}

char* luau_compile(const char* source, size_t size, lua_CompileOptions* options, size_t* outsize)
{
    luauc_allocator_t allocator = luauc_default_allocator();
    luauc_arena_t arena;
    luauc_name_table_t names;
    luauc_parse_result_t parse;
    luauc_compile_result_t compiled;
    unsigned char* result = NULL;

    if (outsize == NULL || (source == NULL && size != 0))
        return NULL;
    *outsize = 0;
    luauc_arena_init(&arena, 16384, allocator);
    if (!luauc_name_table_init(&names, &arena, allocator))
    {
        luauc_arena_destroy(&arena);
        return NULL;
    }
    if (!luauc_parse(source != NULL ? source : "", size, &arena, &names, allocator, &parse))
        goto cleanup;
    if (parse.has_error)
    {
        result = __luauc_make_formatted_error(
            parse.error_location.begin.line + 1,
            parse.error_message != NULL ? parse.error_message : "Out of memory",
            outsize
        );
        goto cleanup;
    }
    if (!luauc_compile_tree(
            parse.root, &parse.hot_comments, &names, options, allocator, &compiled
        ))
        goto cleanup;
    if (compiled.has_error)
    {
        result = __luauc_make_formatted_error(
            compiled.error_location.begin.line + 1,
            compiled.error_message != NULL ? compiled.error_message : "Out of memory",
            outsize
        );
        luauc_compile_result_destroy(&compiled, allocator);
        goto cleanup;
    }
    result = compiled.bytecode;
    *outsize = compiled.bytecode_size;
    compiled.bytecode = NULL;
    compiled.bytecode_size = 0;
    luauc_compile_result_destroy(&compiled, allocator);

cleanup:
    luauc_name_table_destroy(&names);
    luauc_arena_destroy(&arena);
    return (char*)result;
}

void luau_set_compile_constant_nil(lua_CompileConstant* constant)
{
    luauc_compile_constant_value_t* value = (luauc_compile_constant_value_t*)constant;
    if (value != NULL)
        value->type = LUAUC_COMPILE_CONSTANT_NIL;
}

void luau_set_compile_constant_boolean(lua_CompileConstant* constant, int boolean_value)
{
    luauc_compile_constant_value_t* value = (luauc_compile_constant_value_t*)constant;
    if (value != NULL)
    {
        value->type = LUAUC_COMPILE_CONSTANT_BOOLEAN;
        value->value.boolean_value = boolean_value != 0;
    }
}

void luau_set_compile_constant_number(lua_CompileConstant* constant, double number)
{
    luauc_compile_constant_value_t* value = (luauc_compile_constant_value_t*)constant;
    if (value != NULL)
    {
        value->type = LUAUC_COMPILE_CONSTANT_NUMBER;
        value->value.number_value = number;
    }
}

void luau_set_compile_constant_integer64(lua_CompileConstant* constant, int64_t integer)
{
    luauc_compile_constant_value_t* value = (luauc_compile_constant_value_t*)constant;
    if (value != NULL)
    {
        value->type = LUAUC_COMPILE_CONSTANT_INTEGER;
        value->value.integer_value = integer;
    }
}

void luau_set_compile_constant_vector(
    lua_CompileConstant* constant, float x, float y, float z, float w
)
{
    luauc_compile_constant_value_t* value = (luauc_compile_constant_value_t*)constant;
    if (value != NULL)
    {
        value->type = LUAUC_COMPILE_CONSTANT_VECTOR;
        value->value.vector_value[0] = x;
        value->value.vector_value[1] = y;
        value->value.vector_value[2] = z;
        value->value.vector_value[3] = w;
    }
}

void luau_set_compile_constant_string(lua_CompileConstant* constant, const char* string, size_t length)
{
    luauc_compile_constant_value_t* value = (luauc_compile_constant_value_t*)constant;
    if (value != NULL)
    {
        value->type = LUAUC_COMPILE_CONSTANT_STRING;
        value->value.string_value.data = string;
        value->value.string_value.size = length;
    }
}
