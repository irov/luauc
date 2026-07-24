// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "ast/luauc_parser.h"

#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct luauc_parser_function_t
{
    int vararg;
    unsigned int loop_depth;
} luauc_parser_function_t;

typedef struct luauc_parser_binding_t
{
    luauc_name_t name;
    luauc_location_t location;
    luauc_ast_type_t* annotation;
    int is_const;
} luauc_parser_binding_t;

typedef struct luauc_parser_t
{
    luauc_lexer_t lexer;
    luauc_arena_t* arena;
    luauc_name_table_t* names;
    luauc_allocator_t allocator;
    luauc_vector_t locals;
    luauc_vector_t functions;
    luauc_vector_t hot_comments;
    unsigned int recursion;
    int hot_comment_header;
    int failed;
    luauc_location_t error_location;
    char* error_message;
    size_t error_capacity;
    jmp_buf error_jump;
} luauc_parser_t;

static luauc_ast_stat_t* __luauc_parse_block(luauc_parser_t* parser, int scoped);
static luauc_ast_stat_t* __luauc_parse_statement(luauc_parser_t* parser);
static luauc_ast_expr_t* __luauc_parse_expression(luauc_parser_t* parser, unsigned int limit);
static luauc_ast_expr_t* __luauc_parse_primary(luauc_parser_t* parser, int as_statement);
static luauc_ast_type_t* __luauc_parse_type(luauc_parser_t* parser);
static luauc_ast_type_pack_t* __luauc_parse_return_type(luauc_parser_t* parser);
static luauc_ast_stat_t* __luauc_parse_type_alias(luauc_parser_t* parser, luauc_location_t start, int exported);
static luauc_ast_stat_t* __luauc_parse_class_statement(luauc_parser_t* parser, luauc_location_t start, int exported);
static luauc_ast_stat_t* __luauc_parse_attribute_statement(luauc_parser_t* parser);

static void __luauc_parse_generic_list(luauc_parser_t* parser, luauc_vector_t* generics, luauc_vector_t* generic_packs);

static void* __luauc_parser_allocate(luauc_parser_t* parser, size_t size, size_t alignment)
{
    void* result = luauc_arena_allocate(parser->arena, size, alignment);
    if (result == NULL)
    {
        parser->failed = 1;
        longjmp(parser->error_jump, 1);
    }
    memset(result, 0, size);
    return result;
}

#define LUAUC_NEW(parser, type) ((type*)__luauc_parser_allocate((parser), sizeof(type), _Alignof(type)))

static luauc_array_t __luauc_parser_copy_array(luauc_parser_t* parser, const luauc_vector_t* vector)
{
    luauc_array_t result = {NULL, 0};
    size_t bytes;
    if (vector->size == 0)
        return result;
    if (!luauc_size_multiply(vector->size, vector->element_size, &bytes))
    {
        parser->failed = 1;
        longjmp(parser->error_jump, 1);
    }
    result.data = __luauc_parser_allocate(parser, bytes, _Alignof(max_align_t));
    memcpy(result.data, vector->data, bytes);
    result.size = vector->size;
    return result;
}

static luauc_array_t __luauc_parser_copy_single(luauc_parser_t* parser, const void* value, size_t size, size_t alignment)
{
    luauc_array_t result;
    result.data = __luauc_parser_allocate(parser, size, alignment);
    memcpy(result.data, value, size);
    result.size = 1;
    return result;
}

static void __luauc_parser_error(luauc_parser_t* parser, luauc_location_t location, const char* format, ...)
{
    va_list arguments;
    va_list copy;
    int length;
    void* memory;

    parser->error_location = location;
    va_start(arguments, format);
    va_copy(copy, arguments);
    length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0)
    {
        va_end(arguments);
        parser->failed = 1;
        longjmp(parser->error_jump, 1);
    }
    if ((size_t)length + 1 > parser->error_capacity)
    {
        memory = parser->allocator.reallocate(
            parser->allocator.context, parser->error_message, parser->error_capacity, (size_t)length + 1
        );
        if (memory == NULL)
        {
            va_end(arguments);
            parser->failed = 1;
            longjmp(parser->error_jump, 1);
        }
        parser->error_message = (char*)memory;
        parser->error_capacity = (size_t)length + 1;
    }
    vsnprintf(parser->error_message, parser->error_capacity, format, arguments);
    va_end(arguments);
    longjmp(parser->error_jump, 1);
}

static int __luauc_name_is(luauc_name_t name, const char* text)
{
    return name.value != NULL && strcmp(name.value, text) == 0;
}

static luauc_location_t __luauc_join_locations(luauc_location_t begin, luauc_location_t end)
{
    return __luauc_location_make(begin.begin, end.end);
}

static luauc_ast_expr_t* __luauc_new_expression(luauc_parser_t* parser, luauc_ast_expr_kind_t kind, luauc_location_t location)
{
    luauc_ast_expr_t* expression = LUAUC_NEW(parser, luauc_ast_expr_t);
    expression->kind = kind;
    expression->location = location;
    return expression;
}

static luauc_ast_stat_t* __luauc_new_statement(luauc_parser_t* parser, luauc_ast_stat_kind_t kind, luauc_location_t location)
{
    luauc_ast_stat_t* statement = LUAUC_NEW(parser, luauc_ast_stat_t);
    statement->kind = kind;
    statement->location = location;
    return statement;
}

static luauc_ast_type_t* __luauc_new_type(luauc_parser_t* parser, luauc_ast_type_kind_t kind, luauc_location_t location)
{
    luauc_ast_type_t* type = LUAUC_NEW(parser, luauc_ast_type_t);
    type->kind = kind;
    type->location = location;
    return type;
}

static luauc_ast_type_pack_t* __luauc_new_type_pack(
    luauc_parser_t* parser, luauc_ast_type_pack_kind_t kind, luauc_location_t location
)
{
    luauc_ast_type_pack_t* pack = LUAUC_NEW(parser, luauc_ast_type_pack_t);
    pack->kind = kind;
    pack->location = location;
    return pack;
}

static void __luauc_parser_next(luauc_parser_t* parser)
{
    luauc_token_type_t type = luauc_lexer_next_options(&parser->lexer, 0, 1)->type;
    while (type == LUAUC_TOKEN_COMMENT || type == LUAUC_TOKEN_BLOCK_COMMENT)
    {
        const luauc_token_t* token = &parser->lexer.token;
        if (type == LUAUC_TOKEN_COMMENT && token->length != 0 && token->value.data[0] == '!')
        {
            luauc_hot_comment_t comment;
            size_t end = token->length;
            char* text;
            while (end > 0)
            {
                char character = token->value.data[end - 1];
                if (character != ' ' && character != '\t' && character != '\r' && character != '\n' &&
                    character != '\v' && character != '\f')
                    break;
                end--;
            }
            text = luauc_arena_duplicate(parser->arena, token->value.data + 1, end - 1);
            if (text == NULL)
            {
                parser->failed = 1;
                longjmp(parser->error_jump, 1);
            }
            comment.header = parser->hot_comment_header;
            comment.location = token->location;
            comment.text.data = text;
            comment.text.length = end - 1;
            if (luauc_vector_push(&parser->hot_comments, &comment) == NULL)
            {
                parser->failed = 1;
                longjmp(parser->error_jump, 1);
            }
        }
        type = luauc_lexer_next_options(&parser->lexer, 0, 0)->type;
    }
    if (type == LUAUC_TOKEN_BROKEN_COMMENT)
        __luauc_parser_error(parser, parser->lexer.token.location, "Unfinished comment");
    if (type == LUAUC_TOKEN_BROKEN_UNICODE)
        __luauc_parser_error(parser, parser->lexer.token.location, "Invalid Unicode character");
}

static int __luauc_parser_accept(luauc_parser_t* parser, luauc_token_type_t type)
{
    if (parser->lexer.token.type != type)
        return 0;
    __luauc_parser_next(parser);
    return 1;
}

static void __luauc_parser_expect(luauc_parser_t* parser, luauc_token_type_t type, const char* context)
{
    if (parser->lexer.token.type != type)
    {
        if (context != NULL)
            __luauc_parser_error(
                parser,
                parser->lexer.token.location,
                "Expected %s when parsing %s, got %s",
                luauc_token_name(type),
                context,
                luauc_token_name(parser->lexer.token.type)
            );
        else
            __luauc_parser_error(
                parser,
                parser->lexer.token.location,
                "Expected %s, got %s",
                luauc_token_name(type),
                luauc_token_name(parser->lexer.token.type)
            );
    }
    __luauc_parser_next(parser);
}

static luauc_name_t __luauc_parser_parse_name(luauc_parser_t* parser, luauc_location_t* location, const char* context)
{
    luauc_name_t name;
    if (parser->lexer.token.type != LUAUC_TOKEN_NAME)
        __luauc_parser_error(
            parser,
            parser->lexer.token.location,
            context != NULL ? "Expected identifier when parsing %s, got %s" : "Expected identifier, got %s",
            context != NULL ? context : luauc_token_name(parser->lexer.token.type),
            context != NULL ? luauc_token_name(parser->lexer.token.type) : ""
        );
    name.value = parser->lexer.token.value.name;
    if (location != NULL)
        *location = parser->lexer.token.location;
    __luauc_parser_next(parser);
    return name;
}

static int __luauc_parser_block_follow(luauc_token_type_t type)
{
    return type == LUAUC_TOKEN_EOF || type == LUAUC_TOKEN_ELSE || type == LUAUC_TOKEN_ELSEIF ||
        type == LUAUC_TOKEN_END || type == LUAUC_TOKEN_UNTIL;
}

static luauc_parser_function_t* __luauc_parser_current_function(luauc_parser_t* parser)
{
    return (luauc_parser_function_t*)luauc_vector_at(&parser->functions, parser->functions.size - 1);
}

static unsigned int __luauc_parser_save_locals(const luauc_parser_t* parser)
{
    return (unsigned int)parser->locals.size;
}

static void __luauc_parser_restore_locals(luauc_parser_t* parser, unsigned int count)
{
    parser->locals.size = count;
}

static luauc_ast_local_t* __luauc_parser_find_local(const luauc_parser_t* parser, luauc_name_t name)
{
    size_t index;
    for (index = parser->locals.size; index > 0; --index)
    {
        luauc_ast_local_t* local = *(luauc_ast_local_t* const*)luauc_vector_at_const(&parser->locals, index - 1);
        if (local->name.value == name.value)
            return local;
    }
    return NULL;
}

static luauc_ast_local_t* __luauc_parser_push_local(luauc_parser_t* parser, const luauc_parser_binding_t* binding)
{
    luauc_ast_local_t* local = LUAUC_NEW(parser, luauc_ast_local_t);
    local->name = binding->name;
    local->location = binding->location;
    local->shadow = __luauc_parser_find_local(parser, binding->name);
    local->function_depth = parser->functions.size - 1;
    local->loop_depth = __luauc_parser_current_function(parser)->loop_depth;
    local->is_const = binding->is_const;
    local->annotation = binding->annotation;
    if (luauc_vector_push(&parser->locals, &local) == NULL)
    {
        parser->failed = 1;
        longjmp(parser->error_jump, 1);
    }
    return local;
}

static luauc_parser_binding_t __luauc_parser_parse_binding(luauc_parser_t* parser, int is_const)
{
    luauc_parser_binding_t binding;
    binding.name = __luauc_parser_parse_name(parser, &binding.location, "variable name");
    binding.annotation = NULL;
    binding.is_const = is_const;
    if (__luauc_parser_accept(parser, (luauc_token_type_t)':'))
        binding.annotation = __luauc_parse_type(parser);
    return binding;
}

