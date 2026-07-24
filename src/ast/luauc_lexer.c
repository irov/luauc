// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "ast/luauc_lexer.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

static const char* const __luauc_reserved_words[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in",
    "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"
};

_Static_assert(
    sizeof(__luauc_reserved_words) / sizeof(__luauc_reserved_words[0]) ==
        LUAUC_TOKEN_RESERVED_END - LUAUC_TOKEN_RESERVED_BEGIN,
    "reserved word table is out of sync"
);

static luauc_token_t __luauc_make_token(luauc_location_t location, luauc_token_type_t type)
{
    luauc_token_t token;
    memset(&token, 0, sizeof(token));
    token.type = type;
    token.location = location;
    return token;
}

static luauc_token_t __luauc_make_data_token(
    luauc_location_t location, luauc_token_type_t type, const char* data, size_t length
)
{
    luauc_token_t token = __luauc_make_token(location, type);
    token.value.data = data;
    token.length = (unsigned int)length;
    return token;
}

static luauc_token_t __luauc_make_name_token(luauc_location_t location, luauc_token_type_t type, const char* name)
{
    luauc_token_t token = __luauc_make_token(location, type);
    token.value.name = name;
    return token;
}

int luauc_name_table_init(luauc_name_table_t* table, luauc_arena_t* arena, luauc_allocator_t allocator)
{
    size_t index;
    if (table == NULL || arena == NULL)
        return 0;
    memset(table, 0, sizeof(*table));
    table->arena = arena;
    if (!luauc_vector_init(&table->entries, sizeof(luauc_name_entry_t), allocator))
        return 0;
    luauc_string_map_init(&table->indices, allocator);
    if (!luauc_string_map_reserve(&table->indices, 128))
        goto fail;

    for (index = 0; index < sizeof(__luauc_reserved_words) / sizeof(__luauc_reserved_words[0]); ++index)
    {
        luauc_name_entry_t entry;
        int inserted;
        const char* word = __luauc_reserved_words[index];
        entry.name.value = word;
        entry.length = (uint32_t)strlen(word);
        entry.type = (luauc_token_type_t)(LUAUC_TOKEN_RESERVED_BEGIN + index);
        if (luauc_vector_push(&table->entries, &entry) == NULL ||
            !luauc_string_map_insert(&table->indices, word, entry.length, table->entries.size, &inserted))
            goto fail;
    }
    return 1;

fail:
    luauc_name_table_destroy(table);
    return 0;
}

void luauc_name_table_destroy(luauc_name_table_t* table)
{
    if (table == NULL)
        return;
    luauc_vector_destroy(&table->entries);
    luauc_string_map_destroy(&table->indices);
    memset(table, 0, sizeof(*table));
}

luauc_name_t luauc_name_table_get(const luauc_name_table_t* table, const char* name, size_t length, luauc_token_type_t* type)
{
    size_t value;
    luauc_name_t result = {NULL};
    if (type != NULL)
        *type = LUAUC_TOKEN_NAME;
    if (table == NULL || !luauc_string_map_find(&table->indices, name, length, &value) || value == 0)
        return result;
    {
        const luauc_name_entry_t* entry =
            (const luauc_name_entry_t*)luauc_vector_at_const(&table->entries, value - 1);
        if (type != NULL)
            *type = entry->type;
        return entry->name;
    }
}

luauc_name_t luauc_name_table_add(luauc_name_table_t* table, const char* name, size_t length, luauc_token_type_t* type)
{
    luauc_name_t result = luauc_name_table_get(table, name, length, type);
    luauc_name_entry_t entry;
    char* copy;
    int inserted;

    if (result.value != NULL)
        return result;
    if (table == NULL || length > UINT32_MAX)
        return result;
    copy = luauc_arena_duplicate(table->arena, name, length);
    if (copy == NULL)
        return result;
    entry.name.value = copy;
    entry.length = (uint32_t)length;
    entry.type = length != 0 && name[0] == '@' ? LUAUC_TOKEN_ATTRIBUTE : LUAUC_TOKEN_NAME;
    if (luauc_vector_push(&table->entries, &entry) == NULL ||
        !luauc_string_map_insert(&table->indices, copy, length, table->entries.size, &inserted))
    {
        result.value = NULL;
        return result;
    }
    if (type != NULL)
        *type = entry.type;
    return entry.name;
}

static int __luauc_is_space(char character)
{
    return character == ' ' || character == '\t' || character == '\r' || character == '\n' ||
        character == '\v' || character == '\f';
}

static int __luauc_is_alpha(char character)
{
    return (unsigned int)((character | ' ') - 'a') < 26;
}

static int __luauc_is_digit(char character)
{
    return (unsigned int)(character - '0') < 10;
}

