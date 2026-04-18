function(get_rocksdb_version out_var)
  set(_version_header "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../include/rocksdb/version.h")
  if(NOT EXISTS "${_version_header}")
    message(FATAL_ERROR "RocksDB version header not found: ${_version_header}")
  endif()

  file(STRINGS "${_version_header}" _major_line REGEX "^#define ROCKSDB_MAJOR[ \t]+[0-9]+$")
  file(STRINGS "${_version_header}" _minor_line REGEX "^#define ROCKSDB_MINOR[ \t]+[0-9]+$")
  file(STRINGS "${_version_header}" _patch_line REGEX "^#define ROCKSDB_PATCH[ \t]+[0-9]+$")

  string(REGEX REPLACE "^#define ROCKSDB_MAJOR[ \t]+([0-9]+)$" "\\1" _major "${_major_line}")
  string(REGEX REPLACE "^#define ROCKSDB_MINOR[ \t]+([0-9]+)$" "\\1" _minor "${_minor_line}")
  string(REGEX REPLACE "^#define ROCKSDB_PATCH[ \t]+([0-9]+)$" "\\1" _patch "${_patch_line}")

  if(_major STREQUAL "" OR _minor STREQUAL "" OR _patch STREQUAL "")
    message(FATAL_ERROR "Failed to parse RocksDB version from ${_version_header}")
  endif()

  set(${out_var} "${_major}.${_minor}.${_patch}" PARENT_SCOPE)
endfunction()
