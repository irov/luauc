// This file is part of the luauc C port of the Luau programming language
// and is licensed under the MIT License; see LICENSE.txt for details.
#include "lua.h"
#include "luacode.h"
#include "lualib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int __type_callback_count;
static int __constant_callback_count;
static unsigned int __hex_digit(char value);

static int __known_member_type(const char* library, const char* member)
{
    if (strcmp(library, "known") == 0 && strcmp(member, "answer") == 0)
    {
        ++__type_callback_count;
        return 2;
    }
    return 15;
}

static void __known_member_constant(
    const char* library, const char* member, lua_CompileConstant* constant
)
{
    if (strcmp(library, "known") == 0 && strcmp(member, "answer") == 0)
    {
        ++__constant_callback_count;
        luau_set_compile_constant_number(constant, 42.5);
    }
}

static unsigned char* __read_file(const char* path, size_t* size)
{
    FILE* file = fopen(path, "rb");
    unsigned char* data;
    long length;

    if (file == NULL || fseek(file, 0, SEEK_END) != 0)
        return NULL;
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return NULL;
    }
    data = (unsigned char*)malloc((size_t)length + 1);
    if (data == NULL)
    {
        fclose(file);
        return NULL;
    }
    if (fread(data, 1, (size_t)length, file) != (size_t)length)
    {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    data[length] = 0;
    *size = (size_t)length;
    return data;
}

static int __run_source(const char* source)
{
    lua_State* state;
    char* bytecode;
    size_t bytecode_size = 0;
    int status;

    bytecode = luau_compile(source, strlen(source), NULL, &bytecode_size);
    if (bytecode == NULL)
    {
        fprintf(stderr, "luau_compile returned null\n");
        return 0;
    }
    if (bytecode_size == 0 || (unsigned char)bytecode[0] == 0)
    {
        fprintf(stderr, "compile error: %.*s\n", (int)(bytecode_size - 1), bytecode + 1);
        free(bytecode);
        return 0;
    }

    state = luaL_newstate();
    if (state == NULL)
    {
        free(bytecode);
        return 0;
    }
    luaL_openlibs(state);
    status = luau_load(state, "=compiler_test", bytecode, bytecode_size, 0);
    free(bytecode);
    if (status == 0)
        status = lua_pcall(state, 0, 0, 0);
    if (status != 0)
        fprintf(
            stderr,
            "runtime error: %s\n",
            lua_tostring(state, -1) != NULL ? lua_tostring(state, -1) : "(non-string)"
        );
    lua_close(state);
    return status == 0;
}

static int __test_error_bytecode(void)
{
    static const char __source[] = "local =";
    static const unsigned char __expected[] =
        "\0:1: Expected identifier when parsing variable name, got '='";
    size_t size = 0;
    char* bytecode = luau_compile(__source, sizeof(__source) - 1, NULL, &size);
    int result = bytecode != NULL && size == sizeof(__expected) - 1 &&
        memcmp(bytecode, __expected, sizeof(__expected) - 1) == 0;
    free(bytecode);
    return result;
}