static int __luauc_is_hex_digit(char character)
{
    return __luauc_is_digit(character) || (unsigned int)((character | ' ') - 'a') < 6;
}

static char __luauc_unescape(char character)
{
    switch (character)
    {
    case 'a':
        return '\a';
    case 'b':
        return '\b';
    case 'f':
        return '\f';
    case 'n':
        return '\n';
    case 'r':
        return '\r';
    case 't':
        return '\t';
    case 'v':
        return '\v';
    default:
        return character;
    }
}

static char __luauc_lexer_peek(const luauc_lexer_t* lexer, unsigned int lookahead)
{
    size_t position = (size_t)lexer->offset + lookahead;
    return position < lexer->buffer_size ? lexer->buffer[position] : 0;
}

static luauc_position_t __luauc_lexer_position(const luauc_lexer_t* lexer)
{
    return __luauc_position_make(lexer->line, lexer->offset - lexer->line_offset);
}

static void __luauc_lexer_consume(luauc_lexer_t* lexer)
{
    assert(__luauc_lexer_peek(lexer, 0) != '\n');
    lexer->offset++;
}

static void __luauc_lexer_consume_any(luauc_lexer_t* lexer)
{
    if (__luauc_lexer_peek(lexer, 0) == '\n')
    {
        lexer->line++;
        lexer->line_offset = lexer->offset + 1;
    }
    lexer->offset++;
}

int luauc_lexer_init(
    luauc_lexer_t* lexer,
    const char* buffer,
    size_t buffer_size,
    luauc_name_table_t* names,
    luauc_position_t start_position,
    luauc_allocator_t allocator
)
{
    if (lexer == NULL || (buffer == NULL && buffer_size != 0) || names == NULL)
        return 0;
    memset(lexer, 0, sizeof(*lexer));
    lexer->buffer = buffer;
    lexer->buffer_size = buffer_size;
    lexer->line = start_position.line;
    lexer->line_offset = 0u - start_position.column;
    lexer->token = __luauc_make_token(__luauc_location_length(start_position, 0), LUAUC_TOKEN_EOF);
    lexer->names = names;
    lexer->read_names = 1;
    return luauc_vector_init(&lexer->brace_stack, sizeof(uint8_t), allocator);
}

void luauc_lexer_destroy(luauc_lexer_t* lexer)
{
    if (lexer == NULL)
        return;
    luauc_vector_destroy(&lexer->brace_stack);
    memset(lexer, 0, sizeof(*lexer));
}

static int __luauc_lexer_skip_long_separator(luauc_lexer_t* lexer)
{
    char start = __luauc_lexer_peek(lexer, 0);
    int count = 0;
    assert(start == '[' || start == ']');
    __luauc_lexer_consume(lexer);
    while (__luauc_lexer_peek(lexer, 0) == '=')
    {
        __luauc_lexer_consume(lexer);
        count++;
    }
    return start == __luauc_lexer_peek(lexer, 0) ? count : -count - 1;
}

static luauc_token_t __luauc_lexer_read_long_string(
    luauc_lexer_t* lexer,
    luauc_position_t start,
    int separator,
    luauc_token_type_t valid_type,
    luauc_token_type_t broken_type
)
{
    unsigned int start_offset;
    assert(__luauc_lexer_peek(lexer, 0) == '[');
    __luauc_lexer_consume(lexer);
    start_offset = lexer->offset;

    while (__luauc_lexer_peek(lexer, 0) != 0)
    {
        if (__luauc_lexer_peek(lexer, 0) == ']')
        {
            if (__luauc_lexer_skip_long_separator(lexer) == separator)
            {
                unsigned int end_offset;
                assert(__luauc_lexer_peek(lexer, 0) == ']');
                __luauc_lexer_consume(lexer);
                end_offset = lexer->offset - (unsigned int)separator - 2;
                return __luauc_make_data_token(
                    __luauc_location_make(start, __luauc_lexer_position(lexer)),
                    valid_type,
                    lexer->buffer + start_offset,
                    end_offset - start_offset
                );
            }
        }
        else
            __luauc_lexer_consume_any(lexer);
    }
    return __luauc_make_token(__luauc_location_make(start, __luauc_lexer_position(lexer)), broken_type);
}