static void __luauc_parse_generic_list(luauc_parser_t* parser, luauc_vector_t* generics, luauc_vector_t* generic_packs)
{
    if (!__luauc_parser_accept(parser, (luauc_token_type_t)'<'))
        return;
    do
    {
        luauc_location_t location;
        luauc_name_t name = __luauc_parser_parse_name(parser, &location, "generic type");
        if (__luauc_parser_accept(parser, LUAUC_TOKEN_DOT3))
        {
            luauc_ast_generic_type_pack_t* generic = LUAUC_NEW(parser, luauc_ast_generic_type_pack_t);
            generic->location = __luauc_location_make(location.begin, parser->lexer.previous_location.end);
            generic->name = name;
            if (luauc_vector_push(generic_packs, &generic) == NULL)
                longjmp(parser->error_jump, 1);
        }
        else
        {
            luauc_ast_generic_type_t* generic = LUAUC_NEW(parser, luauc_ast_generic_type_t);
            generic->location = location;
            generic->name = name;
            if (luauc_vector_push(generics, &generic) == NULL)
                longjmp(parser->error_jump, 1);
        }
    } while (__luauc_parser_accept(parser, (luauc_token_type_t)','));
    __luauc_parser_expect(parser, (luauc_token_type_t)'>', "generic type list");
}

static void __luauc_parser_parse_expression_list(luauc_parser_t* parser, luauc_vector_t* expressions)
{
    luauc_ast_expr_t* expression = __luauc_parse_expression(parser, 0);
    if (luauc_vector_push(expressions, &expression) == NULL)
        longjmp(parser->error_jump, 1);
    while (__luauc_parser_accept(parser, (luauc_token_type_t)','))
    {
        expression = __luauc_parse_expression(parser, 0);
        if (luauc_vector_push(expressions, &expression) == NULL)
            longjmp(parser->error_jump, 1);
    }
}

static luauc_ast_stat_t* __luauc_parse_if_statement(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_location_t end;
    luauc_ast_expr_t* condition;
    luauc_ast_stat_t* then_body;
    luauc_ast_stat_t* else_body = NULL;
    luauc_ast_stat_t* result;
    luauc_optional_location_t then_location = {0};
    luauc_optional_location_t else_location = {0};

    __luauc_parser_next(parser);
    condition = __luauc_parse_expression(parser, 0);
    then_location.present = 1;
    then_location.value = parser->lexer.token.location;
    __luauc_parser_expect(parser, LUAUC_TOKEN_THEN, "if statement");
    then_body = __luauc_parse_block(parser, 1);

    if (parser->lexer.token.type == LUAUC_TOKEN_ELSEIF)
    {
        else_location.present = 1;
        else_location.value = parser->lexer.token.location;
        else_body = __luauc_parse_if_statement(parser);
        end = else_body->location;
        then_body->value.block.has_end = 1;
    }
    else
    {
        if (parser->lexer.token.type == LUAUC_TOKEN_ELSE)
        {
            else_location.present = 1;
            else_location.value = parser->lexer.token.location;
            __luauc_parser_next(parser);
            else_body = __luauc_parse_block(parser, 1);
        }
        end = parser->lexer.token.location;
        __luauc_parser_expect(parser, LUAUC_TOKEN_END, "if statement");
        then_body->value.block.has_end = 1;
        if (else_body != NULL)
            else_body->value.block.has_end = 1;
    }

    result = __luauc_new_statement(parser, LUAUC_STAT_IF, __luauc_join_locations(start, end));
    result->value.if_statement.condition = condition;
    result->value.if_statement.then_body = then_body;
    result->value.if_statement.else_body = else_body;
    result->value.if_statement.then_location = then_location;
    result->value.if_statement.else_location = else_location;
    return result;
}

static luauc_ast_stat_t* __luauc_parse_while_statement(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_location_t do_location;
    luauc_location_t end;
    luauc_ast_expr_t* condition;
    luauc_ast_stat_t* body;
    luauc_ast_stat_t* result;

    __luauc_parser_next(parser);
    condition = __luauc_parse_expression(parser, 0);
    do_location = parser->lexer.token.location;
    __luauc_parser_expect(parser, LUAUC_TOKEN_DO, "while loop");
    __luauc_parser_current_function(parser)->loop_depth++;
    body = __luauc_parse_block(parser, 1);
    __luauc_parser_current_function(parser)->loop_depth--;
    end = parser->lexer.token.location;
    __luauc_parser_expect(parser, LUAUC_TOKEN_END, "while loop");
    body->value.block.has_end = 1;

    result = __luauc_new_statement(parser, LUAUC_STAT_WHILE, __luauc_join_locations(start, end));
    result->value.while_statement.condition = condition;
    result->value.while_statement.body = body;
    result->value.while_statement.has_do = 1;
    result->value.while_statement.do_location = do_location;
    return result;
}

static luauc_ast_stat_t* __luauc_parse_repeat_statement(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    unsigned int locals = __luauc_parser_save_locals(parser);
    luauc_ast_stat_t* body;
    luauc_ast_expr_t* condition;
    luauc_ast_stat_t* result;

    __luauc_parser_next(parser);
    __luauc_parser_current_function(parser)->loop_depth++;
    body = __luauc_parse_block(parser, 0);
    __luauc_parser_current_function(parser)->loop_depth--;
    __luauc_parser_expect(parser, LUAUC_TOKEN_UNTIL, "repeat loop");
    body->value.block.has_end = 1;
    condition = __luauc_parse_expression(parser, 0);
    __luauc_parser_restore_locals(parser, locals);

    result = __luauc_new_statement(parser, LUAUC_STAT_REPEAT, __luauc_join_locations(start, condition->location));
    result->value.repeat_statement.condition = condition;
    result->value.repeat_statement.body = body;
    result->value.repeat_statement.has_until = 1;
    return result;
}

static luauc_ast_stat_t* __luauc_parse_do_statement(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_location_t end;
    luauc_ast_stat_t* body;
    __luauc_parser_next(parser);
    body = __luauc_parse_block(parser, 1);
    end = parser->lexer.token.location;
    __luauc_parser_expect(parser, LUAUC_TOKEN_END, "do block");
    body->location = __luauc_join_locations(start, end);
    body->value.block.has_end = 1;
    return body;
}

static luauc_ast_stat_t* __luauc_parse_return_statement(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_vector_t expressions;
    luauc_ast_stat_t* result;
    luauc_vector_init(&expressions, sizeof(luauc_ast_expr_t*), parser->allocator);
    __luauc_parser_next(parser);
    if (!__luauc_parser_block_follow(parser->lexer.token.type) && parser->lexer.token.type != (luauc_token_type_t)';')
        __luauc_parser_parse_expression_list(parser, &expressions);
    result = __luauc_new_statement(
        parser,
        LUAUC_STAT_RETURN,
        expressions.size == 0 ? start :
            __luauc_join_locations(start, (*(luauc_ast_expr_t**)luauc_vector_at(&expressions, expressions.size - 1))->location)
    );
    result->value.return_statement.expressions = __luauc_parser_copy_array(parser, &expressions);
    luauc_vector_destroy(&expressions);
    return result;
}

static luauc_ast_stat_t* __luauc_parse_for_statement(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_parser_binding_t first;
    luauc_ast_stat_t* result;
    luauc_ast_stat_t* body;
    luauc_location_t do_location;
    luauc_location_t end;
    unsigned int locals;

    __luauc_parser_next(parser);
    first = __luauc_parser_parse_binding(parser, 0);
    if (__luauc_parser_accept(parser, (luauc_token_type_t)'='))
    {
        luauc_ast_expr_t* from = __luauc_parse_expression(parser, 0);
        luauc_ast_expr_t* to;
        luauc_ast_expr_t* step = NULL;
        luauc_ast_local_t* variable;
        __luauc_parser_expect(parser, (luauc_token_type_t)',', "index range");
        to = __luauc_parse_expression(parser, 0);
        if (__luauc_parser_accept(parser, (luauc_token_type_t)','))
            step = __luauc_parse_expression(parser, 0);
        do_location = parser->lexer.token.location;
        __luauc_parser_expect(parser, LUAUC_TOKEN_DO, "for loop");
        locals = __luauc_parser_save_locals(parser);
        __luauc_parser_current_function(parser)->loop_depth++;
        variable = __luauc_parser_push_local(parser, &first);
        body = __luauc_parse_block(parser, 1);
        __luauc_parser_current_function(parser)->loop_depth--;
        __luauc_parser_restore_locals(parser, locals);
        end = parser->lexer.token.location;
        __luauc_parser_expect(parser, LUAUC_TOKEN_END, "for loop");
        body->value.block.has_end = 1;
        result = __luauc_new_statement(parser, LUAUC_STAT_FOR, __luauc_join_locations(start, end));
        result->value.for_statement.variable = variable;
        result->value.for_statement.from = from;
        result->value.for_statement.to = to;
        result->value.for_statement.step = step;
        result->value.for_statement.body = body;
        result->value.for_statement.has_do = 1;
        result->value.for_statement.do_location = do_location;
        return result;
    }
    else
    {
        luauc_vector_t bindings;
        luauc_vector_t values;
        luauc_vector_t variables;
        luauc_parser_binding_t binding;
        size_t index;
        luauc_vector_init(&bindings, sizeof(luauc_parser_binding_t), parser->allocator);
        luauc_vector_init(&values, sizeof(luauc_ast_expr_t*), parser->allocator);
        luauc_vector_init(&variables, sizeof(luauc_ast_local_t*), parser->allocator);
        luauc_vector_push(&bindings, &first);
        while (__luauc_parser_accept(parser, (luauc_token_type_t)','))
        {
            binding = __luauc_parser_parse_binding(parser, 0);
            luauc_vector_push(&bindings, &binding);
        }
        __luauc_parser_expect(parser, LUAUC_TOKEN_IN, "for loop");
        __luauc_parser_parse_expression_list(parser, &values);
        do_location = parser->lexer.token.location;
        __luauc_parser_expect(parser, LUAUC_TOKEN_DO, "for loop");
        locals = __luauc_parser_save_locals(parser);
        __luauc_parser_current_function(parser)->loop_depth++;
        for (index = 0; index < bindings.size; ++index)
        {
            luauc_ast_local_t* variable = __luauc_parser_push_local(
                parser, (const luauc_parser_binding_t*)luauc_vector_at_const(&bindings, index)
            );
            luauc_vector_push(&variables, &variable);
        }
        body = __luauc_parse_block(parser, 1);
        __luauc_parser_current_function(parser)->loop_depth--;
        __luauc_parser_restore_locals(parser, locals);
        end = parser->lexer.token.location;
        __luauc_parser_expect(parser, LUAUC_TOKEN_END, "for loop");
        body->value.block.has_end = 1;
        result = __luauc_new_statement(parser, LUAUC_STAT_FOR_IN, __luauc_join_locations(start, end));
        result->value.for_in.variables = __luauc_parser_copy_array(parser, &variables);
        result->value.for_in.values = __luauc_parser_copy_array(parser, &values);
        result->value.for_in.body = body;
        result->value.for_in.has_in = 1;
        result->value.for_in.has_do = 1;
        result->value.for_in.do_location = do_location;
        luauc_vector_destroy(&bindings);
        luauc_vector_destroy(&values);
        luauc_vector_destroy(&variables);
        return result;
    }
}

