// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#ifndef LUAUC_PARSER_H
#define LUAUC_PARSER_H

#include "ast/luauc_ast.h"
#include "ast/luauc_lexer.h"

typedef struct luauc_hot_comment_t
{
    int header;
    luauc_location_t location;
    struct
    {
        const char* data;
        size_t length;
    } text;
} luauc_hot_comment_t;

typedef struct luauc_parse_result_t
{
    luauc_ast_stat_t* root;
    size_t lines;
    luauc_array_t hot_comments;
    int has_error;
    luauc_location_t error_location;
    const char* error_message;
} luauc_parse_result_t;

int luauc_parse(
    const char* source,
    size_t size,
    luauc_arena_t* arena,
    luauc_name_table_t* names,
    luauc_allocator_t allocator,
    luauc_parse_result_t* result
);

#endif
