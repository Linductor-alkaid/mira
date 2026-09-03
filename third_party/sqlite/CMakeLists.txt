# Pinned SQLite amalgamation (see README.md for provenance). Compiled as a
# plain C static library: no Mira warning policy, no clang-tidy, no install
# export. Mira code reaches it only through the mira_state_store boundary.
project(MiraSQLite LANGUAGES C)
add_library(sqlite3 STATIC sqlite3.c)
add_library(Mira::sqlite3 ALIAS sqlite3)
# SYSTEM so consumers' -Wold-style-cast never fires inside the amalgamation.
target_include_directories(sqlite3 SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
set_target_properties(sqlite3 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99
)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_FTS5
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DQS=0
)
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(sqlite3 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()
