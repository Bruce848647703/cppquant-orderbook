cmake_minimum_required(VERSION 3.20)
project(cppquant_orderbook VERSION 0.1.0 LANGUAGES CXX)

option(CPPQUANT_BUILD_TESTS "Build the unit tests" ON)
option(CPPQUANT_BUILD_BENCHMARKS "Build the benchmark" ON)
option(CPPQUANT_BUILD_EXAMPLES "Build the examples" ON)
option(CPPQUANT_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(CPPQUANT_TSAN "Build with ThreadSanitizer" OFF)
option(CPPQUANT_WERROR "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(cppquant_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(CPPQUANT_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
        if(CPPQUANT_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

if(CPPQUANT_SANITIZE)
    if(MSVC)
        message(FATAL_ERROR "CPPQUANT_SANITIZE requires Clang or GCC")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(CPPQUANT_TSAN)
    if(MSVC)
        message(FATAL_ERROR "CPPQUANT_TSAN requires Clang or GCC")
    endif()
    if(CPPQUANT_SANITIZE)
        message(FATAL_ERROR "CPPQUANT_TSAN cannot be combined with CPPQUANT_SANITIZE")
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

add_library(cppquant_orderbook
    src/orderbook.cpp
    src/pool.cpp
)
add_library(cppquant::orderbook ALIAS cppquant_orderbook)
target_include_directories(cppquant_orderbook PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(cppquant_orderbook PUBLIC cxx_std_20)
set_target_properties(cppquant_orderbook PROPERTIES EXPORT_NAME orderbook)
cppquant_set_warnings(cppquant_orderbook)

if(CPPQUANT_BUILD_EXAMPLES)
    add_executable(orderbook_replay
        examples/replay.cpp
    )
    target_link_libraries(orderbook_replay PRIVATE cppquant::orderbook)
    cppquant_set_warnings(orderbook_replay)
endif()

if(CPPQUANT_BUILD_BENCHMARKS)
    add_executable(orderbook_bench
        bench/bench_orderbook.cpp
    )
    target_link_libraries(orderbook_bench PRIVATE cppquant::orderbook)
    cppquant_set_warnings(orderbook_bench)
endif()

if(CPPQUANT_BUILD_TESTS)
    enable_testing()

    add_executable(orderbook_tests
        tests/main.cpp
        tests/test_orderbook.cpp
        tests/test_spsc_queue.cpp
        tests/test_engine.cpp
    )
    target_link_libraries(orderbook_tests PRIVATE cppquant::orderbook)
    target_include_directories(orderbook_tests PRIVATE tests)
    cppquant_set_warnings(orderbook_tests)

    add_test(NAME orderbook_tests COMMAND orderbook_tests)
    set_tests_properties(orderbook_tests PROPERTIES TIMEOUT 120)

    if(TARGET orderbook_replay)
        add_test(NAME orderbook_replay_sample
            COMMAND orderbook_replay ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_session.csv
        )
        set_tests_properties(orderbook_replay_sample PROPERTIES TIMEOUT 60)
    endif()

    if(TARGET orderbook_bench)
        add_test(NAME orderbook_bench_smoke COMMAND orderbook_bench --quick)
        set_tests_properties(orderbook_bench_smoke PROPERTIES TIMEOUT 120)
    endif()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS cppquant_orderbook
    EXPORT cppquantTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT cppquantTargets
    FILE cppquantTargets.cmake
    NAMESPACE cppquant::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppquant
)

configure_package_config_file(
    cmake/cppquantConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cppquantConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppquant
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cppquantConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/cppquantConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cppquantConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/cppquant
)
