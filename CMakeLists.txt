cmake_minimum_required(VERSION 3.10)

project(seq_ledger
    VERSION 1.0.0
    DESCRIPTION "A lightweight C++17 request ledger for tracking async request/response lifecycles by seqId"
    HOMEPAGE_URL "https://github.com/yaloooo23/cpp-seq-ledger"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Options ----
option(SEQ_LEDGER_BUILD_SHARED "Build seq_ledger as a shared library" ON)
option(SEQ_LEDGER_BUILD_TESTS  "Build seq_ledger unit tests" OFF)
option(SEQ_LEDGER_BUILD_EXAMPLES "Build example programs" OFF)

# ---- Dependencies ----
find_package(Threads REQUIRED)

# ---- Library target ----
set(SEQ_LEDGER_SOURCES
    src/seq_ledger.cpp
)

set(SEQ_LEDGER_PUBLIC_HEADERS
    include/seq_ledger/seq_ledger.h
    include/seq_ledger/seq_ledger_types.h
    include/seq_ledger/seq_ledger_export.h
)

if(SEQ_LEDGER_BUILD_SHARED)
    add_library(seq_ledger SHARED ${SEQ_LEDGER_SOURCES})
    target_compile_definitions(seq_ledger PRIVATE SEQ_LEDGER_BUILD_SHARED)
else()
    add_library(seq_ledger STATIC ${SEQ_LEDGER_SOURCES})
endif()

target_include_directories(seq_ledger
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(seq_ledger
    PRIVATE Threads::Threads
)

if(SEQ_LEDGER_BUILD_SHARED AND NOT MSVC)
    target_compile_options(seq_ledger PRIVATE -fvisibility=hidden)
endif()

# Versioned output name for shared library
if(SEQ_LEDGER_BUILD_SHARED AND NOT SEQ_LEDGER_BUILD_TESTS)
    set_target_properties(seq_ledger PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
endif()

# ---- Install rules ----
include(GNUInstallDirs)

install(TARGETS seq_ledger
    EXPORT seq_ledgerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES ${SEQ_LEDGER_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/seq_ledger
)

install(EXPORT seq_ledgerTargets
    NAMESPACE seq_ledger::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/seq_ledger
)

# ---- Tests ----
if(SEQ_LEDGER_BUILD_TESTS)
    enable_testing()
    add_executable(seq_ledger_test tests/seq_ledger_test.cpp)
    target_link_libraries(seq_ledger_test PRIVATE seq_ledger)
    add_test(NAME seq_ledger_test COMMAND seq_ledger_test)
endif()

# ---- Examples ----
if(SEQ_LEDGER_BUILD_EXAMPLES)
    add_executable(example_basic examples/basic_usage.cpp)
    target_link_libraries(example_basic PRIVATE seq_ledger)
endif()