static luauc_token_t __luauc_lexer_read_comment(luauc_lexer_t* lexer)
{
    luauc_position_t start = __luauc_lexer_position(lexer);
    size_t start_offset;
    assert(__luauc_lexer_peek(lexer, 0) == '-' && __luauc_lexer_peek(lexer, 1) == '-');
    __luauc_lexer_consume(lexer);
    __luauc_lexer_consume(lexer);
    start_offset = lexer->offset;
    if (__luauc_lexer_peek(lexer, 0) == '[')
    {
        int separator = __luauc_lexer_skip_long_separator(lexer);
        if (separator >= 0)
            return __luauc_lexer_read_long_string(
                lexer, start, separator, LUAUC_TOKEN_BLOCK_COMMENT, LUAUC_TOKEN_BROKEN_COMMENT
            );
    }
    while (__luauc_lexer_peek(lexer, 0) != 0 && __luauc_lexer_peek(lexer, 0) != '\r' &&
           __luauc_lexer_peek(lexer, 0) != '\n')
        __luauc_lexer_consume(lexer);
    return __luauc_make_data_token(
        __luauc_location_make(start, __luauc_lexer_position(lexer)),
        LUAUC_TOKEN_COMMENT,
        lexer->buffer + start_offset,
        lexer->offset - start_offset
    );
}

static void __luauc_lexer_read_backslash(luauc_lexer_t* lexer)
{
    assert(__luauc_lexer_peek(lexer, 0) == '\\');
    __luauc_lexer_consume(lexer);
    switch (__luauc_lexer_peek(lexer, 0))
    {
    case '\r':
        __luauc_lexer_consume(lexer);
        if (__luauc_lexer_peek(lexer, 0) == '\n')
            __luauc_lexer_consume_any(lexer);
        break;
    case 0:
        break;
    case 'z':
        __luauc_lexer_consume(lexer);
        while (__luauc_is_space(__luauc_lexer_peek(lexer, 0)))
            __luauc_lexer_consume_any(lexer);
        break;
    default:
        __luauc_lexer_consume_any(lexer);
    }
}

static luauc_token_t __luauc_lexer_read_quoted_string(luauc_lexer_t* lexer)
{
    luauc_position_t start = __luauc_lexer_position(lexer);
    char delimiter = __luauc_lexer_peek(lexer, 0);
    unsigned int start_offset;
    assert(delimiter == '\'' || delimiter == '"');
    __luauc_lexer_consume(lexer);
    start_offset = lexer->offset;
    while (__luauc_lexer_peek(lexer, 0) != delimiter)
    {
        switch (__luauc_lexer_peek(lexer, 0))
        {
        case 0:
        case '\r':
        case '\n':
            return __luauc_make_token(
                __luauc_location_make(start, __luauc_lexer_position(lexer)), LUAUC_TOKEN_BROKEN_STRING
            );
        case '\\':
            __luauc_lexer_read_backslash(lexer);
            break;
        default:
            __luauc_lexer_consume(lexer);
        }
    }
    __luauc_lexer_consume(lexer);
    return __luauc_make_data_token(
        __luauc_location_make(start, __luauc_lexer_position(lexer)),
        LUAUC_TOKEN_QUOTED_STRING,
        lexer->buffer + start_offset,
        lexer->offset - start_offset - 1
    );
}

static luauc_token_t __luauc_lexer_read_interpolated_section(
    luauc_lexer_t* lexer, luauc_position_t start, luauc_token_type_t format_type, luauc_token_type_t end_type
)
{
    unsigned int start_offset = lexer->offset;
    while (__luauc_lexer_peek(lexer, 0) != '`')
    {
        switch (__luauc_lexer_peek(lexer, 0))
        {
        case 0:
        case '\r':
        case '\n':
            return __luauc_make_token(
                __luauc_location_make(start, __luauc_lexer_position(lexer)), LUAUC_TOKEN_BROKEN_STRING
            );
        case '\\':
            if (__luauc_lexer_peek(lexer, 1) == 'u' && __luauc_lexer_peek(lexer, 2) == '{')
            {
                __luauc_lexer_consume(lexer);
                __luauc_lexer_consume(lexer);
                __luauc_lexer_consume(lexer);
            }
            else
                __luauc_lexer_read_backslash(lexer);
            break;
        case '{':
        {
            uint8_t brace = LUAUC_BRACE_INTERPOLATED_STRING;
            if (luauc_vector_push(&lexer->brace_stack, &brace) == NULL)
                lexer->failed = 1;
            if (__luauc_lexer_peek(lexer, 1) == '{')
            {
                luauc_token_t token = __luauc_make_data_token(
                    __luauc_location_make(start, __luauc_lexer_position(lexer)),
                    LUAUC_TOKEN_BROKEN_INTERP_DOUBLE_BRACE,
                    lexer->buffer + start_offset,
                    lexer->offset - start_offset
                );
                __luauc_lexer_consume(lexer);
                __luauc_lexer_consume(lexer);
                return token;
            }
            __luauc_lexer_consume(lexer);
            return __luauc_make_data_token(
                __luauc_location_make(start, __luauc_lexer_position(lexer)),
                format_type,
                lexer->buffer + start_offset,
                lexer->offset - start_offset - 1
            );
        }
        default:
            __luauc_lexer_consume(lexer);
        }
    }
    __luauc_lexer_consume(lexer);
    return __luauc_make_data_token(
        __luauc_location_make(start, __luauc_lexer_position(lexer)),
        end_type,
        lexer->buffer + start_offset,
        lexer->offset - start_offset - 1
    );
}