static luauc_ast_expr_t* __luauc_parse_function_body(
    luauc_parser_t* parser,
    luauc_location_t start,
    luauc_name_t debug_name,
    int has_self,
    const luauc_parser_binding_t* local_name,
    int local_is_const
)
{
    luauc_vector_t bindings;
    luauc_vector_t arguments;
    luauc_vector_t generics;
    luauc_vector_t generic_packs;
    luauc_ast_local_t* self = NULL;
    luauc_ast_local_t* function_local = NULL;
    luauc_ast_stat_t* body;
    luauc_ast_expr_t* result;
    luauc_ast_type_pack_t* return_annotation = NULL;
    luauc_ast_type_pack_t* vararg_annotation = NULL;
    luauc_location_t vararg_location = {{0, 0}, {0, 0}};
    luauc_location_t argument_location;
    luauc_location_t end;
    luauc_parser_function_t function = {0, 0};
    unsigned int locals;
    int vararg = 0;
    size_t index;

    luauc_vector_init(&bindings, sizeof(luauc_parser_binding_t), parser->allocator);
    luauc_vector_init(&arguments, sizeof(luauc_ast_local_t*), parser->allocator);
    luauc_vector_init(&generics, sizeof(luauc_ast_generic_type_t*), parser->allocator);
    luauc_vector_init(&generic_packs, sizeof(luauc_ast_generic_type_pack_t*), parser->allocator);
    __luauc_parse_generic_list(parser, &generics, &generic_packs);
    argument_location.begin = parser->lexer.token.location.begin;
    __luauc_parser_expect(parser, (luauc_token_type_t)'(', "function");
    while (parser->lexer.token.type != (luauc_token_type_t)')')
    {
        if (parser->lexer.token.type == LUAUC_TOKEN_DOT3)
        {
            vararg = 1;
            vararg_location = parser->lexer.token.location;
            __luauc_parser_next(parser);
            if (__luauc_parser_accept(parser, (luauc_token_type_t)':'))
            {
                if (parser->lexer.token.type == LUAUC_TOKEN_NAME &&
                    luauc_lexer_lookahead(&parser->lexer).type == LUAUC_TOKEN_DOT3)
                {
                    luauc_location_t name_location;
                    luauc_name_t name = __luauc_parser_parse_name(parser, &name_location, "generic type pack");
                    luauc_location_t end_location = parser->lexer.token.location;
                    __luauc_parser_expect(parser, LUAUC_TOKEN_DOT3, "generic type pack");
                    vararg_annotation = __luauc_new_type_pack(
                        parser, LUAUC_TYPE_PACK_GENERIC, __luauc_join_locations(name_location, end_location)
                    );
                    vararg_annotation->value.generic_name = name;
                }
                else
                {
                    luauc_ast_type_t* type = __luauc_parse_type(parser);
                    vararg_annotation = __luauc_new_type_pack(parser, LUAUC_TYPE_PACK_VARIADIC, type->location);
                    vararg_annotation->value.variadic_type = type;
                }
            }
            break;
        }
        {
            luauc_parser_binding_t binding = __luauc_parser_parse_binding(parser, 0);
            luauc_vector_push(&bindings, &binding);
        }
        if (!__luauc_parser_accept(parser, (luauc_token_type_t)','))
            break;
    }
    argument_location.end = parser->lexer.token.location.end;
    __luauc_parser_expect(parser, (luauc_token_type_t)')', "function");
    if (__luauc_parser_accept(parser, (luauc_token_type_t)':'))
        return_annotation = __luauc_parse_return_type(parser);

    if (local_name != NULL)
        function_local = __luauc_parser_push_local(parser, local_name);
    (void)function_local;
    locals = __luauc_parser_save_locals(parser);
    function.vararg = vararg;
    luauc_vector_push(&parser->functions, &function);
    if (has_self)
    {
        luauc_parser_binding_t binding;
        luauc_token_type_t ignored;
        binding.name = luauc_name_table_add(parser->names, "self", 4, &ignored);
        binding.location = start;
        binding.annotation = NULL;
        binding.is_const = 0;
        self = __luauc_parser_push_local(parser, &binding);
    }
    for (index = 0; index < bindings.size; ++index)
    {
        luauc_ast_local_t* argument = __luauc_parser_push_local(
            parser, (const luauc_parser_binding_t*)luauc_vector_at_const(&bindings, index)
        );
        luauc_vector_push(&arguments, &argument);
    }
    body = __luauc_parse_block(parser, 1);
    parser->functions.size--;
    __luauc_parser_restore_locals(parser, locals);
    end = parser->lexer.token.location;
    __luauc_parser_expect(parser, LUAUC_TOKEN_END, "function");
    body->value.block.has_end = 1;

    result = __luauc_new_expression(parser, LUAUC_EXPR_FUNCTION, __luauc_join_locations(start, end));
    result->value.function.generics = __luauc_parser_copy_array(parser, &generics);
    result->value.function.generic_packs = __luauc_parser_copy_array(parser, &generic_packs);
    result->value.function.self = self;
    result->value.function.arguments = __luauc_parser_copy_array(parser, &arguments);
    result->value.function.return_annotation = return_annotation;
    result->value.function.vararg = vararg;
    result->value.function.vararg_location = vararg_location;
    result->value.function.vararg_annotation = vararg_annotation;
    result->value.function.body = body;
    result->value.function.function_depth = parser->functions.size;
    result->value.function.debugname = debug_name;
    result->value.function.argument_location.present = 1;
    result->value.function.argument_location.value = argument_location;
    luauc_vector_destroy(&bindings);
    luauc_vector_destroy(&arguments);
    luauc_vector_destroy(&generics);
    luauc_vector_destroy(&generic_packs);
    (void)local_is_const;
    return result;
}

static luauc_ast_stat_t* __luauc_parse_function_statement(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_location_t name_location;
    luauc_name_t debug_name;
    luauc_ast_expr_t* name;
    luauc_ast_expr_t* function;
    luauc_ast_stat_t* result;
    int self = 0;

    __luauc_parser_next(parser);
    debug_name = __luauc_parser_parse_name(parser, &name_location, "function name");
    {
        luauc_ast_local_t* local = __luauc_parser_find_local(parser, debug_name);
        name = __luauc_new_expression(
            parser, local != NULL ? LUAUC_EXPR_LOCAL : LUAUC_EXPR_GLOBAL, name_location
        );
        if (local != NULL)
        {
            name->value.local.local = local;
            name->value.local.upvalue = local->function_depth != parser->functions.size - 1;
        }
        else
            name->value.global.name = debug_name;
    }
    while (parser->lexer.token.type == (luauc_token_type_t)'.' || parser->lexer.token.type == (luauc_token_type_t)':')
    {
        char operator_character = (char)parser->lexer.token.type;
        luauc_position_t operator_position = parser->lexer.token.location.begin;
        luauc_location_t index_location;
        luauc_name_t index;
        luauc_ast_expr_t* indexed;
        __luauc_parser_next(parser);
        index = __luauc_parser_parse_name(parser, &index_location, "field name");
        debug_name = index;
        indexed = __luauc_new_expression(parser, LUAUC_EXPR_INDEX_NAME, __luauc_join_locations(name->location, index_location));
        indexed->value.index_name.expression = name;
        indexed->value.index_name.index = index;
        indexed->value.index_name.index_location = index_location;
        indexed->value.index_name.operator_position = operator_position;
        indexed->value.index_name.operator_character = operator_character;
        name = indexed;
        if (operator_character == ':')
        {
            self = 1;
            break;
        }
    }
    function = __luauc_parse_function_body(parser, start, debug_name, self, NULL, 0);
    result = __luauc_new_statement(parser, LUAUC_STAT_FUNCTION, __luauc_join_locations(start, function->location));
    result->value.function.name = name;
    result->value.function.function = function;
    return result;
}

static luauc_ast_stat_t* __luauc_parse_local_statement(luauc_parser_t* parser, int is_const, int consume_keyword)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_vector_t bindings;
    luauc_vector_t variables;
    luauc_vector_t values;
    luauc_ast_stat_t* result;
    size_t index;

    if (consume_keyword)
        __luauc_parser_next(parser);
    if (parser->lexer.token.type == LUAUC_TOKEN_FUNCTION)
    {
        luauc_location_t function_start = parser->lexer.token.location;
        luauc_location_t name_location;
        luauc_name_t name;
        luauc_parser_binding_t binding;
        luauc_ast_expr_t* function;
        luauc_ast_local_t* local;
        __luauc_parser_next(parser);
        name = __luauc_parser_parse_name(parser, &name_location, "function name");
        binding.name = name;
        binding.location = name_location;
        binding.annotation = NULL;
        binding.is_const = is_const;
        local = __luauc_parser_push_local(parser, &binding);
        function = __luauc_parse_function_body(parser, function_start, name, 0, NULL, is_const);
        result = __luauc_new_statement(parser, LUAUC_STAT_LOCAL_FUNCTION, __luauc_join_locations(start, function->location));
        result->value.local_function.name = local;
        result->value.local_function.function = function;
        result->value.local_function.is_const = is_const;
        result->value.local_function.const_keyword_begin = is_const ? start.begin : __luauc_position_missing();
        return result;
    }

    luauc_vector_init(&bindings, sizeof(luauc_parser_binding_t), parser->allocator);
    luauc_vector_init(&variables, sizeof(luauc_ast_local_t*), parser->allocator);
    luauc_vector_init(&values, sizeof(luauc_ast_expr_t*), parser->allocator);
    do
    {
        luauc_parser_binding_t binding = __luauc_parser_parse_binding(parser, is_const);
        luauc_vector_push(&bindings, &binding);
    } while (__luauc_parser_accept(parser, (luauc_token_type_t)','));
    if (__luauc_parser_accept(parser, (luauc_token_type_t)'='))
        __luauc_parser_parse_expression_list(parser, &values);
    for (index = 0; index < bindings.size; ++index)
    {
        luauc_ast_local_t* local = __luauc_parser_push_local(
            parser, (const luauc_parser_binding_t*)luauc_vector_at_const(&bindings, index)
        );
        luauc_vector_push(&variables, &local);
    }
    result = __luauc_new_statement(
        parser,
        LUAUC_STAT_LOCAL,
        values.size == 0 ? start :
            __luauc_join_locations(start, (*(luauc_ast_expr_t**)luauc_vector_at(&values, values.size - 1))->location)
    );
    result->value.local.variables = __luauc_parser_copy_array(parser, &variables);
    result->value.local.values = __luauc_parser_copy_array(parser, &values);
    result->value.local.is_const = is_const;
    luauc_vector_destroy(&bindings);
    luauc_vector_destroy(&variables);
    luauc_vector_destroy(&values);
    return result;
}

static int __luauc_expression_is_assignable(const luauc_ast_expr_t* expression)
{
    return expression->kind == LUAUC_EXPR_GLOBAL || expression->kind == LUAUC_EXPR_INDEX_NAME ||
        expression->kind == LUAUC_EXPR_INDEX_EXPR ||
        (expression->kind == LUAUC_EXPR_LOCAL && !expression->value.local.local->is_const);
}

static int __luauc_parse_compound_operator(luauc_token_type_t token, luauc_binary_op_t* op)
{
    switch (token)
    {
    case LUAUC_TOKEN_ADD_ASSIGN:
        *op = LUAUC_BINARY_ADD;
        return 1;
    case LUAUC_TOKEN_SUB_ASSIGN:
        *op = LUAUC_BINARY_SUB;
        return 1;
    case LUAUC_TOKEN_MUL_ASSIGN:
        *op = LUAUC_BINARY_MUL;
        return 1;
    case LUAUC_TOKEN_DIV_ASSIGN:
        *op = LUAUC_BINARY_DIV;
        return 1;
    case LUAUC_TOKEN_FLOOR_DIV_ASSIGN:
        *op = LUAUC_BINARY_FLOOR_DIV;
        return 1;
    case LUAUC_TOKEN_MOD_ASSIGN:
        *op = LUAUC_BINARY_MOD;
        return 1;
    case LUAUC_TOKEN_POW_ASSIGN:
        *op = LUAUC_BINARY_POW;
        return 1;
    case LUAUC_TOKEN_CONCAT_ASSIGN:
        *op = LUAUC_BINARY_CONCAT;
        return 1;
    default:
        return 0;
    }
}

static luauc_ast_stat_t* __luauc_parse_assignment(luauc_parser_t* parser, luauc_ast_expr_t* first)
{
    luauc_vector_t variables;
    luauc_vector_t values;
    luauc_ast_stat_t* result;
    size_t index;
    luauc_vector_init(&variables, sizeof(luauc_ast_expr_t*), parser->allocator);
    luauc_vector_init(&values, sizeof(luauc_ast_expr_t*), parser->allocator);
    luauc_vector_push(&variables, &first);
    while (__luauc_parser_accept(parser, (luauc_token_type_t)','))
    {
        luauc_ast_expr_t* variable = __luauc_parse_primary(parser, 1);
        luauc_vector_push(&variables, &variable);
    }
    for (index = 0; index < variables.size; ++index)
        if (!__luauc_expression_is_assignable(*(luauc_ast_expr_t**)luauc_vector_at(&variables, index)))
            __luauc_parser_error(
                parser,
                (*(luauc_ast_expr_t**)luauc_vector_at(&variables, index))->location,
                "Assigned expression must be a variable or a field"
            );
    __luauc_parser_expect(parser, (luauc_token_type_t)'=', "assignment");
    __luauc_parser_parse_expression_list(parser, &values);
    result = __luauc_new_statement(
        parser,
        LUAUC_STAT_ASSIGN,
        __luauc_join_locations(first->location, (*(luauc_ast_expr_t**)luauc_vector_at(&values, values.size - 1))->location)
    );
    result->value.assign.variables = __luauc_parser_copy_array(parser, &variables);
    result->value.assign.values = __luauc_parser_copy_array(parser, &values);
    luauc_vector_destroy(&variables);
    luauc_vector_destroy(&values);
    return result;
}

