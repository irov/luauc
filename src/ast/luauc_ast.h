// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#ifndef LUAUC_AST_H
#define LUAUC_AST_H

#include "common/luauc_common.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

typedef struct luauc_position_t
{
    unsigned int line;
    unsigned int column;
} luauc_position_t;

typedef struct luauc_location_t
{
    luauc_position_t begin;
    luauc_position_t end;
} luauc_location_t;

typedef struct luauc_name_t
{
    const char* value;
} luauc_name_t;

typedef struct luauc_array_t
{
    void* data;
    size_t size;
} luauc_array_t;

typedef struct luauc_ast_expr_t luauc_ast_expr_t;
typedef struct luauc_ast_stat_t luauc_ast_stat_t;
typedef struct luauc_ast_type_t luauc_ast_type_t;
typedef struct luauc_ast_type_pack_t luauc_ast_type_pack_t;
typedef struct luauc_ast_local_t luauc_ast_local_t;
typedef struct luauc_ast_attr_t luauc_ast_attr_t;
typedef struct luauc_ast_generic_type_t luauc_ast_generic_type_t;
typedef struct luauc_ast_generic_type_pack_t luauc_ast_generic_type_pack_t;

typedef struct luauc_optional_location_t
{
    int present;
    luauc_location_t value;
} luauc_optional_location_t;

typedef struct luauc_type_or_pack_t
{
    luauc_ast_type_t* type;
    luauc_ast_type_pack_t* type_pack;
} luauc_type_or_pack_t;

typedef struct luauc_type_list_t
{
    luauc_array_t types;
    luauc_ast_type_pack_t* tail_type;
} luauc_type_list_t;

typedef enum luauc_constant_number_parse_result_t
{
    LUAUC_NUMBER_OK,
    LUAUC_NUMBER_IMPRECISE,
    LUAUC_NUMBER_MALFORMED,
    LUAUC_NUMBER_BIN_OVERFLOW,
    LUAUC_NUMBER_HEX_OVERFLOW,
    LUAUC_NUMBER_INT_OVERFLOW
} luauc_constant_number_parse_result_t;

typedef enum luauc_string_quote_style_t
{
    LUAUC_STRING_QUOTED_SIMPLE,
    LUAUC_STRING_QUOTED_SINGLE,
    LUAUC_STRING_QUOTED_RAW,
    LUAUC_STRING_UNQUOTED
} luauc_string_quote_style_t;

typedef enum luauc_unary_op_t
{
    LUAUC_UNARY_NOT,
    LUAUC_UNARY_MINUS,
    LUAUC_UNARY_LEN
} luauc_unary_op_t;

typedef enum luauc_binary_op_t
{
    LUAUC_BINARY_ADD,
    LUAUC_BINARY_SUB,
    LUAUC_BINARY_MUL,
    LUAUC_BINARY_DIV,
    LUAUC_BINARY_FLOOR_DIV,
    LUAUC_BINARY_MOD,
    LUAUC_BINARY_POW,
    LUAUC_BINARY_CONCAT,
    LUAUC_BINARY_COMPARE_NE,
    LUAUC_BINARY_COMPARE_EQ,
    LUAUC_BINARY_COMPARE_LT,
    LUAUC_BINARY_COMPARE_LE,
    LUAUC_BINARY_COMPARE_GT,
    LUAUC_BINARY_COMPARE_GE,
    LUAUC_BINARY_AND,
    LUAUC_BINARY_OR,
    LUAUC_BINARY_COUNT
} luauc_binary_op_t;

