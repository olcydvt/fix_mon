# ResolveSQLite3.cmake
#
# Produces the imported target SQLite::SQLite3 by whichever route is available,
# so the rest of the build never has to care where SQLite came from.
#
# Resolution order (FIXMON_SQLITE_PROVIDER controls it):
#   auto    - system first, then a local directory, then download   [default]
#   system  - system only; hard error if missing
#   local   - use FIXMON_SQLITE_SOURCE_DIR only (airgapped builds)
#   fetch   - download only, ignoring any system copy
#
# Why system first: a distro package is patched and gets security updates
# through the normal channel. A vendored amalgamation is frozen at whatever
# version you pinned, and SQLite does get CVEs. Download is the convenience
# path, not the preferred one.

include_guard(GLOBAL)
include(FetchContent)

set(FIXMON_SQLITE_PROVIDER "auto" CACHE STRING
    "Where to get SQLite: auto | system | local | fetch")
set_property(CACHE FIXMON_SQLITE_PROVIDER PROPERTY STRINGS auto system local fetch)

# sqlite.org keeps every release forever under a year directory. The version
# digits are MAJOR MINOR(2) PATCH(2) 00 -- 3.46.0 becomes 3460000.
#
# NOTE: this default pair was not verified against sqlite.org at the time this
# file was written. If the download 404s, set FIXMON_SQLITE_YEAR and
# FIXMON_SQLITE_VERSION_ID to any released pair, or point FIXMON_SQLITE_URL
# straight at a mirror you trust.
set(FIXMON_SQLITE_YEAR       "2024"    CACHE STRING "sqlite.org release-year directory")
set(FIXMON_SQLITE_VERSION_ID "3460000" CACHE STRING "sqlite.org amalgamation version id")

set(FIXMON_SQLITE_URL
    "https://www.sqlite.org/${FIXMON_SQLITE_YEAR}/sqlite-amalgamation-${FIXMON_SQLITE_VERSION_ID}.zip"
    CACHE STRING "URL of a SQLite amalgamation archive")

# Empty means no integrity check. Pin this for anything that ships.
set(FIXMON_SQLITE_SHA256 "" CACHE STRING
    "Expected SHA256 of FIXMON_SQLITE_URL. Strongly recommended.")

# For builds with no outbound network: point at a directory holding sqlite3.c
# and sqlite3.h.
set(FIXMON_SQLITE_SOURCE_DIR "" CACHE PATH
    "Directory containing a pre-downloaded sqlite3.c / sqlite3.h")

# ---------------------------------------------------------------------------

function(_fixmon_build_sqlite_from_dir src_dir)
  if(NOT EXISTS "${src_dir}/sqlite3.c" OR NOT EXISTS "${src_dir}/sqlite3.h")
    message(FATAL_ERROR
      "No sqlite3.c / sqlite3.h in '${src_dir}'.\n"
      "Expected an unpacked SQLite amalgamation.")
  endif()

  add_library(sqlite3_bundled STATIC "${src_dir}/sqlite3.c")
  target_include_directories(sqlite3_bundled SYSTEM PUBLIC "${src_dir}")

  # Build options tuned for an embedded event store. Nothing here changes SQL
  # semantics we rely on; they trim features this process never uses.
  target_compile_definitions(sqlite3_bundled PUBLIC
    SQLITE_THREADSAFE=1            # the store is single-threaded today, but
                                   # serialized mode costs little and removes a
                                   # footgun if that ever changes
    SQLITE_DQS=0                   # reject double-quoted string literals, a
                                   # legacy misfeature that hides typos
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_MAX_EXPR_DEPTH=0
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_LOAD_EXTENSION     # also drops the libdl dependency
    SQLITE_USE_ALLOCA
  )

  # sqlite3.c is generated code and will not survive -Wall -Wextra -Wpedantic.
  if(MSVC)
    target_compile_options(sqlite3_bundled PRIVATE /w)
  else()
    target_compile_options(sqlite3_bundled PRIVATE -w)
  endif()

  set_target_properties(sqlite3_bundled PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99)

  find_package(Threads REQUIRED)
  target_link_libraries(sqlite3_bundled PUBLIC Threads::Threads)
  if(UNIX AND NOT APPLE)
    target_link_libraries(sqlite3_bundled PUBLIC m)
  endif()

  add_library(SQLite::SQLite3 ALIAS sqlite3_bundled)
  set(FIXMON_SQLITE_ORIGIN "bundled (${src_dir})" PARENT_SCOPE)