static luauc_ast_stat_t* __luauc_parse_class_statement(luauc_parser_t* parser, luauc_location_t start, int exported)
{
    luauc_location_t name_location;
    luauc_name_t name = __luauc_parser_parse_name(parser, &name_location, "class name");
    luauc_parser_binding_t binding;
    luauc_ast_local_t* local;
    luauc_vector_t members;
    luauc_location_t end;
    luauc_ast_stat_t* result;

    binding.name = name;
    binding.location = name_location;
    binding.annotation = NULL;
    binding.is_const = 1;
    local = __luauc_parser_push_local(parser, &binding);
    luauc_vector_init(&members, sizeof(luauc_class_member_t), parser->allocator);

    while (parser->lexer.token.type != LUAUC_TOKEN_END && parser->lexer.token.type != LUAUC_TOKEN_EOF)
    {
        luauc_optional_location_t qualifier;

        memset(&qualifier, 0, sizeof(qualifier));

        if (parser->lexer.token.type == LUAUC_TOKEN_NAME &&
            strcmp(parser->lexer.token.value.name, "public") == 0)
        {
            qualifier.present = 1;
            qualifier.value = parser->lexer.token.location;
            __luauc_parser_next(parser);
        }

        if (qualifier.present && parser->lexer.token.type != LUAUC_TOKEN_FUNCTION)
        {
            luauc_class_member_t member;
            luauc_location_t property_location;

            memset(&member, 0, sizeof(member));
            member.is_method = 0;
            member.value.property.qualifier_location = qualifier.value;
            member.value.property.name =
                __luauc_parser_parse_name(parser, &property_location, "class property name");
            member.value.property.name_location = property_location;

            if (strncmp(member.value.property.name.value, "__", 2) == 0)
                __luauc_parser_error(parser, property_location, "Class properties cannot start with '__'");

            if (__luauc_parser_accept(parser, (luauc_token_type_t)':'))
            {
                member.value.property.type_colon_location.present = 1;
                member.value.property.type_colon_location.value = parser->lexer.previous_location;
                member.value.property.type = __luauc_parse_type(parser);
            }

            if (luauc_vector_push(&members, &member) == NULL)
                longjmp(parser->error_jump, 1);
        }
        else if (parser->lexer.token.type == LUAUC_TOKEN_FUNCTION)
        {
            luauc_class_member_t member;
            luauc_location_t keyword_location = parser->lexer.token.location;
            luauc_location_t method_location;
            luauc_name_t method_name;

            memset(&member, 0, sizeof(member));
            __luauc_parser_next(parser);
            method_name = __luauc_parser_parse_name(parser, &method_location, "method name");
            member.is_method = 1;
            member.value.method.qualifier_location = qualifier;
            member.value.method.keyword_location = keyword_location;
            member.value.method.function_name = method_name;
            member.value.method.name_location = method_location;
            member.value.method.function =
                __luauc_parse_function_body(parser, keyword_location, method_name, 0, NULL, 0);

            if (luauc_vector_push(&members, &member) == NULL)
                longjmp(parser->error_jump, 1);
        }
        else
        {
            __luauc_parser_error(
                parser,
                parser->lexer.token.location,
                "Only class properties and functions can be declared within a class"
            );
        }
    }

    end = parser->lexer.token.location;
    __luauc_parser_expect(parser, LUAUC_TOKEN_END, "class");
    result = __luauc_new_statement(parser, LUAUC_STAT_CLASS, __luauc_join_locations(start, end));
    result->value.class_statement.name = local;
    result->value.class_statement.members = __luauc_parser_copy_array(parser, &members);
    result->value.class_statement.exported = exported;
    luauc_vector_destroy(&members);
    return result;
}

static luauc_attr_type_t __luauc_parse_attribute_type(luauc_name_t name)
{
    if (__luauc_name_is(name, "checked"))
        return LUAUC_ATTR_CHECKED;
    if (__luauc_name_is(name, "native"))
        return LUAUC_ATTR_NATIVE;
    if (__luauc_name_is(name, "deprecated"))
        return LUAUC_ATTR_DEPRECATED;
    if (__luauc_name_is(name, "debug_noinline"))
        return LUAUC_ATTR_DEBUG_NOINLINE;
    return LUAUC_ATTR_UNKNOWN;
}

static luauc_ast_stat_t* __luauc_parse_attribute_statement(luauc_parser_t* parser)
{
    luauc_vector_t attributes;
    luauc_ast_stat_t* statement = NULL;
    luauc_location_t start = parser->lexer.token.location;

    luauc_vector_init(&attributes, sizeof(luauc_ast_attr_t*), parser->allocator);
    while (parser->lexer.token.type == LUAUC_TOKEN_ATTRIBUTE)
    {
        luauc_ast_attr_t* attribute = LUAUC_NEW(parser, luauc_ast_attr_t);

        attribute->location = parser->lexer.token.location;
        attribute->name.value = parser->lexer.token.value.name;
        attribute->type = __luauc_parse_attribute_type(attribute->name);
        if (attribute->type == LUAUC_ATTR_UNKNOWN)
            __luauc_parser_error(
                parser, attribute->location, "Invalid attribute '@%s'", attribute->name.value
            );
        if (luauc_vector_push(&attributes, &attribute) == NULL)
            longjmp(parser->error_jump, 1);
        __luauc_parser_next(parser);
    }

    if (parser->lexer.token.type == LUAUC_TOKEN_FUNCTION)
        statement = __luauc_parse_function_statement(parser);
    else if (parser->lexer.token.type == LUAUC_TOKEN_LOCAL)
        statement = __luauc_parse_local_statement(parser, 0, 1);
    else
        __luauc_parser_error(
            parser,
            parser->lexer.token.location,
            "Expected 'function' or 'local function' after attribute"
        );

    if (statement->kind == LUAUC_STAT_FUNCTION)
        statement->value.function.function->value.function.attributes =
            __luauc_parser_copy_array(parser, &attributes);
    else if (statement->kind == LUAUC_STAT_LOCAL_FUNCTION)
        statement->value.local_function.function->value.function.attributes =
            __luauc_parser_copy_array(parser, &attributes);
    else
        __luauc_parser_error(parser, statement->location, "Attributes can only be applied to functions");

    statement->location.begin = start.begin;
    luauc_vector_destroy(&attributes);
    return statement;
}

static luauc_ast_stat_t* __luauc_parse_statement(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_ast_expr_t* expression;
    luauc_ast_stat_t* statement;
    luauc_binary_op_t compound;

    switch (parser->lexer.token.type)
    {
    case LUAUC_TOKEN_IF:
        return __luauc_parse_if_statement(parser);
    case LUAUC_TOKEN_WHILE:
        return __luauc_parse_while_statement(parser);
    case LUAUC_TOKEN_REPEAT:
        return __luauc_parse_repeat_statement(parser);
    case LUAUC_TOKEN_DO:
        return __luauc_parse_do_statement(parser);
    case LUAUC_TOKEN_FOR:
        return __luauc_parse_for_statement(parser);
    case LUAUC_TOKEN_FUNCTION:
        return __luauc_parse_function_statement(parser);
    case LUAUC_TOKEN_LOCAL:
        return __luauc_parse_local_statement(parser, 0, 1);
    case LUAUC_TOKEN_RETURN:
        return __luauc_parse_return_statement(parser);
    case LUAUC_TOKEN_BREAK:
        __luauc_parser_next(parser);
        if (__luauc_parser_current_function(parser)->loop_depth == 0)
            __luauc_parser_error(parser, start, "break statement must be inside a loop");
        return __luauc_new_statement(parser, LUAUC_STAT_BREAK, start);
    case LUAUC_TOKEN_ATTRIBUTE:
        return __luauc_parse_attribute_statement(parser);
    default:
        break;
    }

    expression = __luauc_parse_primary(parser, 1);
    if (expression->kind == LUAUC_EXPR_CALL)
    {
        statement = __luauc_new_statement(parser, LUAUC_STAT_EXPR, expression->location);
        statement->value.expression.expression = expression;
        return statement;
    }
    if (parser->lexer.token.type == (luauc_token_type_t)',' || parser->lexer.token.type == (luauc_token_type_t)'=')
        return __luauc_parse_assignment(parser, expression);
    if (__luauc_parse_compound_operator(parser->lexer.token.type, &compound))
    {
        luauc_ast_expr_t* value;
        if (!__luauc_expression_is_assignable(expression))
            __luauc_parser_error(parser, expression->location, "Assigned expression must be a variable or a field");
        __luauc_parser_next(parser);
        value = __luauc_parse_expression(parser, 0);
        statement = __luauc_new_statement(parser, LUAUC_STAT_COMPOUND_ASSIGN, __luauc_join_locations(start, value->location));
        statement->value.compound_assign.op = compound;
        statement->value.compound_assign.variable = expression;
        statement->value.compound_assign.value = value;
        return statement;
    }
    {
        luauc_name_t identifier = luauc_ast_get_identifier(expression);
        if (__luauc_name_is(identifier, "type"))
            return __luauc_parse_type_alias(parser, expression->location, 0);
        if (__luauc_name_is(identifier, "class"))
            return __luauc_parse_class_statement(parser, expression->location, 0);
        if (__luauc_name_is(identifier, "export") && parser->lexer.token.type == LUAUC_TOKEN_NAME &&
            strcmp(parser->lexer.token.value.name, "type") == 0)
        {
            __luauc_parser_next(parser);
            return __luauc_parse_type_alias(parser, expression->location, 1);
        }
        if (__luauc_name_is(identifier, "export") && parser->lexer.token.type == LUAUC_TOKEN_NAME &&
            strcmp(parser->lexer.token.value.name, "class") == 0)
        {
            __luauc_parser_next(parser);
            return __luauc_parse_class_statement(parser, expression->location, 1);
        }
        if (__luauc_name_is(identifier, "continue"))
        {
            if (__luauc_parser_current_function(parser)->loop_depth == 0)
                __luauc_parser_error(parser, expression->location, "continue statement must be inside a loop");
            return __luauc_new_statement(parser, LUAUC_STAT_CONTINUE, expression->location);
        }
        if (__luauc_name_is(identifier, "const"))
            return __luauc_parse_local_statement(parser, 1, 0);
    }
    __luauc_parser_error(
        parser, expression->location, "Incomplete statement: expected assignment or a function call"
    );
    return NULL;
}

static luauc_ast_stat_t* __luauc_parse_block(luauc_parser_t* parser, int scoped)
{
    unsigned int locals = __luauc_parser_save_locals(parser);
    luauc_vector_t statements;
    luauc_location_t location;
    luauc_ast_stat_t* result;
    luauc_position_t begin = parser->lexer.previous_location.end;
    luauc_vector_init(&statements, sizeof(luauc_ast_stat_t*), parser->allocator);
    while (!__luauc_parser_block_follow(parser->lexer.token.type))
    {
        luauc_ast_stat_t* statement = __luauc_parse_statement(parser);
        if (__luauc_parser_accept(parser, (luauc_token_type_t)';'))
        {
            statement->has_semicolon = 1;
            statement->location.end = parser->lexer.previous_location.end;
        }
        luauc_vector_push(&statements, &statement);
        if (statement->kind == LUAUC_STAT_BREAK || statement->kind == LUAUC_STAT_CONTINUE ||
            statement->kind == LUAUC_STAT_RETURN)
            break;
    }
    location = __luauc_location_make(begin, parser->lexer.token.location.begin);
    result = __luauc_new_statement(parser, LUAUC_STAT_BLOCK, location);
    result->value.block.body = __luauc_parser_copy_array(parser, &statements);
    if (scoped)
        __luauc_parser_restore_locals(parser, locals);
    luauc_vector_destroy(&statements);
    return result;
}