typedef enum luauc_ast_expr_kind_t
{
    LUAUC_EXPR_GROUP,
    LUAUC_EXPR_CONSTANT_NIL,
    LUAUC_EXPR_CONSTANT_BOOL,
    LUAUC_EXPR_CONSTANT_NUMBER,
    LUAUC_EXPR_CONSTANT_INTEGER,
    LUAUC_EXPR_CONSTANT_STRING,
    LUAUC_EXPR_LOCAL,
    LUAUC_EXPR_GLOBAL,
    LUAUC_EXPR_VARARGS,
    LUAUC_EXPR_CALL,
    LUAUC_EXPR_INDEX_NAME,
    LUAUC_EXPR_INDEX_EXPR,
    LUAUC_EXPR_FUNCTION,
    LUAUC_EXPR_TABLE,
    LUAUC_EXPR_UNARY,
    LUAUC_EXPR_BINARY,
    LUAUC_EXPR_TYPE_ASSERTION,
    LUAUC_EXPR_IF_ELSE,
    LUAUC_EXPR_INTERP_STRING,
    LUAUC_EXPR_INSTANTIATE,
    LUAUC_EXPR_ERROR
} luauc_ast_expr_kind_t;

typedef enum luauc_ast_stat_kind_t
{
    LUAUC_STAT_BLOCK,
    LUAUC_STAT_IF,
    LUAUC_STAT_WHILE,
    LUAUC_STAT_REPEAT,
    LUAUC_STAT_BREAK,
    LUAUC_STAT_CONTINUE,
    LUAUC_STAT_RETURN,
    LUAUC_STAT_EXPR,
    LUAUC_STAT_LOCAL,
    LUAUC_STAT_FOR,
    LUAUC_STAT_FOR_IN,
    LUAUC_STAT_ASSIGN,
    LUAUC_STAT_COMPOUND_ASSIGN,
    LUAUC_STAT_FUNCTION,
    LUAUC_STAT_LOCAL_FUNCTION,
    LUAUC_STAT_TYPE_ALIAS,
    LUAUC_STAT_TYPE_FUNCTION,
    LUAUC_STAT_DECLARE_GLOBAL,
    LUAUC_STAT_DECLARE_FUNCTION,
    LUAUC_STAT_CLASS,
    LUAUC_STAT_DECLARE_EXTERN_TYPE,
    LUAUC_STAT_ERROR
} luauc_ast_stat_kind_t;

typedef enum luauc_ast_type_kind_t
{
    LUAUC_TYPE_REFERENCE,
    LUAUC_TYPE_TABLE,
    LUAUC_TYPE_FUNCTION,
    LUAUC_TYPE_TYPEOF,
    LUAUC_TYPE_OPTIONAL,
    LUAUC_TYPE_UNION,
    LUAUC_TYPE_INTERSECTION,
    LUAUC_TYPE_ERROR,
    LUAUC_TYPE_SINGLETON_BOOL,
    LUAUC_TYPE_SINGLETON_STRING,
    LUAUC_TYPE_GROUP
} luauc_ast_type_kind_t;

typedef enum luauc_ast_type_pack_kind_t
{
    LUAUC_TYPE_PACK_EXPLICIT,
    LUAUC_TYPE_PACK_VARIADIC,
    LUAUC_TYPE_PACK_GENERIC
} luauc_ast_type_pack_kind_t;

typedef enum luauc_attr_type_t
{
    LUAUC_ATTR_CHECKED,
    LUAUC_ATTR_NATIVE,
    LUAUC_ATTR_DEPRECATED,
    LUAUC_ATTR_DEBUG_NOINLINE,
    LUAUC_ATTR_UNKNOWN
} luauc_attr_type_t;

typedef enum luauc_table_item_kind_t
{
    LUAUC_TABLE_ITEM_LIST,
    LUAUC_TABLE_ITEM_RECORD,
    LUAUC_TABLE_ITEM_GENERAL
} luauc_table_item_kind_t;

typedef enum luauc_table_access_t
{
    LUAUC_TABLE_READ = 1,
    LUAUC_TABLE_WRITE = 2,
    LUAUC_TABLE_READ_WRITE = 3
} luauc_table_access_t;

struct luauc_ast_local_t
{
    luauc_name_t name;
    luauc_location_t location;
    luauc_ast_local_t* shadow;
    size_t function_depth;
    size_t loop_depth;
    int is_const;
    int is_exported;
    luauc_ast_type_t* annotation;
};