static int __test_compile_options_callbacks(void)
{
    static const char __source[] = "return known.answer";
    static const char __expected[] =
        "070300000101000001020003410000000500000016000200010200000000004045400001000118000000010000000000";
    static const char __mutable_source[] = "known = {}\nreturn known.answer";
    static const char* const __known_libraries[] = {"known", NULL};
    lua_CompileOptions options;
    lua_State* state;
    char* bytecode;
    size_t bytecode_size = 0;
    int status;
    int result;

    memset(&options, 0, sizeof(options));
    options.optimizationLevel = 2;
    options.debugLevel = 1;
    options.typeInfoLevel = 1;
    options.librariesWithKnownMembers = __known_libraries;
    options.libraryMemberTypeCb = __known_member_type;
    options.libraryMemberConstantCb = __known_member_constant;

    __type_callback_count = 0;
    __constant_callback_count = 0;
    bytecode = luau_compile(__source, sizeof(__source) - 1, &options, &bytecode_size);
    if (bytecode == NULL || bytecode_size == 0 || (unsigned char)bytecode[0] == 0)
    {
        free(bytecode);
        return 0;
    }
    if (bytecode_size != strlen(__expected) / 2)
    {
        free(bytecode);
        return 0;
    }
    {
        size_t index;
        for (index = 0; index < bytecode_size; ++index)
        {
            unsigned int high = __hex_digit(__expected[index * 2]);
            unsigned int low = __hex_digit(__expected[index * 2 + 1]);
            if ((unsigned char)bytecode[index] !=
                (unsigned char)((high << 4) | low))
            {
                free(bytecode);
                return 0;
            }
        }
    }

    state = luaL_newstate();
    if (state == NULL)
    {
        free(bytecode);
        return 0;
    }
    status = luau_load(state, "=compile_options", bytecode, bytecode_size, 0);
    free(bytecode);
    if (status == 0)
        status = lua_pcall(state, 0, LUA_MULTRET, 0);
    result = status == 0 && lua_gettop(state) == 1 && lua_isnumber(state, 1) &&
        lua_tonumber(state, 1) == 42.5 && __type_callback_count == 1 &&
        __constant_callback_count == 1;
    lua_close(state);
    if (!result)
        return 0;

    __type_callback_count = 0;
    __constant_callback_count = 0;
    bytecode = luau_compile(
        __mutable_source, sizeof(__mutable_source) - 1, &options, &bytecode_size
    );
    result = bytecode != NULL && bytecode_size > 0 &&
        (unsigned char)bytecode[0] != 0 && __type_callback_count == 1 &&
        __constant_callback_count == 0;
    free(bytecode);
    return result;
}

static int __test_userdata_type_limit(void)
{
    static const char __source[] = "return 1";
    static const char __expected[] =
        ":1: Exceeded userdata type limit in the compilation options";
    const char* types[34];
    lua_CompileOptions options;
    char* bytecode;
    size_t bytecode_size = 0;
    size_t index;
    int result;

    for (index = 0; index < 33; ++index)
        types[index] = "T";
    types[33] = NULL;
    memset(&options, 0, sizeof(options));
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    options.userdataTypes = types;
    bytecode = luau_compile(__source, sizeof(__source) - 1, &options, &bytecode_size);
    result = bytecode != NULL && bytecode_size == sizeof(__expected) &&
        (unsigned char)bytecode[0] == 0 &&
        memcmp(bytecode + 1, __expected, sizeof(__expected) - 1) == 0;
    free(bytecode);
    return result;
}

static unsigned int __hex_digit(char value)
{
    if (value >= '0' && value <= '9')
        return (unsigned int)(value - '0');
    if (value >= 'a' && value <= 'f')
        return (unsigned int)(value - 'a' + 10);
    return 16;
}

static int __bytecode_matches_hex(
    const char* source, const lua_CompileOptions* options, const char* expected
)
{
    size_t actual_size = 0;
    size_t expected_size = strlen(expected) / 2;
    char* actual = luau_compile(source, strlen(source), (lua_CompileOptions*)options, &actual_size);
    size_t byte_index;
    int match = actual != NULL && actual_size == expected_size;

    for (byte_index = 0; match && byte_index < expected_size; ++byte_index)
    {
        unsigned int high = __hex_digit(expected[byte_index * 2]);
        unsigned int low = __hex_digit(expected[byte_index * 2 + 1]);
        match = high < 16 && low < 16 &&
            (unsigned char)actual[byte_index] == (unsigned char)((high << 4) | low);
    }
    free(actual);
    return match;
}