static int __luauc_parse_unary_operator(luauc_token_type_t token, luauc_unary_op_t* op)
{
    if (token == LUAUC_TOKEN_NOT)
        *op = LUAUC_UNARY_NOT;
    else if (token == (luauc_token_type_t)'-')
        *op = LUAUC_UNARY_MINUS;
    else if (token == (luauc_token_type_t)'#')
        *op = LUAUC_UNARY_LEN;
    else
        return 0;
    return 1;
}

static int __luauc_parse_binary_operator(luauc_token_type_t token, luauc_binary_op_t* op)
{
    switch ((int)token)
    {
    case (luauc_token_type_t)'+':
        *op = LUAUC_BINARY_ADD;
        break;
    case (luauc_token_type_t)'-':
        *op = LUAUC_BINARY_SUB;
        break;
    case (luauc_token_type_t)'*':
        *op = LUAUC_BINARY_MUL;
        break;
    case (luauc_token_type_t)'/':
        *op = LUAUC_BINARY_DIV;
        break;
    case LUAUC_TOKEN_FLOOR_DIV:
        *op = LUAUC_BINARY_FLOOR_DIV;
        break;
    case (luauc_token_type_t)'%':
        *op = LUAUC_BINARY_MOD;
        break;
    case (luauc_token_type_t)'^':
        *op = LUAUC_BINARY_POW;
        break;
    case LUAUC_TOKEN_DOT2:
        *op = LUAUC_BINARY_CONCAT;
        break;
    case LUAUC_TOKEN_NOT_EQUAL:
        *op = LUAUC_BINARY_COMPARE_NE;
        break;
    case LUAUC_TOKEN_EQUAL:
        *op = LUAUC_BINARY_COMPARE_EQ;
        break;
    case (luauc_token_type_t)'<':
        *op = LUAUC_BINARY_COMPARE_LT;
        break;
    case LUAUC_TOKEN_LESS_EQUAL:
        *op = LUAUC_BINARY_COMPARE_LE;
        break;
    case (luauc_token_type_t)'>':
        *op = LUAUC_BINARY_COMPARE_GT;
        break;
    case LUAUC_TOKEN_GREATER_EQUAL:
        *op = LUAUC_BINARY_COMPARE_GE;
        break;
    case LUAUC_TOKEN_AND:
        *op = LUAUC_BINARY_AND;
        break;
    case LUAUC_TOKEN_OR:
        *op = LUAUC_BINARY_OR;
        break;
    default:
        return 0;
    }
    return 1;
}

static luauc_ast_expr_t* __luauc_parse_number(luauc_parser_t* parser)
{
    luauc_location_t location = parser->lexer.token.location;
    size_t length = parser->lexer.token.length;
    const char* source = parser->lexer.token.value.data;
    char* data = (char*)__luauc_parser_allocate(parser, length + 1, 1);
    size_t read;
    size_t write = 0;
    char* end;
    double value;
    luauc_constant_number_parse_result_t parse_result = LUAUC_NUMBER_OK;
    luauc_ast_expr_t* result;

    for (read = 0; read < length; ++read)
        if (source[read] != '_')
            data[write++] = source[read];
    data[write] = '\0';
    errno = 0;
    if (write > 0 && data[write - 1] == 'i')
    {
        int64_t integer;
        unsigned long long unsigned_integer;
        int base = 10;
        const char* number = data;
        if (write > 2 && data[0] == '0' && (data[1] == 'b' || data[1] == 'B'))
        {
            base = 2;
            number = data + 2;
        }
        else if (write > 2 && data[0] == '0' && (data[1] == 'x' || data[1] == 'X'))
            base = 16;
        if (base == 10)
        {
            integer = strtoll(number, &end, base);
            if (*end != 'i' || end[1] != '\0')
                parse_result = LUAUC_NUMBER_MALFORMED;
            else if (errno == ERANGE)
                parse_result = LUAUC_NUMBER_INT_OVERFLOW;
        }
        else
        {
            unsigned_integer = strtoull(number, &end, base);
            integer = (int64_t)unsigned_integer;
            if (*end != 'i' || end[1] != '\0')
                parse_result = LUAUC_NUMBER_MALFORMED;
            else if (errno == ERANGE)
                parse_result = base == 2 ? LUAUC_NUMBER_BIN_OVERFLOW : LUAUC_NUMBER_HEX_OVERFLOW;
        }
        __luauc_parser_next(parser);
        if (parse_result == LUAUC_NUMBER_MALFORMED)
            __luauc_parser_error(parser, location, "Malformed integer");
        if (parse_result != LUAUC_NUMBER_OK)
            __luauc_parser_error(parser, location, "Integer overflow");
        result = __luauc_new_expression(parser, LUAUC_EXPR_CONSTANT_INTEGER, location);
        result->value.constant_integer.value = integer;
        result->value.constant_integer.parse_result = parse_result;
        return result;
    }
    else if (write > 2 && data[0] == '0' && (data[1] == 'b' || data[1] == 'B'))
    {
        unsigned long long integer = strtoull(data + 2, &end, 2);
        if (*end != '\0')
            parse_result = LUAUC_NUMBER_MALFORMED;
        else if (errno == ERANGE)
            parse_result = LUAUC_NUMBER_BIN_OVERFLOW;
        value = (double)integer;
        if (parse_result == LUAUC_NUMBER_OK && integer >= (UINT64_C(1) << 53) &&
            (unsigned long long)value != integer)
            parse_result = LUAUC_NUMBER_IMPRECISE;
    }
    else if (write > 2 && data[0] == '0' && (data[1] == 'x' || data[1] == 'X'))
    {
        unsigned long long integer = strtoull(data, &end, 16);
        if (*end != '\0')
            parse_result = LUAUC_NUMBER_MALFORMED;
        else if (errno == ERANGE)
            parse_result = LUAUC_NUMBER_HEX_OVERFLOW;
        value = (double)integer;
        if (parse_result == LUAUC_NUMBER_OK && integer >= (UINT64_C(1) << 53) &&
            (unsigned long long)value != integer)
            parse_result = LUAUC_NUMBER_IMPRECISE;
    }
    else
    {
        value = strtod(data, &end);
        if (*end != '\0')
            parse_result = LUAUC_NUMBER_MALFORMED;
    }
    __luauc_parser_next(parser);
    if (parse_result == LUAUC_NUMBER_MALFORMED)
        __luauc_parser_error(parser, location, "Malformed number");
    result = __luauc_new_expression(parser, LUAUC_EXPR_CONSTANT_NUMBER, location);
    result->value.constant_number.value = value;
    result->value.constant_number.parse_result = parse_result;
    return result;
}

static luauc_array_t __luauc_parser_copy_string(
    luauc_parser_t* parser, const char* source, size_t length, int quoted
)
{
    luauc_array_t result;
    char* data = luauc_arena_duplicate(parser->arena, source, length);
    if (data == NULL)
        longjmp(parser->error_jump, 1);
    if (quoted)
    {
        if (!luauc_fixup_quoted_string(data, &length))
            __luauc_parser_error(parser, parser->lexer.token.location, "String literal contains malformed escape sequence");
    }
    else
        luauc_fixup_multiline_string(data, &length);
    result.data = data;
    result.size = length;
    return result;
}

static luauc_ast_expr_t* __luauc_parse_string(luauc_parser_t* parser)
{
    luauc_token_t token = parser->lexer.token;
    luauc_string_quote_style_t style =
        token.type == LUAUC_TOKEN_RAW_STRING ? LUAUC_STRING_QUOTED_RAW :
        token.type == LUAUC_TOKEN_QUOTED_STRING && luauc_token_get_quote_style(&token) == LUAUC_QUOTE_SINGLE ?
            LUAUC_STRING_QUOTED_SINGLE : LUAUC_STRING_QUOTED_SIMPLE;
    luauc_array_t value = __luauc_parser_copy_string(
        parser, token.value.data, token.length, token.type != LUAUC_TOKEN_RAW_STRING
    );
    luauc_ast_expr_t* result = __luauc_new_expression(parser, LUAUC_EXPR_CONSTANT_STRING, token.location);
    result->value.constant_string.value = value;
    result->value.constant_string.quote_style = style;
    __luauc_parser_next(parser);
    return result;
}

static luauc_ast_expr_t* __luauc_parse_interpolated_string(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_location_t end = start;
    luauc_vector_t strings;
    luauc_vector_t expressions;
    luauc_ast_expr_t* result;
    luauc_vector_init(&strings, sizeof(luauc_array_t), parser->allocator);
    luauc_vector_init(&expressions, sizeof(luauc_ast_expr_t*), parser->allocator);

    for (;;)
    {
        luauc_token_t token = parser->lexer.token;
        luauc_array_t string;
        if (token.type != LUAUC_TOKEN_INTERP_STRING_BEGIN && token.type != LUAUC_TOKEN_INTERP_STRING_MID &&
            token.type != LUAUC_TOKEN_INTERP_STRING_END && token.type != LUAUC_TOKEN_INTERP_STRING_SIMPLE)
            __luauc_parser_error(parser, token.location, "Malformed interpolated string");
        string = __luauc_parser_copy_string(parser, token.value.data, token.length, 1);
        luauc_vector_push(&strings, &string);
        end = token.location;
        __luauc_parser_next(parser);
        if (token.type == LUAUC_TOKEN_INTERP_STRING_END || token.type == LUAUC_TOKEN_INTERP_STRING_SIMPLE)
            break;
        {
            luauc_ast_expr_t* expression = __luauc_parse_expression(parser, 0);
            luauc_vector_push(&expressions, &expression);
        }
        if (parser->lexer.token.type != LUAUC_TOKEN_INTERP_STRING_MID &&
            parser->lexer.token.type != LUAUC_TOKEN_INTERP_STRING_END)
            __luauc_parser_error(
                parser, parser->lexer.token.location, "Malformed interpolated string; did you forget to add a '}'?"
            );
    }
    result = __luauc_new_expression(parser, LUAUC_EXPR_INTERP_STRING, __luauc_join_locations(start, end));
    result->value.interpolated_string.strings = __luauc_parser_copy_array(parser, &strings);
    result->value.interpolated_string.expressions = __luauc_parser_copy_array(parser, &expressions);
    luauc_vector_destroy(&strings);
    luauc_vector_destroy(&expressions);
    return result;
}

static luauc_ast_expr_t* __luauc_parse_table(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_location_t end;
    luauc_vector_t items;
    luauc_ast_expr_t* result;
    luauc_vector_init(&items, sizeof(luauc_ast_table_item_t), parser->allocator);
    __luauc_parser_next(parser);
    while (parser->lexer.token.type != (luauc_token_type_t)'}')
    {
        luauc_ast_table_item_t item;
        memset(&item, 0, sizeof(item));
        if (__luauc_parser_accept(parser, (luauc_token_type_t)'['))
        {
            item.kind = LUAUC_TABLE_ITEM_GENERAL;
            item.key = __luauc_parse_expression(parser, 0);
            __luauc_parser_expect(parser, (luauc_token_type_t)']', "table field");
            __luauc_parser_expect(parser, (luauc_token_type_t)'=', "table field");
            item.value = __luauc_parse_expression(parser, 0);
        }
        else if (parser->lexer.token.type == LUAUC_TOKEN_NAME &&
                 luauc_lexer_lookahead(&parser->lexer).type == (luauc_token_type_t)'=')
        {
            luauc_location_t name_location;
            luauc_name_t name = __luauc_parser_parse_name(parser, &name_location, "table field");
            luauc_ast_expr_t* key = __luauc_new_expression(parser, LUAUC_EXPR_CONSTANT_STRING, name_location);
            key->value.constant_string.value.data = (void*)name.value;
            key->value.constant_string.value.size = strlen(name.value);
            key->value.constant_string.quote_style = LUAUC_STRING_UNQUOTED;
            __luauc_parser_expect(parser, (luauc_token_type_t)'=', "table field");
            item.kind = LUAUC_TABLE_ITEM_RECORD;
            item.key = key;
            item.value = __luauc_parse_expression(parser, 0);
            if (item.value->kind == LUAUC_EXPR_FUNCTION)
                item.value->value.function.debugname = name;
        }
        else
        {
            item.kind = LUAUC_TABLE_ITEM_LIST;
            item.value = __luauc_parse_expression(parser, 0);
        }
        luauc_vector_push(&items, &item);
        if (!__luauc_parser_accept(parser, (luauc_token_type_t)',') &&
            !__luauc_parser_accept(parser, (luauc_token_type_t)';') &&
            parser->lexer.token.type != (luauc_token_type_t)'}')
            __luauc_parser_error(parser, parser->lexer.token.location, "Expected ',' after table constructor element");
    }
    end = parser->lexer.token.location;
    __luauc_parser_next(parser);
    result = __luauc_new_expression(parser, LUAUC_EXPR_TABLE, __luauc_join_locations(start, end));
    result->value.table.items = __luauc_parser_copy_array(parser, &items);
    luauc_vector_destroy(&items);
    return result;
}