static luauc_token_t __luauc_lexer_read_interpolated_begin(luauc_lexer_t* lexer)
{
    luauc_position_t start = __luauc_lexer_position(lexer);
    assert(__luauc_lexer_peek(lexer, 0) == '`');
    __luauc_lexer_consume(lexer);
    return __luauc_lexer_read_interpolated_section(
        lexer, start, LUAUC_TOKEN_INTERP_STRING_BEGIN, LUAUC_TOKEN_INTERP_STRING_SIMPLE
    );
}

static luauc_token_t __luauc_lexer_read_number(luauc_lexer_t* lexer, luauc_position_t start, unsigned int start_offset)
{
    assert(__luauc_is_digit(__luauc_lexer_peek(lexer, 0)));
    do
        __luauc_lexer_consume(lexer);
    while (__luauc_is_digit(__luauc_lexer_peek(lexer, 0)) || __luauc_lexer_peek(lexer, 0) == '.' ||
           __luauc_lexer_peek(lexer, 0) == '_');
    if (__luauc_lexer_peek(lexer, 0) == 'e' || __luauc_lexer_peek(lexer, 0) == 'E')
    {
        __luauc_lexer_consume(lexer);
        if (__luauc_lexer_peek(lexer, 0) == '+' || __luauc_lexer_peek(lexer, 0) == '-')
            __luauc_lexer_consume(lexer);
    }
    while (__luauc_is_alpha(__luauc_lexer_peek(lexer, 0)) || __luauc_is_digit(__luauc_lexer_peek(lexer, 0)) ||
           __luauc_lexer_peek(lexer, 0) == '_')
        __luauc_lexer_consume(lexer);
    return __luauc_make_data_token(
        __luauc_location_make(start, __luauc_lexer_position(lexer)),
        LUAUC_TOKEN_NUMBER,
        lexer->buffer + start_offset,
        lexer->offset - start_offset
    );
}

static luauc_name_t __luauc_lexer_read_name(luauc_lexer_t* lexer, luauc_token_type_t* type)
{
    unsigned int start_offset = lexer->offset;
    assert(__luauc_is_alpha(__luauc_lexer_peek(lexer, 0)) || __luauc_lexer_peek(lexer, 0) == '_' ||
           __luauc_lexer_peek(lexer, 0) == '@');
    do
        __luauc_lexer_consume(lexer);
    while (__luauc_is_alpha(__luauc_lexer_peek(lexer, 0)) || __luauc_is_digit(__luauc_lexer_peek(lexer, 0)) ||
           __luauc_lexer_peek(lexer, 0) == '_');
    if (lexer->read_names)
        return luauc_name_table_add(
            lexer->names, lexer->buffer + start_offset, lexer->offset - start_offset, type
        );
    return luauc_name_table_get(
        lexer->names, lexer->buffer + start_offset, lexer->offset - start_offset, type
    );
}

static luauc_token_t __luauc_lexer_read_utf8_error(luauc_lexer_t* lexer)
{
    luauc_position_t start = __luauc_lexer_position(lexer);
    uint32_t codepoint = 0;
    int size = 0;
    int index;
    unsigned char character = (unsigned char)__luauc_lexer_peek(lexer, 0);

    if ((character & 0x80u) == 0)
    {
        size = 1;
        codepoint = character & 0x7fu;
    }
    else if ((character & 0xe0u) == 0xc0u)
    {
        size = 2;
        codepoint = character & 0x1fu;
    }
    else if ((character & 0xf0u) == 0xe0u)
    {
        size = 3;
        codepoint = character & 0x0fu;
    }
    else if ((character & 0xf8u) == 0xf0u)
    {
        size = 4;
        codepoint = character & 7u;
    }
    else
    {
        __luauc_lexer_consume(lexer);
        return __luauc_make_token(
            __luauc_location_make(start, __luauc_lexer_position(lexer)), LUAUC_TOKEN_BROKEN_UNICODE
        );
    }

    __luauc_lexer_consume(lexer);
    for (index = 1; index < size; ++index)
    {
        character = (unsigned char)__luauc_lexer_peek(lexer, 0);
        if ((character & 0xc0u) != 0x80u)
            return __luauc_make_token(
                __luauc_location_make(start, __luauc_lexer_position(lexer)), LUAUC_TOKEN_BROKEN_UNICODE
            );
        codepoint = (codepoint << 6) | (character & 0x3fu);
        __luauc_lexer_consume(lexer);
    }
    {
        luauc_token_t token = __luauc_make_token(
            __luauc_location_make(start, __luauc_lexer_position(lexer)), LUAUC_TOKEN_BROKEN_UNICODE
        );
        token.value.codepoint = codepoint;
        return token;
    }
}

