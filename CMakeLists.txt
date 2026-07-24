cmake_minimum_required(VERSION 3.15)

project(luauc VERSION 0.729.0 LANGUAGES C)

option(LUAUC_BUILD_TESTS "Build luauc tests" ON)
option(LUAUC_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

function(luauc_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror -pedantic-errors)
    endif()
endfunction()

file(GLOB LUAUC_VM_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vm/*.c"
)

add_library(luauc STATIC
    src/common/luauc_common.c
    src/common/luauc_flags.c
    src/ast/luauc_ast.c
    src/ast/luauc_lexer.c
    src/ast/luauc_parser.c
    src/bytecode/luauc_bytecode_builder.c
    src/compiler/luauc_compiler.c
    src/compiler/luacode.c
    ${LUAUC_VM_SOURCES}
)
add_library(luauc::luauc ALIAS luauc)

target_compile_features(luauc PUBLIC c_std_11)
set_target_properties(luauc PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE OFF
)

target_include_directories(luauc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ast
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common
        ${CMAKE_CURRENT_SOURCE_DIR}/src/bytecode
        ${CMAKE_CURRENT_SOURCE_DIR}/src/compiler
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vm
)

target_compile_definitions(luauc PRIVATE LUA_USE_LONGJMP=1)

if(NOT WIN32)
    target_link_libraries(luauc PUBLIC m)
endif()

luauc_enable_warnings(luauc)

if(LUAUC_ENABLE_SANITIZERS)
    if(MSVC)
        target_compile_options(luauc PRIVATE /fsanitize=address)
        target_link_options(luauc INTERFACE /fsanitize=address)
    else()
        target_compile_options(luauc PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(luauc INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_compile_definitions(luauc PRIVATE LUAU_ENABLE_ASAN=1)
    endif()
endif()

if(LUAUC_BUILD_TESTS)
    enable_testing()

    add_executable(luauc_common_test
        tests/test_common.c
    )
    target_link_libraries(luauc_common_test PRIVATE luauc)
    target_include_directories(luauc_common_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set_target_properties(luauc_common_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )

    luauc_enable_warnings(luauc_common_test)

    add_test(NAME luauc.common COMMAND luauc_common_test)

    add_executable(luauc_vm_test
        tests/test_vm.c
    )
    target_link_libraries(luauc_vm_test PRIVATE luauc)
    set_target_properties(luauc_vm_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )

    luauc_enable_warnings(luauc_vm_test)

    add_test(
        NAME luauc.vm.upstream_bytecode
        COMMAND luauc_vm_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode/vm_smoke.v7.luau.bc"
    )

    add_executable(luauc_bytecode_builder_test
        tests/test_bytecode_builder.c
    )
    target_link_libraries(luauc_bytecode_builder_test PRIVATE luauc)
    target_include_directories(luauc_bytecode_builder_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common
    )
    set_target_properties(luauc_bytecode_builder_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )

    luauc_enable_warnings(luauc_bytecode_builder_test)

    add_test(
        NAME luauc.bytecode_builder.oracle
        COMMAND luauc_bytecode_builder_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode/builder_return1.v7.luau.bc"
    )

    add_executable(luauc_parser_test
        tests/test_parser.c
    )
    target_link_libraries(luauc_parser_test PRIVATE luauc)
    target_include_directories(luauc_parser_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common
    )
    set_target_properties(luauc_parser_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )

    luauc_enable_warnings(luauc_parser_test)

    add_test(
        NAME luauc.parser.smoke
        COMMAND luauc_parser_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/vm_smoke.luau"
    )

    add_executable(luauc_compiler_test
        tests/test_compiler.c
    )
    target_link_libraries(luauc_compiler_test PRIVATE luauc)
    set_target_properties(luauc_compiler_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )

    luauc_enable_warnings(luauc_compiler_test)

    add_test(NAME luauc.compiler.smoke COMMAND luauc_compiler_test)
    add_test(
        NAME luauc.compiler.oracle.return1
        COMMAND luauc_compiler_test
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode/builder_return1.luau"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode/builder_return1.v7.luau.bc"
    )

    add_executable(luauc_typeinfo_test
        tests/test_typeinfo.c
    )
    target_link_libraries(luauc_typeinfo_test PRIVATE luauc)
    target_include_directories(luauc_typeinfo_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vm
    )
    set_target_properties(luauc_typeinfo_test PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )

    luauc_enable_warnings(luauc_typeinfo_test)

    add_test(NAME luauc.compiler.typeinfo COMMAND luauc_typeinfo_test)

    add_executable(luauc_conformance_runner
        tests/conformance_runner.c
    )
    target_link_libraries(luauc_conformance_runner PRIVATE luauc)
    target_include_directories(luauc_conformance_runner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vm
    )
    set_target_properties(luauc_conformance_runner PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )

    luauc_enable_warnings(luauc_conformance_runner)

    file(GLOB LUAUC_CONFORMANCE_FIXTURES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/conformance/*.luau"
    )
    foreach(LUAUC_CONFORMANCE_FIXTURE IN LISTS LUAUC_CONFORMANCE_FIXTURES)
        get_filename_component(LUAUC_CONFORMANCE_NAME
            "${LUAUC_CONFORMANCE_FIXTURE}" NAME_WE
        )
        add_test(
            NAME "luauc.conformance.${LUAUC_CONFORMANCE_NAME}"
            COMMAND luauc_conformance_runner "${LUAUC_CONFORMANCE_FIXTURE}"
        )
        set_tests_properties(
            "luauc.conformance.${LUAUC_CONFORMANCE_NAME}"
            PROPERTIES SKIP_RETURN_CODE 77
        )
    endforeach()
endif()
