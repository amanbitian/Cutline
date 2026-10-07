# Vendored SQLite amalgamation. Keeping the sources in-tree makes the native
# build reproducible without a package manager, which matters because the
# project store is the one dependency every other module needs to compile.
add_library(cutline_sqlite3 STATIC sqlite3.c)
add_library(SQLite::SQLite3 ALIAS cutline_sqlite3)

target_include_directories(cutline_sqlite3 SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Compile-time configuration. WAL, backup, and foreign keys are load-bearing for
# ProjectStore; the omitted features shrink the binary without affecting us.
target_compile_definitions(cutline_sqlite3 PUBLIC
  SQLITE_ENABLE_COLUMN_METADATA
  SQLITE_DQS=0
  SQLITE_DEFAULT_FOREIGN_KEYS=1
  SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
  SQLITE_LIKE_DOESNT_MATCH_BLOBS
  SQLITE_MAX_EXPR_DEPTH=0
  SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_SHARED_CACHE
  SQLITE_USE_ALLOCA
  SQLITE_THREADSAFE=1
  # The session extension records row-level changesets and can invert them.
  # Undo/redo is built on this instead of snapshotting the whole project.
  SQLITE_ENABLE_SESSION
  SQLITE_ENABLE_PREUPDATE_HOOK
)

if(MSVC)
  # The amalgamation is third-party C; its warnings are not actionable for us.
  target_compile_options(cutline_sqlite3 PRIVATE /W0)
else()
  target_compile_options(cutline_sqlite3 PRIVATE -w)
endif()