static luauc_ast_expr_t* __luauc_parse_if_expression(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_ast_expr_t* condition;
    luauc_ast_expr_t* true_expression;
    luauc_ast_expr_t* false_expression;
    luauc_ast_expr_t* result;
    __luauc_parser_next(parser);
    condition = __luauc_parse_expression(parser, 0);
    __luauc_parser_expect(parser, LUAUC_TOKEN_THEN, "if then else expression");
    true_expression = __luauc_parse_expression(parser, 0);
    if (parser->lexer.token.type == LUAUC_TOKEN_ELSEIF)
        false_expression = __luauc_parse_if_expression(parser);
    else
    {
        __luauc_parser_expect(parser, LUAUC_TOKEN_ELSE, "if then else expression");
        false_expression = __luauc_parse_expression(parser, 0);
    }
    result = __luauc_new_expression(parser, LUAUC_EXPR_IF_ELSE, __luauc_join_locations(start, false_expression->location));
    result->value.if_else.condition = condition;
    result->value.if_else.has_then = 1;
    result->value.if_else.true_expression = true_expression;
    result->value.if_else.has_else = 1;
    result->value.if_else.false_expression = false_expression;
    return result;
}

static luauc_ast_expr_t* __luauc_parse_simple(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_ast_expr_t* result;
    switch ((int)parser->lexer.token.type)
    {
    case LUAUC_TOKEN_NIL:
        __luauc_parser_next(parser);
        return __luauc_new_expression(parser, LUAUC_EXPR_CONSTANT_NIL, start);
    case LUAUC_TOKEN_TRUE:
    case LUAUC_TOKEN_FALSE:
        result = __luauc_new_expression(parser, LUAUC_EXPR_CONSTANT_BOOL, start);
        result->value.constant_bool.value = parser->lexer.token.type == LUAUC_TOKEN_TRUE;
        __luauc_parser_next(parser);
        return result;
    case LUAUC_TOKEN_FUNCTION:
        __luauc_parser_next(parser);
        {
            luauc_name_t empty = {NULL};
            return __luauc_parse_function_body(parser, start, empty, 0, NULL, 0);
        }
    case LUAUC_TOKEN_NUMBER:
        return __luauc_parse_number(parser);
    case LUAUC_TOKEN_RAW_STRING:
    case LUAUC_TOKEN_QUOTED_STRING:
    case LUAUC_TOKEN_INTERP_STRING_SIMPLE:
        return __luauc_parse_string(parser);
    case LUAUC_TOKEN_INTERP_STRING_BEGIN:
        return __luauc_parse_interpolated_string(parser);
    case LUAUC_TOKEN_DOT3:
        if (!__luauc_parser_current_function(parser)->vararg)
            __luauc_parser_error(parser, start, "Cannot use '...' outside of a vararg function");
        __luauc_parser_next(parser);
        return __luauc_new_expression(parser, LUAUC_EXPR_VARARGS, start);
    case (luauc_token_type_t)'{':
        return __luauc_parse_table(parser);
    case LUAUC_TOKEN_IF:
        return __luauc_parse_if_expression(parser);
    case LUAUC_TOKEN_BROKEN_STRING:
        __luauc_parser_error(parser, start, "Malformed string; did you forget to finish it?");
        return NULL;
    default:
        return __luauc_parse_primary(parser, 0);
    }
}

static luauc_ast_expr_t* __luauc_parse_prefix(luauc_parser_t* parser)
{
    if (parser->lexer.token.type == (luauc_token_type_t)'(')
    {
        luauc_location_t start = parser->lexer.token.location;
        luauc_location_t end;
        luauc_ast_expr_t* inner;
        luauc_ast_expr_t* result;
        __luauc_parser_next(parser);
        inner = __luauc_parse_expression(parser, 0);
        end = parser->lexer.token.location;
        __luauc_parser_expect(parser, (luauc_token_type_t)')', "expression");
        result = __luauc_new_expression(parser, LUAUC_EXPR_GROUP, __luauc_join_locations(start, end));
        result->value.group.expression = inner;
        return result;
    }
    else
    {
        luauc_location_t location;
        luauc_name_t name = __luauc_parser_parse_name(parser, &location, "expression");
        luauc_ast_local_t* local = __luauc_parser_find_local(parser, name);
        luauc_ast_expr_t* result = __luauc_new_expression(
            parser, local != NULL ? LUAUC_EXPR_LOCAL : LUAUC_EXPR_GLOBAL, location
        );
        if (local != NULL)
        {
            result->value.local.local = local;
            result->value.local.upvalue = local->function_depth != parser->functions.size - 1;
        }
        else
            result->value.global.name = name;
        return result;
    }
}

static luauc_ast_expr_t* __luauc_parse_call(luauc_parser_t* parser, luauc_ast_expr_t* function, int self)
{
    luauc_vector_t arguments;
    luauc_location_t argument_location = {{0, 0}, {0, 0}};
    luauc_location_t end = {{0, 0}, {0, 0}};
    luauc_ast_expr_t* result;
    luauc_vector_init(&arguments, sizeof(luauc_ast_expr_t*), parser->allocator);
    if (parser->lexer.token.type == (luauc_token_type_t)'(')
    {
        argument_location.begin = parser->lexer.token.location.end;
        __luauc_parser_next(parser);
        if (parser->lexer.token.type != (luauc_token_type_t)')')
            __luauc_parser_parse_expression_list(parser, &arguments);
        argument_location.end = parser->lexer.token.location.end;
        end = parser->lexer.token.location;
        __luauc_parser_expect(parser, (luauc_token_type_t)')', "function call");
    }
    else if (parser->lexer.token.type == (luauc_token_type_t)'{')
    {
        luauc_ast_expr_t* argument = __luauc_parse_table(parser);
        luauc_vector_push(&arguments, &argument);
        argument_location = argument->location;
        end = argument->location;
    }
    else if (parser->lexer.token.type == LUAUC_TOKEN_RAW_STRING ||
             parser->lexer.token.type == LUAUC_TOKEN_QUOTED_STRING)
    {
        luauc_ast_expr_t* argument = __luauc_parse_string(parser);
        luauc_vector_push(&arguments, &argument);
        argument_location = argument->location;
        end = argument->location;
    }
    else
    {
        luauc_vector_destroy(&arguments);
        __luauc_parser_error(
            parser,
            parser->lexer.token.location,
            "Expected '(', '{' or <string> when parsing function call, got %s",
            luauc_token_name(parser->lexer.token.type)
        );
    }
    result = __luauc_new_expression(parser, LUAUC_EXPR_CALL, __luauc_join_locations(function->location, end));
    result->value.call.function = function;
    result->value.call.arguments = __luauc_parser_copy_array(parser, &arguments);
    result->value.call.self = self;
    result->value.call.argument_location = argument_location;
    luauc_vector_destroy(&arguments);
    return result;
}

static luauc_array_t __luauc_parse_type_instantiation(luauc_parser_t* parser)
{
    luauc_vector_t arguments;
    luauc_array_t result;
    __luauc_parser_expect(parser, (luauc_token_type_t)'<', "type instantiation");
    __luauc_parser_expect(parser, (luauc_token_type_t)'<', "type instantiation");
    luauc_vector_init(&arguments, sizeof(luauc_type_or_pack_t), parser->allocator);
    if (parser->lexer.token.type != (luauc_token_type_t)'>')
    {
        do
        {
            luauc_type_or_pack_t argument;
            memset(&argument, 0, sizeof(argument));
            if (parser->lexer.token.type == (luauc_token_type_t)'(')
                argument.type_pack = __luauc_parse_return_type(parser);
            else if (parser->lexer.token.type == LUAUC_TOKEN_DOT3 ||
                     (parser->lexer.token.type == LUAUC_TOKEN_NAME &&
                      luauc_lexer_lookahead(&parser->lexer).type == LUAUC_TOKEN_DOT3))
            {
                if (parser->lexer.token.type == LUAUC_TOKEN_DOT3)
                {
                    luauc_location_t start = parser->lexer.token.location;
                    luauc_ast_type_t* type;
                    __luauc_parser_next(parser);
                    type = __luauc_parse_type(parser);
                    argument.type_pack =
                        __luauc_new_type_pack(parser, LUAUC_TYPE_PACK_VARIADIC, __luauc_join_locations(start, type->location));
                    argument.type_pack->value.variadic_type = type;
                }
                else
                {
                    luauc_location_t name_location;
                    luauc_name_t name = __luauc_parser_parse_name(parser, &name_location, "generic type pack");
                    luauc_location_t end = parser->lexer.token.location;
                    __luauc_parser_expect(parser, LUAUC_TOKEN_DOT3, "generic type pack");
                    argument.type_pack =
                        __luauc_new_type_pack(parser, LUAUC_TYPE_PACK_GENERIC, __luauc_join_locations(name_location, end));
                    argument.type_pack->value.generic_name = name;
                }
            }
            else
                argument.type = __luauc_parse_type(parser);
            luauc_vector_push(&arguments, &argument);
        } while (__luauc_parser_accept(parser, (luauc_token_type_t)','));
    }
    __luauc_parser_expect(parser, (luauc_token_type_t)'>', "type instantiation");
    __luauc_parser_expect(parser, (luauc_token_type_t)'>', "type instantiation");
    result = __luauc_parser_copy_array(parser, &arguments);
    luauc_vector_destroy(&arguments);
    return result;
}