endfunction()

function(_fixmon_fetch_sqlite)
  message(STATUS "SQLite: downloading ${FIXMON_SQLITE_URL}")

  set(_hash_arg "")
  if(FIXMON_SQLITE_SHA256)
    set(_hash_arg URL_HASH "SHA256=${FIXMON_SQLITE_SHA256}")
  else()
    message(WARNING
      "SQLite is being downloaded without an integrity check. "
      "Set FIXMON_SQLITE_SHA256 before using this build anywhere real.")
  endif()

  FetchContent_Declare(sqlite3_amalgamation
    URL                        "${FIXMON_SQLITE_URL}"
    ${_hash_arg}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    # Some mirrors ship their own CMakeLists. We compile the amalgamation
    # ourselves so every route produces an identically configured target, so
    # point SOURCE_SUBDIR at a path that does not exist to stop
    # FetchContent_MakeAvailable from calling add_subdirectory.
    SOURCE_SUBDIR              do_not_add_subdirectory
  )
  FetchContent_MakeAvailable(sqlite3_amalgamation)

  # Layouts differ slightly between sqlite.org and mirrors; locate the source.
  file(GLOB_RECURSE _found "${sqlite3_amalgamation_SOURCE_DIR}/sqlite3.c")
  if(NOT _found)
    message(FATAL_ERROR
      "Downloaded archive contains no sqlite3.c: ${FIXMON_SQLITE_URL}")
  endif()
  list(GET _found 0 _sqlite_c)
  get_filename_component(_dir "${_sqlite_c}" DIRECTORY)

  _fixmon_build_sqlite_from_dir("${_dir}")
  set(FIXMON_SQLITE_ORIGIN "downloaded ${FIXMON_SQLITE_URL}" PARENT_SCOPE)
endfunction()

function(fixmon_resolve_sqlite3)
  if(TARGET SQLite::SQLite3)
    return()
  endif()

  set(_p "${FIXMON_SQLITE_PROVIDER}")

  # --- system ---
  if(_p STREQUAL "auto" OR _p STREQUAL "system")
    # FindSQLite3 ships with CMake >= 3.14 and also picks up vcpkg and Conan
    # toolchains transparently.
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
      message(STATUS "SQLite: using system ${SQLite3_VERSION}")
      set(FIXMON_SQLITE_ORIGIN "system ${SQLite3_VERSION}" PARENT_SCOPE)
      return()
    endif()
    if(_p STREQUAL "system")
      message(FATAL_ERROR
        "FIXMON_SQLITE_PROVIDER=system but no SQLite development package was found.\n"
        "  Debian/Ubuntu : sudo apt install libsqlite3-dev\n"
        "  RHEL/Fedora   : sudo dnf install sqlite-devel\n"
        "  macOS         : brew install sqlite3\n"
        "  vcpkg         : vcpkg install sqlite3\n"
        "Or drop the setting to let CMake download it.")
    endif()
    message(STATUS "SQLite: no system package found, falling back")
  endif()

  # --- local directory ---
  if((_p STREQUAL "auto" OR _p STREQUAL "local") AND FIXMON_SQLITE_SOURCE_DIR)
    message(STATUS "SQLite: building from ${FIXMON_SQLITE_SOURCE_DIR}")
    _fixmon_build_sqlite_from_dir("${FIXMON_SQLITE_SOURCE_DIR}")
    set(FIXMON_SQLITE_ORIGIN "${FIXMON_SQLITE_ORIGIN}" PARENT_SCOPE)
    return()
  endif()
  if(_p STREQUAL "local")
    message(FATAL_ERROR
      "FIXMON_SQLITE_PROVIDER=local but FIXMON_SQLITE_SOURCE_DIR is not set.")
  endif()

  # --- download ---
  _fixmon_fetch_sqlite()
  set(FIXMON_SQLITE_ORIGIN "${FIXMON_SQLITE_ORIGIN}" PARENT_SCOPE)
endfunction()
