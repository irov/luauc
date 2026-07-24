// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#ifndef LUAUC_COMPILER_H
#define LUAUC_COMPILER_H

#include "ast/luauc_parser.h"
#include "luacode.h"

typedef struct luauc_compile_result_t
{
    unsigned char* bytecode;
    size_t bytecode_size;
    int has_error;
    luauc_location_t error_location;
    char* error_message;
} luauc_compile_result_t;

int luauc_compile_tree(
    luauc_ast_stat_t* root,
    const luauc_array_t* hot_comments,
    const luauc_name_table_t* names,
    const lua_CompileOptions* options,
    luauc_allocator_t allocator,
    luauc_compile_result_t* result
);

void luauc_compile_result_destroy(luauc_compile_result_t* result, luauc_allocator_t allocator);

#endif