static luauc_ast_expr_t* __luauc_parse_primary(luauc_parser_t* parser, int as_statement)
{
    luauc_position_t start = parser->lexer.token.location.begin;
    luauc_ast_expr_t* expression = __luauc_parse_prefix(parser);
    (void)as_statement;
    for (;;)
    {
        if (__luauc_parser_accept(parser, (luauc_token_type_t)'.'))
        {
            luauc_location_t index_location;
            luauc_name_t index = __luauc_parser_parse_name(parser, &index_location, "field name");
            luauc_ast_expr_t* result =
                __luauc_new_expression(parser, LUAUC_EXPR_INDEX_NAME, __luauc_location_make(start, index_location.end));
            result->value.index_name.expression = expression;
            result->value.index_name.index = index;
            result->value.index_name.index_location = index_location;
            result->value.index_name.operator_character = '.';
            expression = result;
        }
        else if (__luauc_parser_accept(parser, (luauc_token_type_t)'['))
        {
            luauc_ast_expr_t* index = __luauc_parse_expression(parser, 0);
            luauc_location_t end = parser->lexer.token.location;
            luauc_ast_expr_t* result;
            __luauc_parser_expect(parser, (luauc_token_type_t)']', "index expression");
            result = __luauc_new_expression(parser, LUAUC_EXPR_INDEX_EXPR, __luauc_location_make(start, end.end));
            result->value.index_expr.expression = expression;
            result->value.index_expr.index = index;
            expression = result;
        }
        else if (__luauc_parser_accept(parser, (luauc_token_type_t)':'))
        {
            luauc_location_t index_location;
            luauc_name_t index = __luauc_parser_parse_name(parser, &index_location, "method name");
            luauc_ast_expr_t* function =
                __luauc_new_expression(parser, LUAUC_EXPR_INDEX_NAME, __luauc_location_make(start, index_location.end));
            function->value.index_name.expression = expression;
            function->value.index_name.index = index;
            function->value.index_name.index_location = index_location;
            function->value.index_name.operator_character = ':';
            if (parser->lexer.token.type == (luauc_token_type_t)'<' &&
                luauc_lexer_lookahead(&parser->lexer).type == (luauc_token_type_t)'<')
            {
                luauc_array_t types = __luauc_parse_type_instantiation(parser);
                expression = __luauc_parse_call(parser, function, 1);
                expression->value.call.type_arguments = types;
            }
            else
                expression = __luauc_parse_call(parser, function, 1);
        }
        else if (parser->lexer.token.type == (luauc_token_type_t)'(' ||
                 parser->lexer.token.type == (luauc_token_type_t)'{' ||
                 parser->lexer.token.type == LUAUC_TOKEN_RAW_STRING ||
                 parser->lexer.token.type == LUAUC_TOKEN_QUOTED_STRING)
        {
            if (!as_statement && parser->lexer.token.type == (luauc_token_type_t)'(' &&
                expression->location.end.line != parser->lexer.token.location.begin.line)
                __luauc_parser_error(
                    parser,
                    parser->lexer.token.location,
                    "Ambiguous syntax: this looks like an argument list for a function call, "
                    "but could also be a start of new statement; use ';' to separate statements"
                );
            expression = __luauc_parse_call(parser, expression, 0);
        }
        else if (parser->lexer.token.type == (luauc_token_type_t)'<' &&
                 luauc_lexer_lookahead(&parser->lexer).type == (luauc_token_type_t)'<')
        {
            luauc_ast_expr_t* instantiated;
            luauc_array_t types = __luauc_parse_type_instantiation(parser);
            instantiated = __luauc_new_expression(
                parser,
                LUAUC_EXPR_INSTANTIATE,
                __luauc_location_make(expression->location.begin, parser->lexer.previous_location.end)
            );
            instantiated->value.instantiate.expression = expression;
            instantiated->value.instantiate.type_arguments = types;
            expression = instantiated;
        }
        else
            break;
    }
    return expression;
}

static luauc_ast_expr_t* __luauc_parse_assertion(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_ast_expr_t* expression = __luauc_parse_simple(parser);
    if (__luauc_parser_accept(parser, LUAUC_TOKEN_DOUBLE_COLON))
    {
        luauc_ast_type_t* annotation = __luauc_parse_type(parser);
        luauc_ast_expr_t* result =
            __luauc_new_expression(parser, LUAUC_EXPR_TYPE_ASSERTION, __luauc_join_locations(start, annotation->location));
        result->value.type_assertion.expression = expression;
        result->value.type_assertion.annotation = annotation;
        return result;
    }
    return expression;
}

static luauc_ast_expr_t* __luauc_parse_expression(luauc_parser_t* parser, unsigned int limit)
{
    static const unsigned char __priorities[LUAUC_BINARY_COUNT][2] = {
        {6, 6}, {6, 6}, {7, 7}, {7, 7}, {7, 7}, {7, 7}, {10, 9}, {5, 4},
        {3, 3}, {3, 3}, {3, 3}, {3, 3}, {3, 3}, {3, 3}, {2, 2}, {1, 1}
    };
    const unsigned int unary_priority = 8;
    luauc_location_t start = parser->lexer.token.location;
    luauc_unary_op_t unary;
    luauc_binary_op_t binary;
    luauc_ast_expr_t* expression;

    if (++parser->recursion > 1000)
        __luauc_parser_error(parser, start, "Exceeded allowed recursion depth; simplify your expression");
    if (__luauc_parse_unary_operator(parser->lexer.token.type, &unary))
    {
        luauc_ast_expr_t* operand;
        __luauc_parser_next(parser);
        operand = __luauc_parse_expression(parser, unary_priority);
        expression = __luauc_new_expression(parser, LUAUC_EXPR_UNARY, __luauc_join_locations(start, operand->location));
        expression->value.unary.op = unary;
        expression->value.unary.expression = operand;
    }
    else
        expression = __luauc_parse_assertion(parser);

    while (__luauc_parse_binary_operator(parser->lexer.token.type, &binary) &&
           __priorities[binary][0] > limit)
    {
        luauc_ast_expr_t* right;
        luauc_ast_expr_t* combined;
        __luauc_parser_next(parser);
        right = __luauc_parse_expression(parser, __priorities[binary][1]);
        combined = __luauc_new_expression(parser, LUAUC_EXPR_BINARY, __luauc_join_locations(start, right->location));
        combined->value.binary.op = binary;
        combined->value.binary.left = expression;
        combined->value.binary.right = right;
        expression = combined;
    }
    parser->recursion--;
    return expression;
}

static luauc_ast_type_t* __luauc_parse_table_type(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_location_t end;
    luauc_vector_t properties;
    luauc_table_indexer_t* indexer = NULL;
    luauc_ast_type_t* result;
    luauc_vector_init(&properties, sizeof(luauc_table_property_t), parser->allocator);
    __luauc_parser_next(parser);
    while (parser->lexer.token.type != (luauc_token_type_t)'}')
    {
        if (__luauc_parser_accept(parser, (luauc_token_type_t)'['))
        {
            luauc_ast_type_t* index_type = __luauc_parse_type(parser);
            luauc_ast_type_t* result_type;
            __luauc_parser_expect(parser, (luauc_token_type_t)']', "table field");
            __luauc_parser_expect(parser, (luauc_token_type_t)':', "table field");
            result_type = __luauc_parse_type(parser);
            if (indexer != NULL)
                __luauc_parser_error(parser, result_type->location, "Cannot have more than one table indexer");
            indexer = LUAUC_NEW(parser, luauc_table_indexer_t);
            indexer->index_type = index_type;
            indexer->result_type = result_type;
            indexer->location = __luauc_join_locations(index_type->location, result_type->location);
            indexer->access = LUAUC_TABLE_READ_WRITE;
        }
        else if (properties.size == 0 && indexer == NULL &&
                 !(parser->lexer.token.type == LUAUC_TOKEN_NAME &&
                   luauc_lexer_lookahead(&parser->lexer).type == (luauc_token_type_t)':'))
        {
            luauc_token_type_t ignored;
            luauc_ast_type_t* element = __luauc_parse_type(parser);
            luauc_ast_type_t* number = __luauc_new_type(parser, LUAUC_TYPE_REFERENCE, start);
            number->value.reference.name = luauc_name_table_add(parser->names, "number", 6, &ignored);
            number->value.reference.name_location = start;
            indexer = LUAUC_NEW(parser, luauc_table_indexer_t);
            indexer->index_type = number;
            indexer->result_type = element;
            indexer->location = element->location;
            indexer->access = LUAUC_TABLE_READ_WRITE;
        }
        else
        {
            luauc_table_property_t property;
            property.name = __luauc_parser_parse_name(parser, &property.location, "table field");
            __luauc_parser_expect(parser, (luauc_token_type_t)':', "table field");
            property.type = __luauc_parse_type(parser);
            property.access = LUAUC_TABLE_READ_WRITE;
            property.access_location.present = 0;
            luauc_vector_push(&properties, &property);
        }
        if (!__luauc_parser_accept(parser, (luauc_token_type_t)',') &&
            !__luauc_parser_accept(parser, (luauc_token_type_t)';') &&
            parser->lexer.token.type != (luauc_token_type_t)'}')
            __luauc_parser_error(parser, parser->lexer.token.location, "Expected ',' after table type field");
    }
    end = parser->lexer.token.location;
    __luauc_parser_next(parser);
    result = __luauc_new_type(parser, LUAUC_TYPE_TABLE, __luauc_join_locations(start, end));
    result->value.table.properties = __luauc_parser_copy_array(parser, &properties);
    result->value.table.indexer = indexer;
    luauc_vector_destroy(&properties);
    return result;
}

static luauc_ast_type_t* __luauc_parse_parenthesized_type(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_location_t close;
    luauc_vector_t types;
    luauc_vector_t names;
    luauc_ast_type_pack_t* tail = NULL;
    luauc_ast_type_t* result;
    luauc_vector_init(&types, sizeof(luauc_ast_type_t*), parser->allocator);
    luauc_vector_init(&names, sizeof(luauc_optional_argument_name_t), parser->allocator);
    __luauc_parser_next(parser);
    while (parser->lexer.token.type != (luauc_token_type_t)')')
    {
        luauc_ast_type_t* type;
        luauc_optional_argument_name_t argument_name;
        memset(&argument_name, 0, sizeof(argument_name));
        if (parser->lexer.token.type == LUAUC_TOKEN_DOT3)
        {
            luauc_location_t pack_start = parser->lexer.token.location;
            __luauc_parser_next(parser);
            type = __luauc_parse_type(parser);
            tail = __luauc_new_type_pack(parser, LUAUC_TYPE_PACK_VARIADIC, __luauc_join_locations(pack_start, type->location));
            tail->value.variadic_type = type;
            break;
        }
        if (parser->lexer.token.type == LUAUC_TOKEN_NAME &&
            luauc_lexer_lookahead(&parser->lexer).type == (luauc_token_type_t)':')
        {
            argument_name.present = 1;
            argument_name.value.name =
                __luauc_parser_parse_name(parser, &argument_name.value.location, "argument name");
            __luauc_parser_expect(parser, (luauc_token_type_t)':', "argument type");
        }
        type = __luauc_parse_type(parser);
        luauc_vector_push(&types, &type);
        luauc_vector_push(&names, &argument_name);
        if (!__luauc_parser_accept(parser, (luauc_token_type_t)','))
            break;
    }
    close = parser->lexer.token.location;
    __luauc_parser_expect(parser, (luauc_token_type_t)')', "type annotation");

    if (!__luauc_parser_accept(parser, LUAUC_TOKEN_SKINNY_ARROW) && types.size == 1 && tail == NULL)
    {
        result = __luauc_new_type(parser, LUAUC_TYPE_GROUP, __luauc_join_locations(start, close));
        result->value.group.type = *(luauc_ast_type_t**)luauc_vector_at(&types, 0);
    }
    else
    {
        luauc_ast_type_pack_t* returns;
        if (parser->lexer.previous_location.begin.line != close.begin.line ||
            parser->lexer.previous_location.begin.column != close.begin.column)
        {
            /* the arrow was consumed */
        }
        else
            __luauc_parser_error(parser, parser->lexer.token.location, "Expected '->' when parsing function type");
        returns = __luauc_parse_return_type(parser);
        result = __luauc_new_type(parser, LUAUC_TYPE_FUNCTION, __luauc_join_locations(start, returns->location));
        result->value.function.argument_types.types = __luauc_parser_copy_array(parser, &types);
        result->value.function.argument_types.tail_type = tail;
        result->value.function.argument_names = __luauc_parser_copy_array(parser, &names);
        result->value.function.return_types = returns;
    }
    luauc_vector_destroy(&types);
    luauc_vector_destroy(&names);
    return result;
}