static int __test_small_oracle_corpus(void)
{
    static const struct
    {
        const char* source;
        const char* expected;
    } __cases[] = {
        {"", "07030000010000000102000241000000160001000000010001180000010000000000"},
        {"return nil\n", "070300000101000001020003410000000200000016000200000001000118000000010000000000"},
        {"return true\n", "070300000101000001020003410000000300010016000200000001000118000000010000000000"},
        {"return false\n", "070300000101000001020003410000000300000016000200000001000118000000010000000000"},
        {"return 0\n", "070300000101000001020003410000000400000016000200000001000118000000010000000000"},
        {"return -1\n", "070300000101000001020003410000000400ffff16000200000001000118000000010000000000"},
        {"return 32767\n", "070300000101000001020003410000000400ff7f16000200000001000118000000010000000000"},
        {"return 32768\n", "0703000001010000010200034100000005000000160002000102000000000000e0400001000118000000010000000000"},
        {"return 1.5\n", "0703000001010000010200034100000005000000160002000102000000000000f83f0001000118000000010000000000"},
        {"return \"hello\"\n", "0703010568656c6c6f0001010000010200034100000005000000160002000103010001000118000000010000000000"},
        {"return 1 + 2\n", "070300000101000001020003410000000400030016000200000001000118000000010000000000"},
        {"return value\n", "0703010576616c7565000101000001020004410000000c00010000000040160002000203010400000040000100011800000000010000000000"},
        {"return math.abs\n", "070302046d61746803616273000101000001020004410000000c000200000400801600020003030103020400040080000100011800000000010000000000"},
        {"return game.workspace.part\n", "0703030467616d6509776f726b73706163650470617274000101000001020004410000000c000300020400c0160002000403010302030304020400c0000100011800000000010000000000"},
        {"value = 5\nreturn value\n", "0703010576616c75650001010000010200074100000004000500080000cc00000000070000cc0000000016000200010301000100011800000000010000010000000000"},
        {"math = {}\nreturn math.abs\n", "070302046d6174680361627300010100000102000a4100000035000000000000000800006c000000000700006c000000000f0000f401000000160002000203010302000100011800000000000100000000010000000000"}
    };
    size_t case_index;

    for (case_index = 0; case_index < sizeof(__cases) / sizeof(__cases[0]); ++case_index)
    {
        if (!__bytecode_matches_hex(__cases[case_index].source, NULL, __cases[case_index].expected))
        {
            fprintf(stderr, "oracle mismatch for corpus case %zu\n", case_index);
            return 0;
        }
    }
    return 1;
}

static int __test_option_oracle_corpus(void)
{
    lua_CompileOptions options;

    memset(&options, 0, sizeof(options));
    if (!__bytecode_matches_hex(
            "return -1\n",
            &options,
            "070300000102000001020004410000000501000033000100160002000102000000000000f03f000100000000"
        ))
        return 0;

    options.debugLevel = 2;
    if (!__bytecode_matches_hex(
            "return 1 + 2\n",
            &options,
            "07030000010300000102000541000000050100000502010021000102160002000202000000000000f03f02000000000000004000010001180000000000010000000000"
        ))
        return 0;

    options.optimizationLevel = 1;
    options.debugLevel = 1;
    options.coverageLevel = 1;
    if (!__bytecode_matches_hex(
            "return 1\n",
            &options,
            "0703000001010000010200044100000045000000040001001600020000000100011800000000010000000000"
        ))
        return 0;
    options.coverageLevel = 2;
    if (!__bytecode_matches_hex(
            "return 1\n",
            &options,
            "07030000010100000102000541000000450000004500000004000100160002000000010001180000000000010000000000"
        ))
        return 0;
    return 1;
}