struct luauc_ast_attr_t
{
    luauc_location_t location;
    luauc_attr_type_t type;
    luauc_array_t args;
    luauc_name_t name;
};

struct luauc_ast_generic_type_t
{
    luauc_location_t location;
    luauc_name_t name;
    luauc_ast_type_t* default_value;
};

struct luauc_ast_generic_type_pack_t
{
    luauc_location_t location;
    luauc_name_t name;
    luauc_ast_type_pack_t* default_value;
};

typedef struct luauc_ast_table_item_t
{
    luauc_table_item_kind_t kind;
    luauc_ast_expr_t* key;
    luauc_ast_expr_t* value;
} luauc_ast_table_item_t;

typedef struct luauc_argument_name_t
{
    luauc_name_t name;
    luauc_location_t location;
} luauc_argument_name_t;

typedef struct luauc_optional_argument_name_t
{
    int present;
    luauc_argument_name_t value;
} luauc_optional_argument_name_t;

typedef struct luauc_table_indexer_t
{
    luauc_ast_type_t* index_type;
    luauc_ast_type_t* result_type;
    luauc_location_t location;
    luauc_table_access_t access;
    luauc_optional_location_t access_location;
} luauc_table_indexer_t;

typedef struct luauc_table_property_t
{
    luauc_name_t name;
    luauc_location_t location;
    luauc_ast_type_t* type;
    luauc_table_access_t access;
    luauc_optional_location_t access_location;
} luauc_table_property_t;

typedef struct luauc_class_property_t
{
    luauc_location_t qualifier_location;
    luauc_name_t name;
    luauc_location_t name_location;
    luauc_optional_location_t type_colon_location;
    luauc_ast_type_t* type;
} luauc_class_property_t;

typedef struct luauc_class_method_t
{
    luauc_optional_location_t qualifier_location;
    luauc_location_t keyword_location;
    luauc_name_t function_name;
    luauc_location_t name_location;
    luauc_ast_expr_t* function;
} luauc_class_method_t;

typedef struct luauc_class_member_t
{
    int is_method;
    union
    {
        luauc_class_property_t property;
        luauc_class_method_t method;
    } value;
} luauc_class_member_t;

typedef struct luauc_declared_extern_property_t
{
    luauc_name_t name;
    luauc_location_t name_location;
    luauc_ast_type_t* type;
    int is_method;
    luauc_location_t location;
    luauc_table_access_t access;
} luauc_declared_extern_property_t;

struct luauc_ast_expr_t
{
    luauc_ast_expr_kind_t kind;
    luauc_location_t location;
    union
    {
        struct
        {
            luauc_ast_expr_t* expression;
        } group;
        struct
        {
            int value;
        } constant_bool;
        struct
        {
            double value;
            luauc_constant_number_parse_result_t parse_result;
        } constant_number;
        struct
        {
            int64_t value;
            luauc_constant_number_parse_result_t parse_result;
        } constant_integer;
        struct
        {
            luauc_array_t value;
            luauc_string_quote_style_t quote_style;
        } constant_string;
        struct
        {
            luauc_ast_local_t* local;
            int upvalue;
        } local;
        struct
        {
            luauc_name_t name;
        } global;
        struct
        {
            luauc_ast_expr_t* function;
            luauc_array_t type_arguments;
            luauc_array_t arguments;
            int self;
            luauc_location_t argument_location;
        } call;
        struct
        {
            luauc_ast_expr_t* expression;
            luauc_name_t index;
            luauc_location_t index_location;
            luauc_position_t operator_position;
            char operator_character;
        } index_name;
        struct
        {
            luauc_ast_expr_t* expression;
            luauc_ast_expr_t* index;
        } index_expr;
        struct
        {
            luauc_array_t attributes;
            luauc_array_t generics;
            luauc_array_t generic_packs;
            luauc_ast_local_t* self;
            luauc_array_t arguments;
            luauc_ast_type_pack_t* return_annotation;
            int vararg;
            luauc_location_t vararg_location;
            luauc_ast_type_pack_t* vararg_annotation;
            luauc_ast_stat_t* body;
            size_t function_depth;
            luauc_name_t debugname;
            luauc_optional_location_t argument_location;
        } function;
        struct
        {
            luauc_array_t items;
        } table;
        struct
        {
            luauc_unary_op_t op;
            luauc_ast_expr_t* expression;
        } unary;
        struct
        {
            luauc_binary_op_t op;
            luauc_ast_expr_t* left;
            luauc_ast_expr_t* right;
        } binary;
        struct
        {
            luauc_ast_expr_t* expression;
            luauc_ast_type_t* annotation;
        } type_assertion;
        struct
        {
            luauc_ast_expr_t* condition;
            int has_then;
            luauc_ast_expr_t* true_expression;
            int has_else;
            luauc_ast_expr_t* false_expression;
        } if_else;
        struct
        {
            luauc_array_t strings;
            luauc_array_t expressions;
        } interpolated_string;
        struct
        {
            luauc_ast_expr_t* expression;
            luauc_array_t type_arguments;
        } instantiate;
        struct
        {
            luauc_array_t expressions;
            unsigned int message_index;
        } error;
    } value;
};