static luauc_ast_type_t* __luauc_parse_simple_type(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_ast_type_t* result;
    if (parser->lexer.token.type == LUAUC_TOKEN_NIL)
    {
        luauc_token_type_t ignored;
        result = __luauc_new_type(parser, LUAUC_TYPE_REFERENCE, start);
        result->value.reference.name = luauc_name_table_add(parser->names, "nil", 3, &ignored);
        result->value.reference.name_location = start;
        __luauc_parser_next(parser);
        return result;
    }
    if (parser->lexer.token.type == LUAUC_TOKEN_TRUE || parser->lexer.token.type == LUAUC_TOKEN_FALSE)
    {
        result = __luauc_new_type(parser, LUAUC_TYPE_SINGLETON_BOOL, start);
        result->value.singleton_bool.value = parser->lexer.token.type == LUAUC_TOKEN_TRUE;
        __luauc_parser_next(parser);
        return result;
    }
    if (parser->lexer.token.type == LUAUC_TOKEN_RAW_STRING ||
        parser->lexer.token.type == LUAUC_TOKEN_QUOTED_STRING)
    {
        luauc_token_t token = parser->lexer.token;
        result = __luauc_new_type(parser, LUAUC_TYPE_SINGLETON_STRING, start);
        result->value.singleton_string.value = __luauc_parser_copy_string(
            parser, token.value.data, token.length, token.type != LUAUC_TOKEN_RAW_STRING
        );
        __luauc_parser_next(parser);
        return result;
    }
    if (parser->lexer.token.type == LUAUC_TOKEN_NAME)
    {
        luauc_location_t name_location;
        luauc_name_t name = __luauc_parser_parse_name(parser, &name_location, "type name");
        if (__luauc_name_is(name, "typeof"))
        {
            luauc_ast_expr_t* expression;
            luauc_location_t end;
            __luauc_parser_expect(parser, (luauc_token_type_t)'(', "typeof type");
            expression = __luauc_parse_expression(parser, 0);
            end = parser->lexer.token.location;
            __luauc_parser_expect(parser, (luauc_token_type_t)')', "typeof type");
            result = __luauc_new_type(parser, LUAUC_TYPE_TYPEOF, __luauc_join_locations(start, end));
            result->value.typeof_type.expression = expression;
            return result;
        }
        result = __luauc_new_type(parser, LUAUC_TYPE_REFERENCE, name_location);
        if (__luauc_parser_accept(parser, (luauc_token_type_t)'.'))
        {
            result->value.reference.has_prefix = 1;
            result->value.reference.prefix = name;
            result->value.reference.prefix_location.present = 1;
            result->value.reference.prefix_location.value = name_location;
            name = __luauc_parser_parse_name(parser, &name_location, "field name");
            result->location.end = name_location.end;
        }
        result->value.reference.name = name;
        result->value.reference.name_location = name_location;
        if (__luauc_parser_accept(parser, (luauc_token_type_t)'<'))
        {
            luauc_vector_t parameters;
            luauc_vector_init(&parameters, sizeof(luauc_type_or_pack_t), parser->allocator);
            do
            {
                luauc_type_or_pack_t parameter;
                memset(&parameter, 0, sizeof(parameter));
                parameter.type = __luauc_parse_type(parser);
                luauc_vector_push(&parameters, &parameter);
            } while (__luauc_parser_accept(parser, (luauc_token_type_t)','));
            __luauc_parser_expect(parser, (luauc_token_type_t)'>', "type parameters");
            result->value.reference.has_parameter_list = 1;
            result->value.reference.parameters = __luauc_parser_copy_array(parser, &parameters);
            result->location.end = parser->lexer.previous_location.end;
            luauc_vector_destroy(&parameters);
        }
        return result;
    }
    if (parser->lexer.token.type == (luauc_token_type_t)'{')
        return __luauc_parse_table_type(parser);
    if (parser->lexer.token.type == (luauc_token_type_t)'(')
        return __luauc_parse_parenthesized_type(parser);
    __luauc_parser_error(
        parser, parser->lexer.token.location, "Expected type, got %s", luauc_token_name(parser->lexer.token.type)
    );
    return NULL;
}

static luauc_ast_type_t* __luauc_parse_type(luauc_parser_t* parser)
{
    luauc_location_t start = parser->lexer.token.location;
    luauc_vector_t parts;
    luauc_ast_type_t* first = NULL;
    int is_union = 0;
    int is_intersection = 0;
    luauc_vector_init(&parts, sizeof(luauc_ast_type_t*), parser->allocator);

    if (parser->lexer.token.type != (luauc_token_type_t)'|' &&
        parser->lexer.token.type != (luauc_token_type_t)'&')
    {
        first = __luauc_parse_simple_type(parser);
        luauc_vector_push(&parts, &first);
    }
    for (;;)
    {
        if (__luauc_parser_accept(parser, (luauc_token_type_t)'?'))
        {
            luauc_ast_type_t* optional = __luauc_new_type(parser, LUAUC_TYPE_OPTIONAL, parser->lexer.previous_location);
            luauc_vector_push(&parts, &optional);
            is_union = 1;
        }
        else if (__luauc_parser_accept(parser, (luauc_token_type_t)'|'))
        {
            luauc_ast_type_t* part = __luauc_parse_simple_type(parser);
            luauc_vector_push(&parts, &part);
            is_union = 1;
        }
        else if (__luauc_parser_accept(parser, (luauc_token_type_t)'&'))
        {
            luauc_ast_type_t* part = __luauc_parse_simple_type(parser);
            luauc_vector_push(&parts, &part);
            is_intersection = 1;
        }
        else
            break;
    }
    if (is_union && is_intersection)
        __luauc_parser_error(
            parser,
            start,
            "Mixing union and intersection types is not allowed; consider wrapping in parentheses."
        );
    if (!is_union && !is_intersection)
    {
        luauc_vector_destroy(&parts);
        return first;
    }
    {
        luauc_ast_type_t* last = *(luauc_ast_type_t**)luauc_vector_at(&parts, parts.size - 1);
        luauc_ast_type_t* result = __luauc_new_type(
            parser,
            is_union ? LUAUC_TYPE_UNION : LUAUC_TYPE_INTERSECTION,
            __luauc_location_make(start.begin, last->location.end)
        );
        result->value.aggregate.types = __luauc_parser_copy_array(parser, &parts);
        luauc_vector_destroy(&parts);
        return result;
    }
}

static luauc_ast_type_pack_t* __luauc_parse_return_type(luauc_parser_t* parser)
{
    if (parser->lexer.token.type == LUAUC_TOKEN_NAME &&
        luauc_lexer_lookahead(&parser->lexer).type == LUAUC_TOKEN_DOT3)
    {
        luauc_location_t name_location;
        luauc_name_t name = __luauc_parser_parse_name(parser, &name_location, "generic type pack");
        luauc_location_t end = parser->lexer.token.location;
        luauc_ast_type_pack_t* pack;
        __luauc_parser_expect(parser, LUAUC_TOKEN_DOT3, "generic type pack");
        pack = __luauc_new_type_pack(parser, LUAUC_TYPE_PACK_GENERIC, __luauc_join_locations(name_location, end));
        pack->value.generic_name = name;
        return pack;
    }
    if (__luauc_parser_accept(parser, LUAUC_TOKEN_DOT3))
    {
        luauc_ast_type_t* type = __luauc_parse_type(parser);
        luauc_ast_type_pack_t* pack =
            __luauc_new_type_pack(parser, LUAUC_TYPE_PACK_VARIADIC, type->location);
        pack->value.variadic_type = type;
        return pack;
    }
    if (parser->lexer.token.type == (luauc_token_type_t)'(')
    {
        luauc_location_t start = parser->lexer.token.location;
        luauc_location_t end;
        luauc_vector_t types;
        luauc_ast_type_pack_t* pack;
        luauc_vector_init(&types, sizeof(luauc_ast_type_t*), parser->allocator);
        __luauc_parser_next(parser);
        if (parser->lexer.token.type != (luauc_token_type_t)')')
        {
            do
            {
                luauc_ast_type_t* type = __luauc_parse_type(parser);
                luauc_vector_push(&types, &type);
            } while (__luauc_parser_accept(parser, (luauc_token_type_t)','));
        }
        end = parser->lexer.token.location;
        __luauc_parser_expect(parser, (luauc_token_type_t)')', "return type");
        pack = __luauc_new_type_pack(parser, LUAUC_TYPE_PACK_EXPLICIT, __luauc_join_locations(start, end));
        pack->value.explicit_types.types = __luauc_parser_copy_array(parser, &types);
        luauc_vector_destroy(&types);
        return pack;
    }
    else
    {
        luauc_ast_type_t* type = __luauc_parse_type(parser);
        luauc_ast_type_pack_t* pack =
            __luauc_new_type_pack(parser, LUAUC_TYPE_PACK_EXPLICIT, type->location);
        pack->value.explicit_types.types =
            __luauc_parser_copy_single(parser, &type, sizeof(type), _Alignof(luauc_ast_type_t*));
        return pack;
    }
}

static luauc_ast_stat_t* __luauc_parse_type_alias(luauc_parser_t* parser, luauc_location_t start, int exported)
{
    luauc_location_t name_location;
    luauc_name_t name = __luauc_parser_parse_name(parser, &name_location, "type alias");
    luauc_ast_type_t* type;
    luauc_ast_stat_t* result;
    __luauc_parser_expect(parser, (luauc_token_type_t)'=', "type alias");
    type = __luauc_parse_type(parser);
    result = __luauc_new_statement(parser, LUAUC_STAT_TYPE_ALIAS, __luauc_join_locations(start, type->location));
    result->value.type_alias.name = name;
    result->value.type_alias.name_location = name_location;
    result->value.type_alias.type = type;
    result->value.type_alias.exported = exported;
    return result;
}

static int __luauc_parser_init(
    luauc_parser_t* parser,
    const char* source,
    size_t size,
    luauc_arena_t* arena,
    luauc_name_table_t* names,
    luauc_allocator_t allocator
)
{
    luauc_parser_function_t top = {1, 0};
    memset(parser, 0, sizeof(*parser));
    parser->arena = arena;
    parser->names = names;
    parser->allocator = allocator.reallocate != NULL ? allocator : luauc_default_allocator();
    parser->hot_comment_header = 1;
    if (!luauc_vector_init(&parser->locals, sizeof(luauc_ast_local_t*), parser->allocator) ||
        !luauc_vector_init(&parser->functions, sizeof(luauc_parser_function_t), parser->allocator) ||
        !luauc_vector_init(&parser->hot_comments, sizeof(luauc_hot_comment_t), parser->allocator) ||
        !luauc_lexer_init(
            &parser->lexer, source, size, names, __luauc_position_make(0, 0), parser->allocator
        ))
        return 0;
    if (luauc_vector_push(&parser->functions, &top) == NULL)
        return 0;
    return 1;
}

static void __luauc_parser_destroy(luauc_parser_t* parser)
{
    luauc_lexer_destroy(&parser->lexer);
    luauc_vector_destroy(&parser->locals);
    luauc_vector_destroy(&parser->functions);
    luauc_vector_destroy(&parser->hot_comments);
    if (parser->error_message != NULL)
        parser->allocator.reallocate(
            parser->allocator.context, parser->error_message, parser->error_capacity, 0
        );
}

int luauc_parse(
    const char* source,
    size_t size,
    luauc_arena_t* arena,
    luauc_name_table_t* names,
    luauc_allocator_t allocator,
    luauc_parse_result_t* result
)
{
    luauc_parser_t parser;
    int jumped;
    if (result == NULL || !__luauc_parser_init(&parser, source, size, arena, names, allocator))
        return 0;
    memset(result, 0, sizeof(*result));
    jumped = setjmp(parser.error_jump);
    if (jumped == 0)
    {
        __luauc_parser_next(&parser);
        parser.hot_comment_header = 0;
        result->root = __luauc_parse_block(&parser, 1);
        if (parser.lexer.token.type != LUAUC_TOKEN_EOF)
            __luauc_parser_error(
                &parser, parser.lexer.token.location, "Expected <eof>, got %s",
                luauc_token_name(parser.lexer.token.type)
            );
        result->lines = parser.lexer.token.location.end.line +
            (size > 0 && source[size - 1] != '\n');
        result->hot_comments = __luauc_parser_copy_array(&parser, &parser.hot_comments);
        __luauc_parser_destroy(&parser);
        return 1;
    }
    else
    {
        result->has_error = 1;
        result->error_location = parser.error_location;
        if (parser.error_message != NULL)
            result->error_message =
                luauc_arena_duplicate(arena, parser.error_message, strlen(parser.error_message));
        else if (parser.failed)
            result->error_message = "out of memory";
        __luauc_parser_destroy(&parser);
        return result->error_message != NULL;
    }
}
