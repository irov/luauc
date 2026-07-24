// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "ast/luauc_ast.h"

static int __luauc_position_less_equal(luauc_position_t left, luauc_position_t right)
{
    return __luauc_position_equal(left, right) || __luauc_position_less(left, right);
}

int luauc_location_encloses(luauc_location_t outer, luauc_location_t inner)
{
    return __luauc_position_less_equal(outer.begin, inner.begin) && __luauc_position_less_equal(inner.end, outer.end);
}

int luauc_location_overlaps(luauc_location_t left, luauc_location_t right)
{
    return __luauc_position_less(left.begin, right.end) && __luauc_position_less(right.begin, left.end);
}

int luauc_location_contains(luauc_location_t location, luauc_position_t position)
{
    return __luauc_position_less_equal(location.begin, position) && __luauc_position_less(position, location.end);
}

int luauc_location_contains_closed(luauc_location_t location, luauc_position_t position)
{
    return __luauc_position_less_equal(location.begin, position) && __luauc_position_less_equal(position, location.end);
}

void luauc_location_extend(luauc_location_t* location, luauc_location_t other)
{
    if (__luauc_position_less(other.begin, location->begin))
        location->begin = other.begin;
    if (__luauc_position_less(location->end, other.end))
        location->end = other.end;
}

int luauc_ast_is_lvalue(const luauc_ast_expr_t* expression)
{
    return expression != NULL &&
        (expression->kind == LUAUC_EXPR_LOCAL || expression->kind == LUAUC_EXPR_GLOBAL ||
         expression->kind == LUAUC_EXPR_INDEX_NAME || expression->kind == LUAUC_EXPR_INDEX_EXPR);
}

int luauc_ast_is_constant_literal(const luauc_ast_expr_t* expression)
{
    if (expression == NULL)
        return 0;
    return expression->kind == LUAUC_EXPR_CONSTANT_NIL || expression->kind == LUAUC_EXPR_CONSTANT_BOOL ||
        expression->kind == LUAUC_EXPR_CONSTANT_NUMBER || expression->kind == LUAUC_EXPR_CONSTANT_INTEGER ||
        expression->kind == LUAUC_EXPR_CONSTANT_STRING;
}

int luauc_ast_is_literal_table(const luauc_ast_expr_t* expression)
{
    return expression != NULL && expression->kind == LUAUC_EXPR_TABLE;
}

luauc_name_t luauc_ast_get_identifier(const luauc_ast_expr_t* expression)
{
    luauc_name_t result = {NULL};
    if (expression == NULL)
        return result;
    if (expression->kind == LUAUC_EXPR_LOCAL)
        return expression->value.local.local->name;
    if (expression->kind == LUAUC_EXPR_GLOBAL)
        return expression->value.global.name;
    if (expression->kind == LUAUC_EXPR_INDEX_NAME)
        return expression->value.index_name.index;
    return result;
}

int luauc_ast_function_has_attribute(const luauc_ast_expr_t* function, luauc_attr_type_t type)
{
    size_t index;
    luauc_ast_attr_t* const* attributes;
    if (function == NULL || function->kind != LUAUC_EXPR_FUNCTION)
        return 0;
    attributes = (luauc_ast_attr_t* const*)function->value.function.attributes.data;
    for (index = 0; index < function->value.function.attributes.size; ++index)
        if (attributes[index]->type == type)
            return 1;
    return 0;
}