struct luauc_ast_stat_t
{
    luauc_ast_stat_kind_t kind;
    luauc_location_t location;
    int has_semicolon;
    union
    {
        struct
        {
            luauc_array_t body;
            int has_end;
        } block;
        struct
        {
            luauc_ast_expr_t* condition;
            luauc_ast_stat_t* then_body;
            luauc_ast_stat_t* else_body;
            luauc_optional_location_t then_location;
            luauc_optional_location_t else_location;
        } if_statement;
        struct
        {
            luauc_ast_expr_t* condition;
            luauc_ast_stat_t* body;
            int has_do;
            luauc_location_t do_location;
        } while_statement;
        struct
        {
            luauc_ast_expr_t* condition;
            luauc_ast_stat_t* body;
            int has_until;
        } repeat_statement;
        struct
        {
            luauc_array_t expressions;
        } return_statement;
        struct
        {
            luauc_ast_expr_t* expression;
        } expression;
        struct
        {
            luauc_array_t variables;
            luauc_array_t values;
            int is_const;
            int is_exported;
            luauc_optional_location_t keyword_location;
            luauc_optional_location_t equals_location;
        } local;
        struct
        {
            luauc_ast_local_t* variable;
            luauc_ast_expr_t* from;
            luauc_ast_expr_t* to;
            luauc_ast_expr_t* step;
            luauc_ast_stat_t* body;
            int has_do;
            luauc_location_t do_location;
        } for_statement;
        struct
        {
            luauc_array_t variables;
            luauc_array_t values;
            luauc_ast_stat_t* body;
            int has_in;
            luauc_location_t in_location;
            int has_do;
            luauc_location_t do_location;
        } for_in;
        struct
        {
            luauc_array_t variables;
            luauc_array_t values;
        } assign;
        struct
        {
            luauc_binary_op_t op;
            luauc_ast_expr_t* variable;
            luauc_ast_expr_t* value;
        } compound_assign;
        struct
        {
            luauc_ast_expr_t* name;
            luauc_ast_expr_t* function;
        } function;
        struct
        {
            luauc_ast_local_t* name;
            luauc_ast_expr_t* function;
            int is_const;
            luauc_position_t const_keyword_begin;
        } local_function;
        struct
        {
            luauc_name_t name;
            luauc_location_t name_location;
            luauc_array_t generics;
            luauc_array_t generic_packs;
            luauc_ast_type_t* type;
            int exported;
        } type_alias;
        struct
        {
            luauc_name_t name;
            luauc_location_t name_location;
            luauc_ast_expr_t* body;
            int exported;
            int has_errors;
        } type_function;
        struct
        {
            luauc_name_t name;
            luauc_location_t name_location;
            luauc_ast_type_t* type;
        } declare_global;
        struct
        {
            luauc_array_t attributes;
            luauc_name_t name;
            luauc_location_t name_location;
            luauc_array_t generics;
            luauc_array_t generic_packs;
            luauc_type_list_t parameters;
            luauc_array_t parameter_names;
            int vararg;
            luauc_location_t vararg_location;
            luauc_ast_type_pack_t* return_types;
        } declare_function;
        struct
        {
            luauc_ast_local_t* name;
            luauc_array_t members;
            int exported;
        } class_statement;
        struct
        {
            luauc_name_t name;
            int has_super_name;
            luauc_name_t super_name;
            luauc_array_t properties;
            luauc_table_indexer_t* indexer;
        } declare_extern_type;
        struct
        {
            luauc_array_t expressions;
            luauc_array_t statements;
            unsigned int message_index;
        } error;
    } value;
};