static luauc_token_t __luauc_lexer_read_next(luauc_lexer_t* lexer)
{
    luauc_position_t start = __luauc_lexer_position(lexer);
    char character = __luauc_lexer_peek(lexer, 0);

    switch (character)
    {
    case 0:
        return __luauc_make_token(__luauc_location_length(start, 0), LUAUC_TOKEN_EOF);
    case '-':
        if (__luauc_lexer_peek(lexer, 1) == '>')
        {
            __luauc_lexer_consume(lexer);
            __luauc_lexer_consume(lexer);
            return __luauc_make_token(__luauc_location_length(start, 2), LUAUC_TOKEN_SKINNY_ARROW);
        }
        if (__luauc_lexer_peek(lexer, 1) == '=')
        {
            __luauc_lexer_consume(lexer);
            __luauc_lexer_consume(lexer);
            return __luauc_make_token(__luauc_location_length(start, 2), LUAUC_TOKEN_SUB_ASSIGN);
        }
        if (__luauc_lexer_peek(lexer, 1) == '-')
            return __luauc_lexer_read_comment(lexer);
        __luauc_lexer_consume(lexer);
        return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)'-');
    case '[':
    {
        int separator = __luauc_lexer_skip_long_separator(lexer);
        if (separator >= 0)
            return __luauc_lexer_read_long_string(
                lexer, start, separator, LUAUC_TOKEN_RAW_STRING, LUAUC_TOKEN_BROKEN_STRING
            );
        if (separator == -1)
            return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)'[');
        return __luauc_make_token(
            __luauc_location_make(start, __luauc_lexer_position(lexer)), LUAUC_TOKEN_BROKEN_STRING
        );
    }
    case '{':
    {
        __luauc_lexer_consume(lexer);
        if (lexer->brace_stack.size != 0)
        {
            uint8_t brace = LUAUC_BRACE_NORMAL;
            if (luauc_vector_push(&lexer->brace_stack, &brace) == NULL)
                lexer->failed = 1;
        }
        return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)'{');
    }
    case '}':
        __luauc_lexer_consume(lexer);
        if (lexer->brace_stack.size == 0)
            return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)'}');
        else
        {
            uint8_t brace = *(const uint8_t*)luauc_vector_at_const(
                &lexer->brace_stack, lexer->brace_stack.size - 1
            );
            lexer->brace_stack.size--;
            if (brace != LUAUC_BRACE_INTERPOLATED_STRING)
                return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)'}');
            return __luauc_lexer_read_interpolated_section(
                lexer, start, LUAUC_TOKEN_INTERP_STRING_MID, LUAUC_TOKEN_INTERP_STRING_END
            );
        }
    case '=':
        __luauc_lexer_consume(lexer);
        if (__luauc_lexer_peek(lexer, 0) == '=')
        {
            __luauc_lexer_consume(lexer);
            return __luauc_make_token(__luauc_location_length(start, 2), LUAUC_TOKEN_EQUAL);
        }
        return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)'=');
    case '<':
        __luauc_lexer_consume(lexer);
        if (__luauc_lexer_peek(lexer, 0) == '=')
        {
            __luauc_lexer_consume(lexer);
            return __luauc_make_token(__luauc_location_length(start, 2), LUAUC_TOKEN_LESS_EQUAL);
        }
        return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)'<');
    case '>':
        __luauc_lexer_consume(lexer);
        if (__luauc_lexer_peek(lexer, 0) == '=')
        {
            __luauc_lexer_consume(lexer);
            return __luauc_make_token(__luauc_location_length(start, 2), LUAUC_TOKEN_GREATER_EQUAL);
        }
        return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)'>');
    case '~':
        __luauc_lexer_consume(lexer);
        if (__luauc_lexer_peek(lexer, 0) == '=')
        {
            __luauc_lexer_consume(lexer);
            return __luauc_make_token(__luauc_location_length(start, 2), LUAUC_TOKEN_NOT_EQUAL);
        }
        return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)'~');
    case '"':
    case '\'':
        return __luauc_lexer_read_quoted_string(lexer);
    case '`':
        return __luauc_lexer_read_interpolated_begin(lexer);
    case '.':
        __luauc_lexer_consume(lexer);
        if (__luauc_lexer_peek(lexer, 0) == '.')
        {
            __luauc_lexer_consume(lexer);
            if (__luauc_lexer_peek(lexer, 0) == '.')
            {
                __luauc_lexer_consume(lexer);
                return __luauc_make_token(__luauc_location_length(start, 3), LUAUC_TOKEN_DOT3);
            }
            if (__luauc_lexer_peek(lexer, 0) == '=')
            {
                __luauc_lexer_consume(lexer);
                return __luauc_make_token(__luauc_location_length(start, 3), LUAUC_TOKEN_CONCAT_ASSIGN);
            }
            return __luauc_make_token(__luauc_location_length(start, 2), LUAUC_TOKEN_DOT2);
        }
        if (__luauc_is_digit(__luauc_lexer_peek(lexer, 0)))
            return __luauc_lexer_read_number(lexer, start, lexer->offset - 1);
        return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)'.');
    case '+':
    case '*':
    case '%':
    case '^':
    {
        luauc_token_type_t assign_type = character == '+' ? LUAUC_TOKEN_ADD_ASSIGN :
            character == '*' ? LUAUC_TOKEN_MUL_ASSIGN :
            character == '%' ? LUAUC_TOKEN_MOD_ASSIGN : LUAUC_TOKEN_POW_ASSIGN;
        __luauc_lexer_consume(lexer);
        if (__luauc_lexer_peek(lexer, 0) == '=')
        {
            __luauc_lexer_consume(lexer);
            return __luauc_make_token(__luauc_location_length(start, 2), assign_type);
        }
        return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)(unsigned char)character);
    }
    case '/':
        __luauc_lexer_consume(lexer);
        if (__luauc_lexer_peek(lexer, 0) == '=')
        {
            __luauc_lexer_consume(lexer);
            return __luauc_make_token(__luauc_location_length(start, 2), LUAUC_TOKEN_DIV_ASSIGN);
        }
        if (__luauc_lexer_peek(lexer, 0) == '/')
        {
            __luauc_lexer_consume(lexer);
            if (__luauc_lexer_peek(lexer, 0) == '=')
            {
                __luauc_lexer_consume(lexer);
                return __luauc_make_token(__luauc_location_length(start, 3), LUAUC_TOKEN_FLOOR_DIV_ASSIGN);
            }
            return __luauc_make_token(__luauc_location_length(start, 2), LUAUC_TOKEN_FLOOR_DIV);
        }
        return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)'/');
    case ':':
        __luauc_lexer_consume(lexer);
        if (__luauc_lexer_peek(lexer, 0) == ':')
        {
            __luauc_lexer_consume(lexer);
            return __luauc_make_token(__luauc_location_length(start, 2), LUAUC_TOKEN_DOUBLE_COLON);
        }
        return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)':');
    case '(':
    case ')':
    case ']':
    case ';':
    case ',':
    case '#':
    case '?':
    case '&':
    case '|':
        __luauc_lexer_consume(lexer);
        return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)(unsigned char)character);
    case '@':
        if (__luauc_lexer_peek(lexer, 1) == '[')
        {
            __luauc_lexer_consume(lexer);
            __luauc_lexer_consume(lexer);
            return __luauc_make_token(__luauc_location_length(start, 2), LUAUC_TOKEN_ATTRIBUTE_OPEN);
        }
        else
        {
            luauc_name_t name;
            luauc_token_type_t type;
            __luauc_lexer_consume(lexer);
            if (__luauc_is_alpha(__luauc_lexer_peek(lexer, 0)) || __luauc_lexer_peek(lexer, 0) == '_')
                name = __luauc_lexer_read_name(lexer, &type);
            else
                name.value = "";
            return __luauc_make_name_token(
                __luauc_location_make(start, __luauc_lexer_position(lexer)), LUAUC_TOKEN_ATTRIBUTE, name.value
            );
        }
    default:
        if (__luauc_is_digit(character))
            return __luauc_lexer_read_number(lexer, start, lexer->offset);
        if (__luauc_is_alpha(character) || character == '_')
        {
            luauc_token_type_t type;
            luauc_name_t name = __luauc_lexer_read_name(lexer, &type);
            if (name.value == NULL)
                lexer->failed = 1;
            return __luauc_make_name_token(
                __luauc_location_make(start, __luauc_lexer_position(lexer)), type, name.value
            );
        }
        if ((unsigned char)character & 0x80u)
            return __luauc_lexer_read_utf8_error(lexer);
        __luauc_lexer_consume(lexer);
        return __luauc_make_token(__luauc_location_length(start, 1), (luauc_token_type_t)(unsigned char)character);
    }
}