static int __test_typeinfo_oracle_corpus(void)
{
    static const struct
    {
        const char* source;
        const char* expected;
    } __cases[] = {
        {
            "return function(x: number) return x end\n",
            "0703000002010100000006030000050102011600020000000100011800010000000001000001020003410000004000000016000200010600010001000118000000010000000001"
        },
        {
            "local function f(x: number, y: string) return x end\nreturn f\n",
            "070301016600020202000000070400000502020301160002000000010101180001000000000100000102070000010f00010203410000004000000016000200010600010001000118000001010000000001"
        },
        {
            "return function(x: number?) return x end\n",
            "0703000002010100000006030000050182011600020000000100011800010000000001000001020003410000004000000016000200010600010001000118000000010000000001"
        },
        {
            "return function(x: {string}) return x end\n",
            "0703000002010100000006030000050104011600020000000100011800010000000001000001020003410000004000000016000200010600010001000118000000010000000001"
        }
    };
    lua_CompileOptions options;
    size_t index;

    memset(&options, 0, sizeof(options));
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    options.typeInfoLevel = 1;
    for (index = 0; index < sizeof(__cases) / sizeof(__cases[0]); ++index)
        if (!__bytecode_matches_hex(__cases[index].source, &options, __cases[index].expected))
            return 0;
    return 1;
}

static int __compare_oracle(const char* source_path, const char* bytecode_path)
{
    size_t source_size = 0;
    size_t expected_size = 0;
    size_t actual_size = 0;
    unsigned char* source = __read_file(source_path, &source_size);
    unsigned char* expected = __read_file(bytecode_path, &expected_size);
    char* actual;
    int result;

    if (source == NULL || expected == NULL)
    {
        free(source);
        free(expected);
        return 0;
    }
    actual = luau_compile((const char*)source, source_size, NULL, &actual_size);
    result = actual != NULL && actual_size == expected_size && memcmp(actual, expected, actual_size) == 0;
    if (!result)
    {
        size_t limit = actual_size < expected_size ? actual_size : expected_size;
        size_t offset = 0;
        while (offset < limit && (unsigned char)actual[offset] == expected[offset])
            ++offset;
        fprintf(
            stderr,
            "compiler bytecode mismatch: C=%zu bytes, oracle=%zu bytes, first difference=%zu (C=%u, oracle=%u)\n",
            actual_size,
            expected_size,
            offset,
            offset < actual_size ? (unsigned)(unsigned char)actual[offset] : 0,
            offset < expected_size ? (unsigned)expected[offset] : 0
        );
        for (offset = 0; offset < actual_size; ++offset)
            fprintf(stderr, "%02x%s", (unsigned)(unsigned char)actual[offset], (offset + 1) % 16 == 0 ? "\n" : " ");
        if (actual_size % 16 != 0)
            fputc('\n', stderr);
    }
    free(actual);
    free(expected);
    free(source);
    return result;
}

int main(int argc, char** argv)
{
    static const char __source[] =
        "local function makeCounter(start)\n"
        "  local value = start\n"
        "  return function(step)\n"
        "    value += step\n"
        "    return value\n"
        "  end\n"
        "end\n"
        "local counter = makeCounter(10)\n"
        "assert(counter(2) == 12)\n"
        "assert(counter(5) == 17)\n"
        "local t = {x = 3, 4, 5}\n"
        "assert(t.x == 3 and t[1] == 4 and t[2] == 5)\n"
        "local sum = 0\n"
        "for i = 1, 5 do sum += i end\n"
        "assert(sum == 15)\n"
        "local j = 0\n"
        "while j < 3 do j += 1 end\n"
        "assert(j == 3)\n"
        "assert(`value={j}` == \"value=3\")\n";

    if (argc == 3)
        return __compare_oracle(argv[1], argv[2]) ? 0 : 3;
    if (argc != 1)
        return 4;
    if (!__run_source(__source))
        return 1;
    if (!__test_error_bytecode())
        return 2;
    if (!__test_compile_options_callbacks())
        return 5;
    if (!__test_userdata_type_limit())
        return 6;
    if (!__test_small_oracle_corpus())
        return 7;
    if (!__test_option_oracle_corpus())
        return 8;
    if (!__test_typeinfo_oracle_corpus())
        return 9;
    return 0;
}