struct luauc_ast_type_t
{
    luauc_ast_type_kind_t kind;
    luauc_location_t location;
    union
    {
        struct
        {
            int has_parameter_list;
            int has_prefix;
            luauc_name_t prefix;
            luauc_optional_location_t prefix_location;
            luauc_ast_local_t* prefix_local;
            luauc_name_t name;
            luauc_location_t name_location;
            luauc_array_t parameters;
        } reference;
        struct
        {
            luauc_array_t properties;
            luauc_table_indexer_t* indexer;
        } table;
        struct
        {
            luauc_array_t attributes;
            luauc_array_t generics;
            luauc_array_t generic_packs;
            luauc_type_list_t argument_types;
            luauc_array_t argument_names;
            luauc_ast_type_pack_t* return_types;
        } function;
        struct
        {
            luauc_ast_expr_t* expression;
        } typeof_type;
        struct
        {
            luauc_array_t types;
        } aggregate;
        struct
        {
            luauc_array_t types;
            int is_missing;
            unsigned int message_index;
        } error;
        struct
        {
            int value;
        } singleton_bool;
        struct
        {
            luauc_array_t value;
        } singleton_string;
        struct
        {
            luauc_ast_type_t* type;
        } group;
    } value;
};

struct luauc_ast_type_pack_t
{
    luauc_ast_type_pack_kind_t kind;
    luauc_location_t location;
    union
    {
        luauc_type_list_t explicit_types;
        luauc_ast_type_t* variadic_type;
        luauc_name_t generic_name;
    } value;
};

static inline luauc_position_t __luauc_position_make(unsigned int line, unsigned int column)
{
    luauc_position_t result = {line, column};
    return result;
}

static inline luauc_position_t __luauc_position_missing(void)
{
    return __luauc_position_make(UINT_MAX, UINT_MAX);
}

static inline luauc_location_t __luauc_location_make(luauc_position_t begin, luauc_position_t end)
{
    luauc_location_t result = {begin, end};
    return result;
}

static inline luauc_location_t __luauc_location_length(luauc_position_t begin, unsigned int length)
{
    return __luauc_location_make(begin, __luauc_position_make(begin.line, begin.column + length));
}

static inline int __luauc_position_equal(luauc_position_t left, luauc_position_t right)
{
    return left.line == right.line && left.column == right.column;
}

static inline int __luauc_position_less(luauc_position_t left, luauc_position_t right)
{
    return left.line < right.line || (left.line == right.line && left.column < right.column);
}

int luauc_location_encloses(luauc_location_t outer, luauc_location_t inner);
int luauc_location_overlaps(luauc_location_t left, luauc_location_t right);
int luauc_location_contains(luauc_location_t location, luauc_position_t position);
int luauc_location_contains_closed(luauc_location_t location, luauc_position_t position);
void luauc_location_extend(luauc_location_t* location, luauc_location_t other);

int luauc_ast_is_lvalue(const luauc_ast_expr_t* expression);
int luauc_ast_is_constant_literal(const luauc_ast_expr_t* expression);
int luauc_ast_is_literal_table(const luauc_ast_expr_t* expression);
luauc_name_t luauc_ast_get_identifier(const luauc_ast_expr_t* expression);
int luauc_ast_function_has_attribute(const luauc_ast_expr_t* function, luauc_attr_type_t type);

#endif