const luauc_token_t* luauc_lexer_next_options(luauc_lexer_t* lexer, int skip_comments, int update_previous_location)
{
    do
    {
        while (__luauc_is_space(__luauc_lexer_peek(lexer, 0)))
            __luauc_lexer_consume_any(lexer);
        if (update_previous_location)
            lexer->previous_location = lexer->token.location;
        lexer->token = __luauc_lexer_read_next(lexer);
        update_previous_location = 0;
    } while (skip_comments &&
             (lexer->token.type == LUAUC_TOKEN_COMMENT || lexer->token.type == LUAUC_TOKEN_BLOCK_COMMENT));
    return &lexer->token;
}

const luauc_token_t* luauc_lexer_next(luauc_lexer_t* lexer)
{
    return luauc_lexer_next_options(lexer, lexer->skip_comments, 1);
}

luauc_token_t luauc_lexer_lookahead(luauc_lexer_t* lexer)
{
    unsigned int offset = lexer->offset;
    unsigned int line = lexer->line;
    unsigned int line_offset = lexer->line_offset;
    luauc_token_t token = lexer->token;
    luauc_location_t previous = lexer->previous_location;
    size_t brace_size = lexer->brace_stack.size;
    uint8_t brace = brace_size != 0 ?
        *(const uint8_t*)luauc_vector_at_const(&lexer->brace_stack, brace_size - 1) : LUAUC_BRACE_NORMAL;
    luauc_token_t result = *luauc_lexer_next(lexer);

    lexer->offset = offset;
    lexer->line = line;
    lexer->line_offset = line_offset;
    lexer->token = token;
    lexer->previous_location = previous;
    if (lexer->brace_stack.size < brace_size)
        luauc_vector_push(&lexer->brace_stack, &brace);
    else if (lexer->brace_stack.size > brace_size)
        lexer->brace_stack.size--;
    return result;
}

