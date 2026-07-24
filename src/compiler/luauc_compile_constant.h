// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#ifndef LUAUC_COMPILE_CONSTANT_H
#define LUAUC_COMPILE_CONSTANT_H

#include <stddef.h>
#include <stdint.h>

typedef enum luauc_compile_constant_type_t
{
    LUAUC_COMPILE_CONSTANT_UNKNOWN,
    LUAUC_COMPILE_CONSTANT_NIL,
    LUAUC_COMPILE_CONSTANT_BOOLEAN,
    LUAUC_COMPILE_CONSTANT_NUMBER,
    LUAUC_COMPILE_CONSTANT_INTEGER,
    LUAUC_COMPILE_CONSTANT_VECTOR,
    LUAUC_COMPILE_CONSTANT_STRING
} luauc_compile_constant_type_t;

typedef struct luauc_compile_constant_value_t
{
    luauc_compile_constant_type_t type;
    union
    {
        int boolean_value;
        double number_value;
        int64_t integer_value;
        float vector_value[4];
        struct
        {
            const char* data;
            size_t size;
        } string_value;
    } value;
} luauc_compile_constant_value_t;

#endif
