// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#ifndef LUAUC_LEXER_H
#define LUAUC_LEXER_H

#include "ast/luauc_ast.h"

typedef enum luauc_token_type_t
{
    LUAUC_TOKEN_EOF = 0,
    LUAUC_TOKEN_CHAR_END = 256,

    LUAUC_TOKEN_EQUAL,
    LUAUC_TOKEN_LESS_EQUAL,
    LUAUC_TOKEN_GREATER_EQUAL,
    LUAUC_TOKEN_NOT_EQUAL,
    LUAUC_TOKEN_DOT2,
    LUAUC_TOKEN_DOT3,
    LUAUC_TOKEN_SKINNY_ARROW,
    LUAUC_TOKEN_DOUBLE_COLON,
    LUAUC_TOKEN_FLOOR_DIV,

    LUAUC_TOKEN_INTERP_STRING_BEGIN,
    LUAUC_TOKEN_INTERP_STRING_MID,
    LUAUC_TOKEN_INTERP_STRING_END,
    LUAUC_TOKEN_INTERP_STRING_SIMPLE,

    LUAUC_TOKEN_ADD_ASSIGN,
    LUAUC_TOKEN_SUB_ASSIGN,
    LUAUC_TOKEN_MUL_ASSIGN,
    LUAUC_TOKEN_DIV_ASSIGN,
    LUAUC_TOKEN_FLOOR_DIV_ASSIGN,
    LUAUC_TOKEN_MOD_ASSIGN,
    LUAUC_TOKEN_POW_ASSIGN,
    LUAUC_TOKEN_CONCAT_ASSIGN,

    LUAUC_TOKEN_RAW_STRING,
    LUAUC_TOKEN_QUOTED_STRING,
    LUAUC_TOKEN_NUMBER,
    LUAUC_TOKEN_NAME,
    LUAUC_TOKEN_COMMENT,
    LUAUC_TOKEN_BLOCK_COMMENT,
    LUAUC_TOKEN_ATTRIBUTE,
    LUAUC_TOKEN_ATTRIBUTE_OPEN,
    LUAUC_TOKEN_BROKEN_STRING,
    LUAUC_TOKEN_BROKEN_COMMENT,
    LUAUC_TOKEN_BROKEN_UNICODE,
    LUAUC_TOKEN_BROKEN_INTERP_DOUBLE_BRACE,
    LUAUC_TOKEN_ERROR,

    LUAUC_TOKEN_RESERVED_BEGIN,
    LUAUC_TOKEN_AND = LUAUC_TOKEN_RESERVED_BEGIN,
    LUAUC_TOKEN_BREAK,
    LUAUC_TOKEN_DO,
    LUAUC_TOKEN_ELSE,
    LUAUC_TOKEN_ELSEIF,
    LUAUC_TOKEN_END,
    LUAUC_TOKEN_FALSE,
    LUAUC_TOKEN_FOR,
    LUAUC_TOKEN_FUNCTION,
    LUAUC_TOKEN_IF,
    LUAUC_TOKEN_IN,
    LUAUC_TOKEN_LOCAL,
    LUAUC_TOKEN_NIL,
    LUAUC_TOKEN_NOT,
    LUAUC_TOKEN_OR,
    LUAUC_TOKEN_REPEAT,
    LUAUC_TOKEN_RETURN,
    LUAUC_TOKEN_THEN,
    LUAUC_TOKEN_TRUE,
    LUAUC_TOKEN_UNTIL,
    LUAUC_TOKEN_WHILE,
    LUAUC_TOKEN_RESERVED_END
} luauc_token_type_t;

typedef enum luauc_quote_style_t
{
    LUAUC_QUOTE_SINGLE,
    LUAUC_QUOTE_DOUBLE
} luauc_quote_style_t;

typedef struct luauc_token_t
{
    luauc_token_type_t type;
    luauc_location_t location;
    unsigned int length;
    union
    {
        const char* data;
        const char* name;
        unsigned int codepoint;
    } value;
} luauc_token_t;

typedef struct luauc_name_entry_t
{
    luauc_name_t name;
    uint32_t length;
    luauc_token_type_t type;
} luauc_name_entry_t;

typedef struct luauc_name_table_t
{
    luauc_arena_t* arena;
    luauc_vector_t entries;
    luauc_string_map_t indices;
} luauc_name_table_t;

typedef enum luauc_brace_type_t
{
    LUAUC_BRACE_INTERPOLATED_STRING,
    LUAUC_BRACE_NORMAL
} luauc_brace_type_t;

typedef struct luauc_lexer_t
{
    const char* buffer;
    size_t buffer_size;
    unsigned int offset;
    unsigned int line;
    unsigned int line_offset;
    luauc_token_t token;
    luauc_location_t previous_location;
    luauc_name_table_t* names;
    int skip_comments;
    int read_names;
    luauc_vector_t brace_stack;
    int failed;
} luauc_lexer_t;

int luauc_name_table_init(luauc_name_table_t* table, luauc_arena_t* arena, luauc_allocator_t allocator);
void luauc_name_table_destroy(luauc_name_table_t* table);
luauc_name_t luauc_name_table_add(luauc_name_table_t* table, const char* name, size_t length, luauc_token_type_t* type);
luauc_name_t luauc_name_table_get(const luauc_name_table_t* table, const char* name, size_t length, luauc_token_type_t* type);

int luauc_lexer_init(
    luauc_lexer_t* lexer,
    const char* buffer,
    size_t buffer_size,
    luauc_name_table_t* names,
    luauc_position_t start_position,
    luauc_allocator_t allocator
);
void luauc_lexer_destroy(luauc_lexer_t* lexer);
const luauc_token_t* luauc_lexer_next(luauc_lexer_t* lexer);
const luauc_token_t* luauc_lexer_next_options(luauc_lexer_t* lexer, int skip_comments, int update_previous_location);
luauc_token_t luauc_lexer_lookahead(luauc_lexer_t* lexer);
void luauc_lexer_next_line(luauc_lexer_t* lexer);
unsigned int luauc_token_get_block_depth(const luauc_token_t* token);
luauc_quote_style_t luauc_token_get_quote_style(const luauc_token_t* token);
const char* luauc_token_name(luauc_token_type_t type);

int luauc_fixup_quoted_string(char* data, size_t* size);
void luauc_fixup_multiline_string(char* data, size_t* size);

#endif