void luauc_lexer_next_line(luauc_lexer_t* lexer)
{
    while (__luauc_lexer_peek(lexer, 0) != 0 && __luauc_lexer_peek(lexer, 0) != '\r' &&
           __luauc_lexer_peek(lexer, 0) != '\n')
        __luauc_lexer_consume(lexer);
    luauc_lexer_next(lexer);
}

unsigned int luauc_token_get_block_depth(const luauc_token_t* token)
{
    unsigned int depth = 0;
    assert(token != NULL && (token->type == LUAUC_TOKEN_RAW_STRING || token->type == LUAUC_TOKEN_BLOCK_COMMENT));
    do
        depth++;
    while (token->value.data[token->length + depth] != ']');
    return depth - 1;
}

luauc_quote_style_t luauc_token_get_quote_style(const luauc_token_t* token)
{
    assert(token != NULL && token->type == LUAUC_TOKEN_QUOTED_STRING);
    return token->value.data[token->length] == '\'' ? LUAUC_QUOTE_SINGLE : LUAUC_QUOTE_DOUBLE;
}

const char* luauc_token_name(luauc_token_type_t type)
{
    static const char* const __names[] = {
        "<eof>", "'=='", "'<='", "'>='", "'~='", "'..'", "'...'", "'->'", "'::'", "'//'",
        "interpolated string", "interpolated string", "interpolated string", "interpolated string",
        "'+='", "'-='", "'*='", "'/='", "'//='", "'%='", "'^='", "'..='",
        "string", "string", "number", "identifier", "comment", "comment", "attribute", "'@['",
        "malformed string", "unfinished comment", "Unicode character", "'{{'", "error"
    };
    if (type < LUAUC_TOKEN_CHAR_END)
    {
        switch ((unsigned int)type)
        {
        case '!': return "'!'";
        case '#': return "'#'";
        case '$': return "'$'";
        case '%': return "'%'";
        case '&': return "'&'";
        case '(': return "'('";
        case ')': return "')'";
        case '*': return "'*'";
        case '+': return "'+'";
        case ',': return "','";
        case '-': return "'-'";
        case '.': return "'.'";
        case '/': return "'/'";
        case ':': return "':'";
        case ';': return "';'";
        case '<': return "'<'";
        case '=': return "'='";
        case '>': return "'>'";
        case '?': return "'?'";
        case '@': return "'@'";
        case '[': return "'['";
        case '\\': return "'\\'";
        case ']': return "']'";
        case '^': return "'^'";
        case '`': return "'`'";
        case '{': return "'{'";
        case '|': return "'|'";
        case '}': return "'}'";
        case '~': return "'~'";
        default: return "character";
        }
    }
    if (type >= LUAUC_TOKEN_EQUAL && type <= LUAUC_TOKEN_ERROR)
        return __names[type - LUAUC_TOKEN_CHAR_END];
    if (type >= LUAUC_TOKEN_RESERVED_BEGIN && type < LUAUC_TOKEN_RESERVED_END)
        return __luauc_reserved_words[type - LUAUC_TOKEN_RESERVED_BEGIN];
    return "<unknown>";
}

static size_t __luauc_to_utf8(char* data, unsigned int code)
{
    if (code < 0x80)
    {
        data[0] = (char)code;
        return 1;
    }
    if (code < 0x800)
    {
        data[0] = (char)(0xc0u | (code >> 6));
        data[1] = (char)(0x80u | (code & 0x3fu));
        return 2;
    }
    if (code < 0x10000)
    {
        data[0] = (char)(0xe0u | (code >> 12));
        data[1] = (char)(0x80u | ((code >> 6) & 0x3fu));
        data[2] = (char)(0x80u | (code & 0x3fu));
        return 3;
    }
    if (code < 0x110000)
    {
        data[0] = (char)(0xf0u | (code >> 18));
        data[1] = (char)(0x80u | ((code >> 12) & 0x3fu));
        data[2] = (char)(0x80u | ((code >> 6) & 0x3fu));
        data[3] = (char)(0x80u | (code & 0x3fu));
        return 4;
    }
    return 0;
}

int luauc_fixup_quoted_string(char* data, size_t* size_pointer)
{
    size_t size;
    size_t write = 0;
    size_t index = 0;
    if (data == NULL || size_pointer == NULL)
        return 0;
    size = *size_pointer;
    while (index < size)
    {
        char escape;
        if (data[index] != '\\')
        {
            data[write++] = data[index++];
            continue;
        }
        if (index + 1 == size)
            return 0;
        escape = data[index + 1];
        index += 2;
        switch (escape)
        {
        case '\n':
            data[write++] = '\n';
            break;
        case '\r':
            data[write++] = '\n';
            if (index < size && data[index] == '\n')
                index++;
            break;
        case 0:
            return 0;
        case 'x':
        {
            unsigned int code = 0;
            int digit;
            if (index + 2 > size)
                return 0;
            for (digit = 0; digit < 2; ++digit)
            {
                char character = data[index + (size_t)digit];
                if (!__luauc_is_hex_digit(character))
                    return 0;
                code = 16 * code +
                    (unsigned int)(__luauc_is_digit(character) ? character - '0' : (character | ' ') - 'a' + 10);
            }
            data[write++] = (char)code;
            index += 2;
            break;
        }
        case 'z':
            while (index < size && __luauc_is_space(data[index]))
                index++;
            break;
        case 'u':
        {
            unsigned int code = 0;
            int digit;
            size_t utf8;
            if (index + 3 > size || data[index] != '{')
                return 0;
            index++;
            if (data[index] == '}')
                return 0;
            for (digit = 0; digit < 16; ++digit)
            {
                char character;
                if (index == size)
                    return 0;
                character = data[index];
                if (character == '}')
                    break;
                if (!__luauc_is_hex_digit(character))
                    return 0;
                code = 16 * code +
                    (unsigned int)(__luauc_is_digit(character) ? character - '0' : (character | ' ') - 'a' + 10);
                index++;
            }
            if (index == size || data[index] != '}')
                return 0;
            index++;
            utf8 = __luauc_to_utf8(data + write, code);
            if (utf8 == 0)
                return 0;
            write += utf8;
            break;
        }
        default:
            if (__luauc_is_digit(escape))
            {
                unsigned int code = (unsigned int)(escape - '0');
                int digit;
                for (digit = 0; digit < 2; ++digit)
                {
                    if (index == size || !__luauc_is_digit(data[index]))
                        break;
                    code = 10 * code + (unsigned int)(data[index] - '0');
                    index++;
                }
                if (code > UCHAR_MAX)
                    return 0;
                data[write++] = (char)code;
            }
            else
                data[write++] = __luauc_unescape(escape);
        }
    }
    *size_pointer = write;
    return 1;
}

void luauc_fixup_multiline_string(char* data, size_t* size_pointer)
{
    size_t size;
    size_t read = 0;
    size_t write = 0;
    if (data == NULL || size_pointer == NULL || *size_pointer == 0)
        return;
    size = *size_pointer;
    if (size >= 2 && data[0] == '\r' && data[1] == '\n')
        read = 2;
    else if (data[0] == '\n')
        read = 1;
    while (read < size)
    {
        if (read + 1 < size && data[read] == '\r' && data[read + 1] == '\n')
        {
            data[write++] = '\n';
            read += 2;
        }
        else
            data[write++] = data[read++];
    }
    *size_pointer = write;
}
