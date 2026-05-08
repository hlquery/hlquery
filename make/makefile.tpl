#!/usr/bin/make -f
#
# hlquery - Search beyond keywords.
# https://www.hlquery.com
#
# Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
#
# This file is part of hlquery, released under the BSD License version 3.
# You are free to redistribute and/or modify this software
# under the terms of the BSD License.
# For more details, please visit: https://docs.hlquery.com

# This project uses GNU make features extensively.
# If your system `make` is BSD make (common on macOS/*BSD), run `gmake` instead.
ifndef MAKE_VERSION
$(error GNU make is required. Run: gmake <target>)
endif

# ANSI color codes for terminal output
# Only use colors if output is to a TTY (not in pipes or Docker without TTY)
ifeq ($(shell [ -t 1 ] && echo yes),yes)
  RED     = \033[0;31m
  YELLOW  = \033[0;33m
  GREEN   = \033[0;32m
  BLUE    = \033[1;34m
  CYAN    = \033[0;36m
  NC      = \033[0m
  BOLD    = \033[1m
else
  RED     =
  YELLOW  =
  GREEN   =
  BLUE    =
  CYAN    =
  NC      =
  BOLD    =
endif

# Set default target to 'all' (build everything)
.DEFAULT_GOAL := all

CONFIGURE_COMMAND := ${CONFIGURE_COMMAND}

# BUILD CONFIGURATION

# Build mode: release (default), debug, profile, sanitize
BUILD_MODE ?= release

# Compiler selection
CC       ?= cc
CXX      = ${CXX}
CPPFLAGS ?=
EXTRA_LDFLAGS ?=
TLS_CFLAGS ?= ${TLS_CFLAGS}
TLS_LDFLAGS ?= ${TLS_LDFLAGS}
OS_NAME := $(shell uname -s 2>/dev/null || echo unknown)
ARCH_NAME := $(shell uname -m 2>/dev/null || echo unknown)
FS_LIB := -lstdc++fs
ifneq ($(filter FreeBSD OpenBSD NetBSD DragonFly Darwin,$(OS_NAME)),)
  FS_LIB :=
endif

PKG_TLS_CFLAGS_OTHER = $(shell if pkg-config --exists openssl 2>/dev/null; then pkg-config --cflags-only-other openssl 2>/dev/null; elif pkg-config --exists gnutls 2>/dev/null; then pkg-config --cflags-only-other gnutls 2>/dev/null; fi)
PKG_TLS_CFLAGS_INCLUDE = $(shell if pkg-config --exists openssl 2>/dev/null; then pkg-config --cflags-only-I openssl 2>/dev/null; elif pkg-config --exists gnutls 2>/dev/null; then pkg-config --cflags-only-I gnutls 2>/dev/null; fi)
ifeq ($(strip $(TLS_CFLAGS)),)
  TLS_CFLAGS = $(PKG_TLS_CFLAGS_OTHER) $(patsubst -I%,-isystem %,$(PKG_TLS_CFLAGS_INCLUDE))
endif
ifeq ($(strip $(TLS_LDFLAGS)),)
  TLS_LDFLAGS = $(shell if pkg-config --exists openssl 2>/dev/null; then pkg-config --libs openssl 2>/dev/null; elif pkg-config --exists gnutls 2>/dev/null; then pkg-config --libs gnutls 2>/dev/null; else printf '%s' '-lssl -lcrypto'; fi)
endif

# RocksDB configuration
# IMPORTANT: If vendor/rocksdb/build/librocksdb.a exists, NO APT-GET IS NEEDED!
# The static library is self-contained and only requires basic system libraries
# (zlib, pthread, dl) which are always available on Linux systems.
# NOTE: librocksdb.a is architecture and OS-specific. A library built on Linux x86_64
# won't work on macOS or ARM Linux. The build system will automatically rebuild RocksDB
# from source if needed for the target platform (if vendor/rocksdb source exists).
ROCKSDB_DIR ?= vendor/rocksdb
ROCKSDB_BUILD_DIR ?= $(ROCKSDB_DIR)/build
ROCKSDB_LIB = $(ROCKSDB_BUILD_DIR)/librocksdb.a
ROCKSDB_JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
ROCKSDB_BUILD_PARALLEL := -j$(ROCKSDB_JOBS)
ifneq ($(findstring --jobserver-auth,$(MAKEFLAGS)),)
  ROCKSDB_BUILD_PARALLEL :=
endif
ROCKSDB_INCLUDE = $(if $(wildcard $(ROCKSDB_DIR)/include),-I$(ROCKSDB_DIR)/include,)

# Base compiler flags
# Security hardening flags for production builds
SECURITY_FLAGS = -fstack-protector-strong
# Define ROCKSDB_NAMESPACE to suppress warnings from RocksDB headers
ROCKSDB_DEFINES = -DROCKSDB_NAMESPACE=rocksdb
CONFIGURE_CXXFLAGS = -std=c++17 -O2 -fPIC ${TLS_CFLAGS} $(CPPFLAGS)
CONFIGURE_CFLAGS = -std=c11 -O2 -fPIC ${TLS_CFLAGS} $(CPPFLAGS)
BASE_CXXFLAGS = $(CONFIGURE_CXXFLAGS) -Iinclude -Iinclude/common -I. -Isrc -Ivendor -Ibuild/include $(ROCKSDB_INCLUDE) $(ROCKSDB_DEFINES) -std=c++20 -fPIC -pipe $(SECURITY_FLAGS)
BASE_CFLAGS = $(CONFIGURE_CFLAGS) -Iinclude -Iinclude/common -I. -Isrc -Ivendor -Ibuild/include $(ROCKSDB_INCLUDE) -fPIC -pipe $(SECURITY_FLAGS)

# Warning flags - comprehensive set for better code quality
# Note: -Wconversion and -Wsign-conversion are very noisy, so we use them selectively
# -Wpedantic is disabled for flexible array members
# -Wstrict-overflow is disabled as it's too noisy with optimized code and produces many false positives
# -Wnull-dereference is disabled due to false positives from inlined standard library code (streambuf)
# Additional flags for production-grade C++ projects
WARNING_FLAGS = -Wall -Wextra \
                -Wformat=2 -Wformat-security -Wformat-signedness \
                -Wno-unused-parameter -Wno-missing-field-initializers \
                -Wcast-qual -Wcast-align \
                -Wold-style-cast -Woverloaded-virtual \
                -Wshadow -Wpointer-arith -Winit-self \
                -Wmissing-include-dirs \
                -Wno-double-promotion \
                -Wno-pedantic -Wno-strict-overflow \
                -Wno-free-nonheap-object \
                -Wno-undef

# Warning flags for vendor C files (less strict - they're third-party code)
VENDOR_C_WARNING_FLAGS = -Wall -Wextra \
                         -Wformat=2 -Wformat-security \
                         -Wno-unused-parameter -Wno-missing-field-initializers \
                         -Wno-pedantic \
                         -Wno-strict-overflow

# Warning flags for vendor C++ files (fmt library, etc.)
VENDOR_CXX_WARNING_FLAGS = -Wall -Wextra \
                           -Wformat=2 -Wformat-security \
                           -Wno-unused-parameter -Wno-missing-field-initializers \
                           -Wno-pedantic -Wno-strict-overflow

# Clang warns about infinity/NaN when -ffast-math is enabled.
# This is expected for vendored json code and creates excessive noise.
IS_CLANG := $(shell $(CXX) --version 2>/dev/null | grep -qi clang && echo 1 || echo 0)
ifeq ($(IS_CLANG),1)
  WARNING_FLAGS += -Wno-nan-infinity-disabled
  VENDOR_CXX_WARNING_FLAGS += -Wno-nan-infinity-disabled
else
  WARNING_FLAGS += -Wlogical-op -Wduplicated-cond -Wduplicated-branches
endif

# Benchmark sources pull in vendored json.hpp heavily; keep clang output usable.
BENCHMARK_CXX_WARNING_FLAGS =
ifeq ($(IS_CLANG),1)
  BENCHMARK_CXX_WARNING_FLAGS += -Wno-nan-infinity-disabled
endif

# Build mode specific flags
ifeq ($(BUILD_MODE),debug)
  # Debug build: no optimization, debug symbols, assertions enabled
  OPT_FLAGS = -O0 -g3 -DDEBUG -UNDEBUG
  LTO_FLAGS =
  STRIP_FLAGS =
else ifeq ($(BUILD_MODE),profile)
  # Profile build: optimization with profiling support
  OPT_FLAGS = -O2 -g -fno-omit-frame-pointer -DNDEBUG -D_FORTIFY_SOURCE=2
  LTO_FLAGS = -flto=auto
  STRIP_FLAGS =
else ifeq ($(BUILD_MODE),sanitize)
  # Sanitizer build: address, undefined behavior, and thread sanitizers
  OPT_FLAGS = -O1 -g -fsanitize=address,undefined,thread -fno-omit-frame-pointer -DDEBUG
  LTO_FLAGS =
  STRIP_FLAGS =
  LDFLAGS += -fsanitize=address,undefined,thread
else ifeq ($(BUILD_MODE),coverage)
  # Coverage build: for code coverage analysis
  OPT_FLAGS = -O0 -g --coverage -DDEBUG
  LTO_FLAGS =
  STRIP_FLAGS =
  LDFLAGS += --coverage
else
  # Release build (default): maximum optimization
  # -D_FORTIFY_SOURCE=2 requires -O2 or higher (we use -O3)
  OPT_FLAGS = -O3 -DNDEBUG -D_FORTIFY_SOURCE=2
  LTO_FLAGS = -flto=auto
  STRIP_FLAGS = -Wl,--strip-all
endif

# Architecture-specific optimizations
ARCH_FLAGS =
ifneq ($(OS_NAME),Darwin)
  ARCH_FLAGS += -march=native -mtune=native
endif
OPT_FLAGS += $(ARCH_FLAGS)

# Link-time optimization (LTO) - improves performance significantly
LTO_CXXFLAGS = $(LTO_FLAGS)
LTO_LDFLAGS = $(LTO_FLAGS)

# FreeBSD/BSD toolchains can emit noisy/invalid GNU jobserver warnings during GCC LTO.
# Disable LTO there for reliable parallel builds.
ifneq ($(filter FreeBSD OpenBSD NetBSD DragonFly,$(OS_NAME)),)
  LTO_FLAGS =
  LTO_CXXFLAGS =
  LTO_LDFLAGS =
endif

# Advanced optimization flags for release builds
ifeq ($(BUILD_MODE),release)
  # Additional performance optimizations
  OPT_FLAGS += -funroll-loops -ffast-math -finline-functions \
               -fomit-frame-pointer -fstrict-aliasing
  # x86-only CPU tuning flags
  ifneq ($(filter x86_64 amd64 i386 i686,$(ARCH_NAME)),)
    OPT_FLAGS += -mfpmath=sse -msse4.2
  endif
endif

# Combine all CXXFLAGS
CXXFLAGS = $(BASE_CXXFLAGS) $(OPT_FLAGS) $(WARNING_FLAGS) $(LTO_CXXFLAGS)

# Linker flags
# Security hardening: RELRO (Read-Only Relocations)
# -Wl,-z,relro: Make relocation sections read-only
# -Wl,-z,now: Immediate binding (optional, can impact startup time)
# If using vendor RocksDB, use static linking with bundled dependencies
# Otherwise, use system libraries
# Check if RocksDB source exists (not just the built library, since clean removes it)
# Check for CMakeLists.txt to ensure it's a complete RocksDB source tree
HAS_ROCKSDB_SOURCE = $(wildcard $(ROCKSDB_DIR)/CMakeLists.txt)
ifeq ($(HAS_ROCKSDB_SOURCE),)
  # System-wide RocksDB: use dynamic linking
  # NOTE: This requires apt-get install librocksdb-dev (or equivalent)
  USE_VENDOR_ROCKSDB = 
  BASE_LDFLAGS = -lz -ldl $(FS_LIB) -pthread -lsnappy -llz4 -lzstd -lbz2 -lrocksdb
  ROCKSDB_LDFLAGS = 
else
  # Vendor RocksDB: static linking with bundled dependencies
  # NO APT-GET NEEDED! librocksdb.a contains everything statically linked.
  # Only requires basic system libraries (zlib, pthread, dl) which are always available.
  USE_VENDOR_ROCKSDB = $(ROCKSDB_LIB)
  BASE_LDFLAGS = -lz -ldl $(FS_LIB) -pthread
  ROCKSDB_LDFLAGS = $(ROCKSDB_LIB)
endif
CONFIGURE_LDFLAGS = -ldl $(FS_LIB) -pthread ${TLS_LDFLAGS} $(EXTRA_LDFLAGS)
EXTRA_LD_HARDEN_FLAGS = -Wl,--as-needed -Wl,-z,relro
ifneq ($(filter FreeBSD OpenBSD NetBSD DragonFly Darwin,$(OS_NAME)),)
  EXTRA_LD_HARDEN_FLAGS =
endif
LDFLAGS = $(CONFIGURE_LDFLAGS) $(BASE_LDFLAGS) $(ROCKSDB_LDFLAGS) $(LTO_LDFLAGS) $(STRIP_FLAGS) $(EXTRA_LD_HARDEN_FLAGS)
MODULE_SHARED_LDFLAGS =

ifeq ($(OS_NAME),Darwin)
  STRIP_FLAGS =
  RDYNAMIC ?= -Wl,-export_dynamic
  MODULE_SHARED_LDFLAGS = -Wl,-undefined,dynamic_lookup
else
  RDYNAMIC ?= -rdynamic
endif

# Parallel compilation
MAKEFLAGS += -j$(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# FEATURE FLAGS

# Optional jemalloc linkage (enable with WITH_JEMALLOC=1)
ifeq ($(WITH_JEMALLOC),1)
  LDFLAGS += -ljemalloc
  CXXFLAGS += -DWITH_JEMALLOC
endif

# Optional tcmalloc linkage (enable with WITH_TCMALLOC=1)
ifeq ($(WITH_TCMALLOC),1)
  LDFLAGS += -ltcmalloc
  CXXFLAGS += -DWITH_TCMALLOC
endif

# Profile-Guided Optimization (PGO) support
ifeq ($(USE_PGO),1)
  ifeq ($(BUILD_MODE),release)
    CXXFLAGS += -fprofile-generate
    LDFLAGS += -fprofile-generate
  else ifeq ($(USE_PGO),2)
    CXXFLAGS += -fprofile-use -fprofile-correction
    LDFLAGS += -fprofile-use
  endif
endif

# Static analysis support (clang-tidy)
ifeq ($(USE_CLANG_TIDY),1)
  CLANG_TIDY := $(shell which clang-tidy 2>/dev/null)
  ifneq ($(CLANG_TIDY),)
    CXX := $(CLANG_TIDY) $(CXX)
  endif
endif

# DIRECTORY STRUCTURE

OBJ_DIR     = build/obj
BIN_DIR     = build/bin
SRC_DIR     = src
INC_DIR     = include
CONFIG_HEADER = $(INC_DIR)/core/config.h
VENDOR_DIR  = vendor
RUN_DIR     = run

# SOURCE FILE DISCOVERY

# Source files discovery with explicit exclusions
SRCS_TOP := $(filter-out $(SRC_DIR)/test_server.cpp $(SRC_DIR)/test_config.cpp $(SRC_DIR)/hlquery_main.cpp $(SRC_DIR)/benchmarkmain.cpp $(SRC_DIR)/benchmark.cpp $(SRC_DIR)/compaction_manager.cpp $(SRC_DIR)/dynamic.cpp, $(wildcard $(SRC_DIR)/*.cpp))
SRCS_TOP := $(filter-out %/, $(SRCS_TOP))

# Common/shared source files
COMMON_SRCS := $(filter-out $(SRC_DIR)/common/options.cpp $(SRC_DIR)/common/shim.cpp,$(wildcard $(SRC_DIR)/common/*.cpp))
SRCS_TOP += $(COMMON_SRCS)

# Core source files
CORE_SRCS := $(wildcard $(SRC_DIR)/core/*.cpp)
SRCS_TOP += $(CORE_SRCS)

# Runtime source files
RUNTIME_SRCS := $(wildcard $(SRC_DIR)/runtime/*.cpp)
SRCS_TOP += $(RUNTIME_SRCS)

# Utils source files
UTILS_SRCS := $(wildcard $(SRC_DIR)/utils/*.cpp)
SRCS_TOP += $(UTILS_SRCS)

# API source files
# Exclude httpserver.cpp and split searchapi*.cpp files as they're in HTTP_SRCS to avoid duplicate linking
API_SRCS := $(filter-out $(SRC_DIR)/api/httpserver.cpp $(SRC_DIR)/api/searchapi.cpp $(SRC_DIR)/api/searchapi%.cpp, $(wildcard $(SRC_DIR)/api/*.cpp))
SRCS_TOP += $(API_SRCS)

# Search storage source files
SEARCH_SRCS := $(filter-out $(SRC_DIR)/search/context.cpp,$(wildcard $(SRC_DIR)/search/*.cpp))
SEARCH_SRCS += $(wildcard $(SRC_DIR)/sam/*.cpp)
SRCS_TOP += $(SEARCH_SRCS)

SQL_SRCS := $(wildcard $(SRC_DIR)/sql/*.cpp)
SRCS_TOP += $(SQL_SRCS)

# Runtime-loadable modules
CORE_MODULE_SRCS := $(wildcard $(SRC_DIR)/modules/core_*.cpp) $(wildcard $(SRC_DIR)/modules/m_core_*.cpp)
MODULE_SIMPLE_SRCS := $(CORE_MODULE_SRCS) $(filter-out $(CORE_MODULE_SRCS),$(wildcard $(SRC_DIR)/modules/m*.cpp))
EXTRA_MODULE_SIMPLE_SRCS := ${EXTRA_MODULE_SIMPLE_SRCS}
EXTRA_MODULE_DIR_SRCS := ${EXTRA_MODULE_DIR_SRCS}
EXTRA_MODULE_LIBS := ${EXTRA_MODULE_LIBS}
EXTRA_MODULE_KNOWN_NAMES := ${EXTRA_MODULE_KNOWN_NAMES}
EXTRA_MODULE_ENABLED_NAMES := ${EXTRA_MODULE_ENABLED_NAMES}
MODULE_DIRS := $(filter-out $(EXTRA_MODULE_ENABLED_NAMES),$(notdir $(shell find -L $(SRC_DIR)/modules -mindepth 1 -maxdepth 1 -type d -name 'm_*' 2>/dev/null)))
MODULE_DIR_SRCS := $(foreach mod,$(MODULE_DIRS),$(wildcard $(SRC_DIR)/modules/$(mod)/*.cpp))
MODULE_SIMPLE_SRCS += $(EXTRA_MODULE_SIMPLE_SRCS)
MODULE_DIR_SRCS += $(EXTRA_MODULE_DIR_SRCS)

# HTTP server and search sources
# Note: LSM files (advanced_search, lexical_index, collection_store, document_storage) are already
# included in LSM_SRCS -> SRCS_TOP -> REGULAR_OBJS, so we don't duplicate them here
# Include main searchapi.cpp and all split searchapi*.cpp files
HTTP_SRCS := $(SRC_DIR)/api/httpserver.cpp $(SRC_DIR)/api/searchapi.cpp $(filter-out $(SRC_DIR)/api/searchapi.cpp,$(wildcard $(SRC_DIR)/api/searchapi*.cpp))

# Socket engine sources
SOCKETENGINE_FILE := ${SOCKETENGINE_FILE}
ifeq ($(SOCKETENGINE_FILE),)
SOCKETENGINE_FILE := epoll.cpp
endif
SOCKETENGINE_SRC := $(SRC_DIR)/socketengines/$(SOCKETENGINE_FILE)

# Separate regular sources from HTTP server for special linker handling
REGULAR_SRCS = $(SRCS_TOP) $(wildcard $(SRC_DIR)/monitors/*.cpp) $(SOCKETENGINE_SRC)

# OBJECT FILES AND DEPENDENCIES

REGULAR_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(REGULAR_SRCS))
HTTP_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(HTTP_SRCS))
OBJS := $(REGULAR_OBJS) $(HTTP_OBJS)
DEPS := $(OBJS:.o=.d)
MODULE_SIMPLE_OBJS := $(patsubst $(SRC_DIR)/modules/%.cpp,$(OBJ_DIR)/modules/%.module.o,$(MODULE_SIMPLE_SRCS))
MODULE_DIR_OBJS := $(patsubst $(SRC_DIR)/modules/%.cpp,$(OBJ_DIR)/modules/%.module.o,$(MODULE_DIR_SRCS))
MODULE_OBJS := $(MODULE_SIMPLE_OBJS) $(MODULE_DIR_OBJS)
MODULE_SIMPLE_LIBS := $(patsubst $(SRC_DIR)/modules/%.cpp,$(RUN_DIR)/modules/%.so,$(MODULE_SIMPLE_SRCS))
MODULE_DIR_LIBS := $(foreach mod,$(MODULE_DIRS),$(RUN_DIR)/modules/$(mod).so)
MODULE_LIBS := $(MODULE_SIMPLE_LIBS) $(MODULE_DIR_LIBS) $(EXTRA_MODULE_LIBS)
DEPS += $(MODULE_OBJS:.o=.d)

# Vendor/allocator objects
FMT_OBJ := $(OBJ_DIR)/vendor/fmt/format.o
SHA2_OBJ := $(OBJ_DIR)/vendor/sha2/sha2.o
MD5_OBJ := $(OBJ_DIR)/vendor/md5/md5.o
REGULAR_ALL_OBJS = $(REGULAR_OBJS) $(FMT_OBJ) $(SHA2_OBJ) $(MD5_OBJ)
ALL_OBJS = $(REGULAR_ALL_OBJS) $(HTTP_OBJS)

# BUILD TARGETS

prepare: rocksdb-check rocksdb-preflight binary-compat-check $(ROCKSDB_LIB) prune-disabled-extra-modules
	@mkdir -p $(OBJ_DIR) $(BIN_DIR)
	@mkdir -p $(OBJ_DIR)/core $(OBJ_DIR)/runtime $(OBJ_DIR)/utils $(OBJ_DIR)/api $(OBJ_DIR)/search $(OBJ_DIR)/sql $(OBJ_DIR)/socketengines $(OBJ_DIR)/timers $(OBJ_DIR)/cli $(OBJ_DIR)/talk $(OBJ_DIR)/modules $(OBJ_DIR)/vendor/fmt $(OBJ_DIR)/vendor/sha2 $(OBJ_DIR)/vendor/md5
	@mkdir -p $(RUN_DIR)/bin $(RUN_DIR)/conf $(RUN_DIR)/data $(RUN_DIR)/logs $(RUN_DIR)/modules $(RUN_DIR)/pid
	@mkdir -p $(INC_DIR)
	@# Fix permissions for Docker volume mounts - ensure entire build tree is writable
	@if [ -d "$(OBJ_DIR)" ]; then \
		find $(OBJ_DIR) -type d -exec chmod u+w {} + 2>/dev/null || true; \
		find $(OBJ_DIR) -type f -exec chmod u+w {} + 2>/dev/null || true; \
	fi

# Ensure we don't treat an empty RocksDB archive as valid.
rocksdb-check:
	@if [ -f "$(ROCKSDB_LIB)" ] && [ ! -s "$(ROCKSDB_LIB)" ]; then \
		echo "$(YELLOW)Warning: empty RocksDB archive detected at $(ROCKSDB_LIB)$(NC)"; \
		echo "$(YELLOW)Removing empty archive to force rebuild$(NC)"; \
		rm -f "$(ROCKSDB_LIB)" 2>/dev/null || true; \
	fi
	@if [ "$$(uname -s)" = "FreeBSD" ] && [ -f "$(ROCKSDB_LIB)" ] && grep -aqE "__errno_location|__libc_single_threaded|__isoc23_strtol" "$(ROCKSDB_LIB)"; then \
		echo "$(YELLOW)Warning: Linux/glibc RocksDB archive detected on FreeBSD at $(ROCKSDB_LIB)$(NC)"; \
		echo "$(YELLOW)Removing incompatible RocksDB archive to force local rebuild$(NC)"; \
		rm -f "$(ROCKSDB_LIB)" 2>/dev/null || true; \
	fi
	@if [ -d "$(BIN_DIR)" ]; then \
		find $(BIN_DIR) -type d -exec chmod u+w {} + 2>/dev/null || true; \
		find $(BIN_DIR) -type f -exec chmod u+w {} + 2>/dev/null || true; \
	fi
	@([ "$$(id -u)" != "0" ] && chown -R $$(id -u):$$(id -g) $(OBJ_DIR) $(BIN_DIR) 2>/dev/null || true) || true

rocksdb-preflight:
	@if [ -f "$(ROCKSDB_DIR)/include/rocksdb/cache.h" ]; then \
		:; \
	elif printf '#include <rocksdb/cache.h>\n' | $(CXX) $(BASE_CXXFLAGS) -x c++ -E - >/dev/null 2>&1; then \
		:; \
	else \
		echo "$(RED)Error: RocksDB headers not found$(NC)" >&2; \
		echo "$(YELLOW)Expected vendored headers under $(ROCKSDB_DIR)/include/rocksdb$(NC)" >&2; \
		echo "$(YELLOW)Or install system-wide RocksDB headers (example: sudo apt-get install librocksdb-dev)$(NC)" >&2; \
		exit 1; \
	fi

# Remove stale Linux/glibc artifacts when building on FreeBSD/BSD from a shared checkout.
# Otherwise gmake may consider prebuilt targets up to date and skip rebuilding them locally.
binary-compat-check:
	@if [ "$$(uname -s)" = "FreeBSD" ]; then \
		for f in "$(BIN_DIR)/hlquery" "$(BIN_DIR)/hlquery-cli" "$(BIN_DIR)/hlquery-benchmark" "$(BIN_DIR)/hlquery-talk" "$(RUN_DIR)/modules/"*.so; do \
			[ -e "$$f" ] || continue; \
			if grep -aqE "libc\\.so\\.6|ld-linux|GLIBC_" "$$f"; then \
				echo "$(YELLOW)Warning: removing incompatible Linux binary artifact $$f$(NC)"; \
				rm -f "$$f" 2>/dev/null || true; \
			fi; \
		done; \
	fi

# Build RocksDB library (optional - will build if RocksDB source is present)
$(ROCKSDB_LIB):
	@if [ ! -d "$(ROCKSDB_DIR)" ] || [ ! -f "$(ROCKSDB_DIR)/CMakeLists.txt" ]; then \
		echo "$(RED)Error: RocksDB source not found at $(ROCKSDB_DIR)$(NC)" >&2; \
		echo "$(YELLOW)Restore the vendored RocksDB directory from the repository checkout$(NC)" >&2; \
		echo "$(YELLOW)Or install RocksDB system-wide and update ROCKSDB_DIR$(NC)" >&2; \
		exit 1; \
	else \
		echo "$(CYAN)Compiling RocksDB...$(NC)"; \
		if ! command -v cmake >/dev/null 2>&1; then \
			echo "$(RED)Error: cmake is required to build vendor RocksDB but was not found$(NC)" >&2; \
			echo "$(YELLOW)Install cmake (examples): apt-get install cmake | brew install cmake | pkg install cmake$(NC)" >&2; \
			exit 1; \
		fi; \
		mkdir -p $(ROCKSDB_BUILD_DIR) && \
		# Clean CMake cache if paths don't match (Docker volume mount issue) \
			if [ -f "$(ROCKSDB_BUILD_DIR)/CMakeCache.txt" ]; then \
				CACHED_SOURCE=$$(grep "^CMAKE_SOURCE_DIR:" "$(ROCKSDB_BUILD_DIR)/CMakeCache.txt" 2>/dev/null | cut -d'=' -f2 | tr -d '\r\n' || echo ""); \
				CURRENT_SOURCE=$$(cd $(ROCKSDB_DIR) && pwd); \
				CACHED_CXX=$$(grep "^CMAKE_CXX_COMPILER:FILEPATH=" "$(ROCKSDB_BUILD_DIR)/CMakeCache.txt" 2>/dev/null | cut -d'=' -f2 | tr -d '\r\n' || echo ""); \
				CURRENT_CXX=$$(command -v $(CXX) 2>/dev/null || echo "$(CXX)"); \
				if [ "$$CACHED_SOURCE" != "$$CURRENT_SOURCE" ] && [ -n "$$CACHED_SOURCE" ]; then \
					echo "$(YELLOW)Cleaning CMake cache (source path mismatch: $$CACHED_SOURCE != $$CURRENT_SOURCE)$(NC)"; \
					rm -rf "$(ROCKSDB_BUILD_DIR)/CMakeCache.txt" "$(ROCKSDB_BUILD_DIR)/CMakeFiles" 2>/dev/null || true; \
				elif [ "$$CACHED_CXX" != "$$CURRENT_CXX" ] && [ -n "$$CACHED_CXX" ]; then \
					echo "$(YELLOW)Cleaning CMake cache (compiler mismatch: $$CACHED_CXX != $$CURRENT_CXX)$(NC)"; \
					rm -rf "$(ROCKSDB_BUILD_DIR)/CMakeCache.txt" "$(ROCKSDB_BUILD_DIR)/CMakeFiles" 2>/dev/null || true; \
				fi; \
			fi && \
			ROCKSDB_WARN_FLAGS="-Wno-maybe-uninitialized"; \
			if [ "$$(uname -s)" = "FreeBSD" ]; then \
				ROCKSDB_WARN_FLAGS="-Wno-uninitialized -Wno-unknown-warning-option -Wno-error=unknown-warning-option"; \
			elif $(CXX) --version 2>/dev/null | grep -qi clang; then \
				ROCKSDB_WARN_FLAGS="-Wno-uninitialized -Wno-unknown-warning-option -Wno-error=unknown-warning-option"; \
			fi; \
			cmake --log-level=NOTICE -S $(ROCKSDB_DIR) -B $(ROCKSDB_BUILD_DIR) -DCMAKE_BUILD_TYPE=Release \
			      -DCMAKE_CXX_COMPILER="$(CXX)" \
			      -DWITH_TESTS=OFF \
		      -DWITH_TOOLS=OFF \
		      -DWITH_BENCHMARK_TOOLS=OFF \
		      -DROCKSDB_BUILD_SHARED=OFF \
		      -DPORTABLE=ON \
		      -DUSE_RTTI=ON \
		      -DFAIL_ON_WARNINGS=OFF \
		      -DWITH_GFLAGS=OFF \
		      -DWITH_JEMALLOC=OFF \
		      -DWITH_TBB=OFF \
		      -DWITH_SNAPPY=OFF \
		      -DWITH_LZ4=OFF \
		      -DWITH_ZSTD=OFF \
		      -DWITH_BZ2=OFF \
			      -DWITH_BENCHMARK_TOOLS=OFF \
			      -DCMAKE_CXX_FLAGS="-fPIC -Wno-error $$ROCKSDB_WARN_FLAGS" \
			      || { echo "$(RED)Error: CMake configuration failed$(NC)" >&2; echo "$(YELLOW)Try: rm -rf $(ROCKSDB_BUILD_DIR) && make$(NC)" >&2; exit 1; }; \
			cmake --build $(ROCKSDB_BUILD_DIR) --target rocksdb $(ROCKSDB_BUILD_PARALLEL) >/dev/null || { echo "$(RED)Error: RocksDB build failed$(NC)" >&2; exit 1; } && \
		([ "$$(id -u)" != "0" ] && chown -R $$(id -u):$$(id -g) $(ROCKSDB_BUILD_DIR) 2>/dev/null || true) && \
		([ "$$(id -u)" != "0" ] && chmod -R u+w $(ROCKSDB_BUILD_DIR) 2>/dev/null || true) && \
		cd $(ROCKSDB_BUILD_DIR) && \
		(if [ -f librocksdb.a ]; then \
			:; \
		elif [ -f librocksdb_static.a ]; then \
			cp librocksdb_static.a librocksdb.a || { echo "$(RED)Error: Failed to copy librocksdb_static.a$(NC)" >&2; exit 1; }; \
		elif [ -f CMakeFiles/rocksdb.dir/librocksdb.a ]; then \
			cp CMakeFiles/rocksdb.dir/librocksdb.a librocksdb.a || { echo "$(RED)Error: Failed to copy librocksdb.a from CMakeFiles$(NC)" >&2; exit 1; }; \
		else \
			LIB_FILE=$$(find . -name "librocksdb*.a" -type f | head -1); \
			if [ -z "$$LIB_FILE" ]; then \
				echo "$(RED)Error: RocksDB library file not found after compilation$(NC)" >&2; \
				exit 1; \
			fi; \
			cp "$$LIB_FILE" librocksdb.a || { echo "$(RED)Error: Failed to copy RocksDB library$(NC)" >&2; exit 1; }; \
		fi) && \
		if [ ! -f librocksdb.a ]; then \
			echo "$(RED)Error: RocksDB library file not found after compilation$(NC)" >&2; \
			exit 1; \
		fi && \
		chmod u+w librocksdb.a 2>/dev/null || true && \
		chown $$(id -u):$$(id -g) librocksdb.a 2>/dev/null || true && \
		echo "$(GREEN)✓ RocksDB compiled successfully$(NC)"; \
	fi

# Vendor object files (use less strict warnings for third-party code)
$(FMT_OBJ): $(VENDOR_DIR)/fmt/format.cc | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@chmod -R u+w $(dir $@) $(OBJ_DIR) 2>/dev/null || true
	@([ "$$(id -u)" != "0" ] && chown -R $$(id -u):$$(id -g) $(dir $@) 2>/dev/null || true) || true
	$(CXX) $(BASE_CXXFLAGS) $(OPT_FLAGS) $(VENDOR_CXX_WARNING_FLAGS) $(LTO_CXXFLAGS) -MMD -MP -c $< -o $@

# Universal pattern rule for source compilation with auto-deps
# Use order-only prerequisite (|) for directory creation to avoid race conditions in parallel builds
$(OBJ_DIR)/cli/benchmark_%.o: CXXFLAGS += $(BENCHMARK_CXX_WARNING_FLAGS)
$(OBJ_DIR)/search/mindexopt.o: CXXFLAGS += -mavx2

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@chmod -R u+w $(dir $@) $(OBJ_DIR) 2>/dev/null || true
	@([ "$$(id -u)" != "0" ] && chown -R $$(id -u):$$(id -g) $(dir $@) 2>/dev/null || true) || true
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/modules/%.module.o: $(SRC_DIR)/modules/%.cpp | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@chmod -R u+w $(dir $@) $(OBJ_DIR) 2>/dev/null || true
	@([ "$$(id -u)" != "0" ] && chown -R $$(id -u):$$(id -g) $(dir $@) 2>/dev/null || true) || true
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# Per-module annotation flags injected by configure.
${MODULE_ANNOTATION_CXXFLAGS}

$(RUN_DIR)/modules/%.so: $(OBJ_DIR)/modules/%.module.o $(ROCKSDB_LIB) | $(BIN_DIR)
	@mkdir -p $(dir $@)
	@rm -f $(SRC_DIR)/modules/$*.so
	$(CXX) -shared -o $@ $< $(CONFIGURE_LDFLAGS) $(MODULE_SHARED_LDFLAGS) $(MODULE_EXTRA_LDFLAGS)

define MODULE_DIR_RULE
$(RUN_DIR)/modules/$(1).so: $$(patsubst $$(SRC_DIR)/modules/%.cpp,$$(OBJ_DIR)/modules/%.module.o,$$(wildcard $$(SRC_DIR)/modules/$(1)/*.cpp)) $$(ROCKSDB_LIB) | $$(BIN_DIR)
	@mkdir -p $$(dir $$@)
	@rm -f $$(SRC_DIR)/modules/$(1).so
	$(CXX) -shared -o $$@ $$^ $$(CONFIGURE_LDFLAGS) $$(MODULE_SHARED_LDFLAGS) $$(MODULE_EXTRA_LDFLAGS)
endef

# Per-module linker flags injected by configure.
${MODULE_ANNOTATION_LDFLAGS}

# Explicit build rules for enabled extra modules injected by configure.
${EXTRA_MODULE_BUILD_RULES}

$(foreach mod,$(MODULE_DIRS),$(eval $(call MODULE_DIR_RULE,$(mod))))

# Create all object directories as order-only prerequisites
$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)/core $(OBJ_DIR)/runtime $(OBJ_DIR)/utils $(OBJ_DIR)/api $(OBJ_DIR)/search $(OBJ_DIR)/socketengines $(OBJ_DIR)/timers $(OBJ_DIR)/cli $(OBJ_DIR)/talk $(OBJ_DIR)/modules $(OBJ_DIR)/vendor/fmt $(OBJ_DIR)/vendor/sha2 $(OBJ_DIR)/vendor/md5 || \
	(chmod -R u+w $(OBJ_DIR) 2>/dev/null || true; chown -R $$(id -u):$$(id -g) $(OBJ_DIR) 2>/dev/null || true; \
	mkdir -p $(OBJ_DIR)/core $(OBJ_DIR)/runtime $(OBJ_DIR)/utils $(OBJ_DIR)/api $(OBJ_DIR)/search $(OBJ_DIR)/socketengines $(OBJ_DIR)/timers $(OBJ_DIR)/cli $(OBJ_DIR)/talk $(OBJ_DIR)/modules $(OBJ_DIR)/vendor/fmt $(OBJ_DIR)/vendor/sha2 $(OBJ_DIR)/vendor/md5)
	@# Ensure all directories are writable (fixes Docker volume mount permissions)
	@chmod -R u+w $(OBJ_DIR) 2>/dev/null || true
	@([ "$$(id -u)" != "0" ] && chown -R $$(id -u):$$(id -g) $(OBJ_DIR) 2>/dev/null || true) || true

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

$(CONFIG_HEADER): configure make/config.h.tpl
	@echo "$(CYAN)Regenerating $(CONFIG_HEADER) via ${CONFIGURE_COMMAND}...$(NC)"
	@${CONFIGURE_COMMAND} >/dev/null

# Special rules for vendor C files (SHA2 and MD5)
# Use less strict warnings for vendor code to avoid noise from third-party code
$(OBJ_DIR)/vendor/sha2/sha2.o: $(VENDOR_DIR)/sha2/sha2.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@chmod -R u+w $(dir $@) $(OBJ_DIR) 2>/dev/null || true
	@([ "$$(id -u)" != "0" ] && chown -R $$(id -u):$$(id -g) $(dir $@) 2>/dev/null || true) || true
	$(CC) $(BASE_CFLAGS) $(OPT_FLAGS) $(VENDOR_C_WARNING_FLAGS) $(LTO_FLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/vendor/md5/md5.o: $(VENDOR_DIR)/md5/md5.c | $(OBJ_DIR)
	@mkdir -p $(dir $@)
	@chmod -R u+w $(dir $@) $(OBJ_DIR) 2>/dev/null || true
	@([ "$$(id -u)" != "0" ] && chown -R $$(id -u):$$(id -g) $(dir $@) 2>/dev/null || true) || true
	$(CC) $(BASE_CFLAGS) $(OPT_FLAGS) $(VENDOR_C_WARNING_FLAGS) $(LTO_FLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

.PHONY: all prepare rocksdb-check rocksdb-preflight binary-compat-check prune-disabled-extra-modules clean install uninstall debug create_ssl help build-info synonyms-sync synonyms-check package package-all package-deb package-rpm

prune-disabled-extra-modules:
	@mkdir -p $(RUN_DIR)/modules
	@for mod in $(EXTRA_MODULE_KNOWN_NAMES); do \
		case " $(EXTRA_MODULE_ENABLED_NAMES) " in \
			*" $$mod "*) ;; \
			*) rm -f "$(RUN_DIR)/modules/$$mod.so" 2>/dev/null || true ;; \
		esac; \
	done

# CLEANUP

clean:
	@echo "$(YELLOW)Cleaning build artifacts...$(NC)"
	@rm -rf $(OBJ_DIR) $(BIN_DIR) 2>/dev/null || true
	@rm -rf build/ backup/ collections/ 2>/dev/null || true
	@rm -rf run/bin/* 2>/dev/null || true
	@find $(SRC_DIR)/modules -mindepth 1 -maxdepth 1 -type l -delete 2>/dev/null || true
	@rm -f $(SRC_DIR)/modules/*.so 2>/dev/null || true
	@find $(SRC_DIR)/modules -mindepth 2 -maxdepth 2 -name '*.so' -delete 2>/dev/null || true
	@rm -rf $(RUN_DIR)/modules 2>/dev/null || true
	@rm -f $(RUN_DIR)/pid/*.pid $(RUN_DIR)/pid/*.lock 2>/dev/null || true
	@rm -rf $(RUN_DIR)/test/* 2>/dev/null || true
	@find $(RUN_DIR) -maxdepth 2 \( -name '*.core' -o -name 'core.*' -o -name '*.stackdump' \) -delete 2>/dev/null || true
	@rm -f include/core/config.h 2>/dev/null || true
	@echo "$(YELLOW)Cleaning vendor build artifacts...$(NC)"
	@# Fix permissions before removing to avoid permission denied errors
	@if [ -d "$(ROCKSDB_BUILD_DIR)" ]; then \
		chmod -R u+w "$(ROCKSDB_BUILD_DIR)" 2>/dev/null || true; \
		chown -R $$(id -u):$$(id -g) "$(ROCKSDB_BUILD_DIR)" 2>/dev/null || true; \
	fi
	@if [ -d "$(ROCKSDB_BUILD_DIR)" ]; then \
		if [ ! -w "$(ROCKSDB_BUILD_DIR)" ]; then \
			echo "$(YELLOW)Warning: $(ROCKSDB_BUILD_DIR) not writable; run 'sudo make clean' if removal fails$(NC)"; \
		fi; \
	fi
	@rm -rf $(ROCKSDB_BUILD_DIR) 2>/dev/null || true
	@rm -f $(ROCKSDB_LIB) 2>/dev/null || true
	@# Fix permissions on vendor build directories before removing
	@find $(VENDOR_DIR) -type d -name "build" -exec sh -c 'chmod -R u+w "{}" 2>/dev/null || true; chown -R $$(id -u):$$(id -g) "{}" 2>/dev/null || true' \; 2>/dev/null || true
	@find $(VENDOR_DIR) -type d -name "build" -exec rm -rf {} + 2>/dev/null || true
	@find $(VENDOR_DIR) -type d -name "cmake-build-*" -exec sh -c 'chmod -R u+w "{}" 2>/dev/null || true; chown -R $$(id -u):$$(id -g) "{}" 2>/dev/null || true' \; 2>/dev/null || true
	@find $(VENDOR_DIR) -type d -name "cmake-build-*" -exec rm -rf {} + 2>/dev/null || true
	@find $(VENDOR_DIR) -name "*.o" -exec chmod u+w {} \; -delete 2>/dev/null || true
	@find $(VENDOR_DIR) -name "*.d" -exec chmod u+w {} \; -delete 2>/dev/null || true
	@find $(VENDOR_DIR) -name "CMakeCache.txt" -exec chmod u+w {} \; -delete 2>/dev/null || true
	@find $(VENDOR_DIR) -name "CMakeFiles" -type d -exec sh -c 'chmod -R u+w "{}" 2>/dev/null || true; chown -R $$(id -u):$$(id -g) "{}" 2>/dev/null || true; rm -rf "{}"' \; 2>/dev/null || true
	@find $(VENDOR_DIR) -name "Makefile" -not -path "*/\.*" -exec chmod u+w {} \; -delete 2>/dev/null || true
	@rm -f ./Makefile 2>/dev/null || true

clean-all: clean
	@echo "$(YELLOW)Cleaning all generated files...$(NC)"
	@rm -rf *.gcda *.gcno *.gcov coverage/
	@rm -rf *.profraw *.profdata
	@find . -name "*.o" -delete
	@find . -name "*.d" -delete
	@rm -rf $(RUN_DIR)/logs/* 2>/dev/null || true
	@rm -rf $(RUN_DIR)/ssl/* 2>/dev/null || true
	@rm -rf $(RUN_DIR)/admin/* 2>/dev/null || true
	@echo "$(GREEN)[OK] All generated files cleaned$(NC)"

# INSTALLATION

PREFIX ?= ${PREFIX}
CONFDIR ?= ${CONFDIR}
LOGDIR ?= ${LOGDIR}
DATADIR ?= ${DATADIR}
RUNDIR ?= ${RUNDIR}
BINDIR ?= ${BINDIR}
SYSTEMD_UNIT_DIR ?= ${SYSTEMD_UNIT_DIR}
INSTALL ?= install
STAGED_RUN_DIR := $(if $(strip $(DESTDIR)),$(DESTDIR)/$(RUN_DIR),$(RUN_DIR))

install:
	@echo ""
	@echo "Installing binaries to $(STAGED_RUN_DIR)/bin/..."
	@if [ ! -f "$(BIN_DIR)/hlquery" ] || [ ! -f "$(BIN_DIR)/hlquery-cli" ]; then \
		echo "$(RED) Error: Binaries not found in $(BIN_DIR)/$(NC)"; \
		echo "$(YELLOW)   [TIP] Please run 'make' first to build the binaries.$(NC)"; \
			echo ""; \
			exit 1; \
		fi
	@$(INSTALL) -d "$(STAGED_RUN_DIR)/bin"
	@$(INSTALL) -d "$(STAGED_RUN_DIR)/modules"
	@$(INSTALL) -m 0755 $(BIN_DIR)/hlquery "$(STAGED_RUN_DIR)/bin/hlquery"
	@$(INSTALL) -m 0755 $(BIN_DIR)/hlquery-cli "$(STAGED_RUN_DIR)/bin/hlquery-cli"
	@if [ -f "$(BIN_DIR)/hlquery-benchmark" ]; then \
		$(INSTALL) -m 0755 $(BIN_DIR)/hlquery-benchmark "$(STAGED_RUN_DIR)/bin/hlquery-benchmark"; \
	fi
	@if [ -f "$(BIN_DIR)/hlquery-talk" ]; then \
		$(INSTALL) -m 0755 $(BIN_DIR)/hlquery-talk "$(STAGED_RUN_DIR)/bin/hlquery-talk"; \
	fi
	@if [ -n "$(MODULE_LIBS)" ]; then \
		if [ -z "$(DESTDIR)" ] && [ "$(STAGED_RUN_DIR)" = "$(RUN_DIR)" ]; then \
			echo "$(BLUE)   Modules already staged in $(RUN_DIR)/modules; skipping self-copy.$(NC)"; \
		else \
			$(INSTALL) -m 0755 $(MODULE_LIBS) "$(STAGED_RUN_DIR)/modules/"; \
		fi; \
	fi
	@if [ -f "run/hlquery" ]; then \
		echo "$(BLUE)   Wrapper: run/hlquery (already generated by configure)$(NC)"; \
	fi
	@echo "$(GREEN) Installation complete!$(NC)"
	@echo "$(BLUE)   Binaries: $(STAGED_RUN_DIR)/bin/hlquery, $(STAGED_RUN_DIR)/bin/hlquery-cli$(NC)"
	@if [ -f "$(STAGED_RUN_DIR)/bin/hlquery-talk" ]; then \
		echo "$(BLUE)   Talk:     $(STAGED_RUN_DIR)/bin/hlquery-talk$(NC)"; \
	fi
	@if [ -d "$(STAGED_RUN_DIR)/modules" ]; then \
		echo "$(BLUE)   Modules:  $(STAGED_RUN_DIR)/modules$(NC)"; \
	fi
	@if [ -f "$(STAGED_RUN_DIR)/bin/hlquery-benchmark" ]; then \
		echo "$(BLUE)   Benchmark: $(STAGED_RUN_DIR)/bin/hlquery-benchmark$(NC)"; \
	fi
	@echo ""
	@if [ -z "$(DESTDIR)" ]; then \
		if [ -f "$(RUN_DIR)/hlquery" ]; then \
			echo "$(CYAN)Quick help:$(NC)"; \
			$(RUN_DIR)/hlquery help 2>/dev/null | head -n 20 || true; \
		elif [ -f "$(RUN_DIR)/bin/hlquery-cli" ]; then \
			echo "$(CYAN)Quick help:$(NC)"; \
			$(RUN_DIR)/bin/hlquery-cli --help 2>/dev/null | head -n 20 || true; \
		fi; \
		echo "To start the server:"; \
		echo "  Run in foreground: ./run/hlquery start --nofork"; \
		echo "  Run as daemon:     ./run/hlquery start"; \
		echo ""; \
		echo "To run benchmarks, run:"; \
		echo "  ./run/bin/hlquery-benchmark"; \
		echo ""; \
		echo "To run the command line, run:"; \
		echo "  ./run/bin/hlquery-cli"; \
		echo "  ./run/bin/hlquery-talk"; \
		echo ""; \
	else \
		echo "$(CYAN)Staging tree ready at $(STAGED_RUN_DIR)$(NC)"; \
		echo ""; \
	fi

uninstall:
	@rm -f "$(RUN_DIR)/bin/hlquery"
	@rm -f "$(RUN_DIR)/bin/hlquery-cli"
	@rm -f "$(RUN_DIR)/bin/hlquery-benchmark"
	@rm -f "$(RUN_DIR)/bin/talk" "$(RUN_DIR)/bin/hlquery-talk"
	@rm -f "$(RUN_DIR)/modules/"*.so
	@rm -f "$(RUN_DIR)/hlquery"
	@echo "$(GREEN)" Uninstallation complete!$(NC)"

# DISTRIBUTED SYNONYM UTILITIES

synonyms-sync:
	@echo "$(CYAN)Syncing synonyms across cluster nodes...$(NC)"
	@./etc/scripts/synonyms_cluster_sync.sh \
		--mode sync \
		--source "$${SOURCE:-http://127.0.0.1:9200}" \
		--links "$${LINKS:-run/conf/links.conf}" \
		$${APIKEY:+--api-key "$$APIKEY"} \
		$${PRUNE:+--$$PRUNE}
	@echo "$(GREEN)✓ Synonym sync completed$(NC)"

synonyms-check:
	@echo "$(CYAN)Checking synonym parity across cluster nodes...$(NC)"
	@./etc/scripts/synonyms_cluster_sync.sh \
		--mode check \
		--source "$${SOURCE:-http://127.0.0.1:9200}" \
		--links "$${LINKS:-run/conf/links.conf}" \
		$${APIKEY:+--api-key "$$APIKEY"}

# BINARY TARGETS

# Separate hlquery server binary
# Use $(sort ...) to deduplicate object files (important for LTO to avoid duplicate symbols)
# Explicitly depend on ALL_OBJS and ROCKSDB_LIB to ensure proper dependency tracking
# This prevents the linker from starting before all object files are compiled AND RocksDB is built
# Note: ALL_OBJS includes REGULAR_OBJS, HTTP_OBJS, FMT_OBJ, SHA2_OBJ, MD5_OBJ
$(BIN_DIR)/hlquery: $(REGULAR_OBJS) $(HTTP_OBJS) $(FMT_OBJ) $(SHA2_OBJ) $(MD5_OBJ) | $(ROCKSDB_LIB)
	@mkdir -p $(BIN_DIR)
	@echo "$(CYAN)Linking hlquery...$(NC)"
	$(CXX) $(CXXFLAGS) \
		$(sort $(REGULAR_OBJS) $(HTTP_OBJS) $(FMT_OBJ) $(SHA2_OBJ) $(MD5_OBJ)) \
		-o $@ \
		$(LDFLAGS) $(RDYNAMIC)

# CLI support objects shared by the CLI and benchmark binaries
CLI_SUPPORT_OBJS := $(OBJ_DIR)/cli/cliutils.o \
                    $(OBJ_DIR)/utils/consolewriter.o \
                    $(OBJ_DIR)/cli/core.o

# CLI object files
CLI_OBJS := $(CLI_SUPPORT_OBJS) \
            $(OBJ_DIR)/cli/collections.o \
            $(OBJ_DIR)/cli/documents.o \
            $(OBJ_DIR)/cli/stats.o \
            $(OBJ_DIR)/cli/info.o \
            $(OBJ_DIR)/cli/modules.o \
            $(OBJ_DIR)/cli/synonyms.o \
            $(OBJ_DIR)/cli/stopwords.o \
            $(OBJ_DIR)/cli/keys.o \
            $(OBJ_DIR)/cli/main.o \
            $(OBJ_DIR)/runtime/exitmanager.o
BENCHMARK_OBJ := $(CLI_SUPPORT_OBJS) \
                 $(OBJ_DIR)/cli/benchmarkclient.o \
                 $(OBJ_DIR)/cli/benchmarktasks.o \
                 $(OBJ_DIR)/cli/benchmarksearch.o \
                 $(OBJ_DIR)/cli/benchmarkmodes.o \
                 $(OBJ_DIR)/cli/benchmarkreport.o \
                 $(OBJ_DIR)/cli/benchmarkdata.o \
                 $(OBJ_DIR)/cli/benchmarkmain.o \
                 $(OBJ_DIR)/cli/modules.o \
                 $(OBJ_DIR)/runtime/exitmanager.o
TALK_OBJS := $(CLI_SUPPORT_OBJS) \
             $(OBJ_DIR)/cli/collections.o \
             $(OBJ_DIR)/cli/documents.o \
             $(OBJ_DIR)/cli/modules.o \
             $(OBJ_DIR)/cli/stats.o \
             $(OBJ_DIR)/talk/entry.o \
             $(OBJ_DIR)/talk/linenoise.o \
             $(OBJ_DIR)/talk/main.o \
             $(OBJ_DIR)/runtime/exitmanager.o

$(ALL_OBJS) $(CLI_OBJS) $(BENCHMARK_OBJ) $(TALK_OBJS): | prepare

# CLI binary
# Note: CLI doesn't need RocksDB, but we ensure it waits for prepare target
$(BIN_DIR)/hlquery-cli: $(CLI_OBJS) | prepare
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) \
		$(CLI_OBJS) \
		-o $@ \
		$(LDFLAGS)

# Benchmark binary
# Note: Benchmark doesn't need RocksDB, but we ensure it waits for prepare target
$(BIN_DIR)/hlquery-benchmark: $(BENCHMARK_OBJ) | prepare
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) \
		$(BENCHMARK_OBJ) \
		-o $@ \
		$(LDFLAGS)

$(BIN_DIR)/hlquery-talk: $(TALK_OBJS) | prepare
	@mkdir -p $(BIN_DIR) $(RUN_DIR)/bin
	$(CXX) $(CXXFLAGS) \
		$(TALK_OBJS) \
		-o $@ \
		$(LDFLAGS)
	@$(INSTALL) -m 0755 $@ "$(RUN_DIR)/bin/hlquery-talk"

$(REGULAR_OBJS) $(HTTP_OBJS) $(CLI_OBJS) $(BENCHMARK_OBJ) $(TALK_OBJS) $(MODULE_OBJS): $(CONFIG_HEADER)

# Main build target
all: prepare $(BIN_DIR)/hlquery $(BIN_DIR)/hlquery-cli $(BIN_DIR)/hlquery-benchmark $(BIN_DIR)/hlquery-talk $(MODULE_LIBS)
	@echo ""
	@echo "$(GREEN)  Build complete!$(NC)"
	@echo "$(BLUE)   Server: build/bin/hlquery$(NC)"
	@echo "$(BLUE)   CLI:    build/bin/hlquery-cli$(NC)"
	@echo "$(BLUE)   Talk:   build/bin/hlquery-talk$(NC)"
	@echo "$(YELLOW)   Run 'make install' to install to run/bin/$(NC)"
	@echo ""
	@echo "$(NC)$(BOLD)Done!$(NC)"
	@echo ""

# UTILITY TARGETS

help:
	@echo "$(BOLD)HLQuery Build System$(NC)"
	@echo ""
	@echo "$(BOLD)Build Modes:$(NC)"
	@echo "  make BUILD_MODE=release   - Optimized production build (default)"
	@echo "  make BUILD_MODE=debug     - Debug build with symbols"
	@echo "  make BUILD_MODE=profile   - Profile build for performance analysis"
	@echo "  make BUILD_MODE=sanitize - Build with AddressSanitizer, UBSan, ThreadSanitizer"
	@echo "  make BUILD_MODE=coverage  - Build with code coverage support"
	@echo ""
	@echo "$(BOLD)Feature Flags:$(NC)"
	@echo "  make WITH_JEMALLOC=1     - Use jemalloc memory allocator"
	@echo "  make WITH_TCMALLOC=1     - Use tcmalloc memory allocator"
	@echo "  make USE_PGO=1           - Enable Profile-Guided Optimization (generation)"
	@echo "  make USE_PGO=2           - Use PGO profile data for optimization"
	@echo "  make USE_CLANG_TIDY=1    - Enable clang-tidy static analysis"
	@echo ""
	@echo "$(BOLD)Common Targets:$(NC)"
	@echo "  make                      - Build everything (release mode)"
	@echo "  make clean                 - Remove build artifacts"
	@echo "  make clean-all            - Remove all generated files"
	@echo "  make install              - Install binaries to run/bin/"
	@echo "  make package              - Build Debian/RPM packages via etc/package/build.sh"
	@echo "  make uninstall          - Remove installed binaries"
	@echo "  make build-info            - Show build configuration"
	@echo "  make debug                - Show build variables"
	@echo "  make synonyms-sync        - Sync synonyms from SOURCE to all links.conf peers"
	@echo "  make synonyms-check       - Check synonym parity across links.conf peers"
	@echo ""
	@echo "$(BOLD)Synonym Variables:$(NC)"
	@echo "  SOURCE=http://host:port   - Source node (default: http://127.0.0.1:9200)"
	@echo "  LINKS=run/conf/links.conf - Peer list file (default shown)"
	@echo "  APIKEY=...                - Optional X-API-Key for protected APIs"
	@echo "  PRUNE=no-prune            - Keep extra target synonyms during sync"
	@echo ""

build-info:
	@echo "$(BOLD)Build Configuration:$(NC)"
	@echo "  Build Mode:     $(BUILD_MODE)"
	@echo "  Compiler:       $(CXX)"
	@echo "  CXXFLAGS:       $(CXXFLAGS)"
	@echo "  LDFLAGS:        $(LDFLAGS)"
	@echo "  Parallel Jobs:  $(shell nproc 2>/dev/null || echo 4)"
	@echo "  jemalloc:       $(if $(filter 1,$(WITH_JEMALLOC)),enabled,disabled)"
	@echo "  tcmalloc:       $(if $(filter 1,$(WITH_TCMALLOC)),enabled,disabled)"
	@echo "  PGO:            $(if $(USE_PGO),enabled ($(USE_PGO)),disabled)"
	@echo "  SSL Support:    $(if $(filter 1,0),full (certs found),$(if $(filter 1,1),capabilities only (libraries linked),disabled))"
	@echo ""

debug:
	@echo "$(BOLD)Build Variables:$(NC)"
	@echo "  SRCS_TOP: $(words $(SRCS_TOP)) source files"
	@echo "  OBJS: $(words $(OBJS)) object files"
	@echo "  BUILD_MODE: $(BUILD_MODE)"
	@echo "  SSL_ENABLED: 0 (Certificates found)"
	@echo "  OPENSSL_ENABLED: 1 (Libraries linked)"

debug-verbose:
	@echo "$(BOLD)Source Files:$(NC)"
	@$(foreach src,$(SRCS_TOP),echo "  $(src)";)
	@echo "$(BOLD)Object Files:$(NC)"
	@$(foreach obj,$(OBJS),echo "  $(obj)";)

# SSL certificate generation
create_ssl:
	@echo ""
	@echo "  Creating SSL Certificate for HLQuery"
	@echo ""
	@bash make/create_ssl.sh
	@echo ""
	@echo " SSL certificate created successfully!"
	@echo ""
	@echo "Next steps:"
	@echo "  1. Run ./configure to detect SSL and enable support"
	@echo "  2. Run make clean && make to rebuild with SSL enabled"
	@echo ""

# Optional system-wide install (requires privileges)
install-system: all
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 0755 $(BIN_DIR)/hlquery "$(DESTDIR)$(BINDIR)/hlquery"
	$(INSTALL) -m 0755 $(BIN_DIR)/hlquery-cli "$(DESTDIR)$(BINDIR)/hlquery-cli"
	@if [ -f "$(BIN_DIR)/hlquery-benchmark" ]; then \
		$(INSTALL) -m 0755 $(BIN_DIR)/hlquery-benchmark "$(DESTDIR)$(BINDIR)/hlquery-benchmark"; \
	fi
	@if [ -f "$(BIN_DIR)/hlquery-talk" ]; then \
		$(INSTALL) -m 0755 $(BIN_DIR)/hlquery-talk "$(DESTDIR)$(BINDIR)/hlquery-talk"; \
	fi
	@if [ -n "$(MODULE_LIBS)" ]; then \
		$(INSTALL) -d "$(DESTDIR)$(PREFIX)/lib/hlquery/modules"; \
		$(INSTALL) -m 0755 $(MODULE_LIBS) "$(DESTDIR)$(PREFIX)/lib/hlquery/modules/"; \
	fi
	@if [ -d "run/conf" ]; then \
		$(INSTALL) -d "$(DESTDIR)$(CONFDIR)"; \
		cp -r run/conf/* "$(DESTDIR)$(CONFDIR)/"; \
	fi
	@if [ -n "$(SYSTEMD_UNIT_DIR)" ] && [ -f "etc/package-builder/hlquery.service" ]; then \
		$(INSTALL) -d "$(DESTDIR)$(SYSTEMD_UNIT_DIR)"; \
		$(INSTALL) -m 0644 "etc/package-builder/hlquery.service" "$(DESTDIR)$(SYSTEMD_UNIT_DIR)/hlquery.service"; \
	fi
	@$(INSTALL) -d "$(DESTDIR)$(LOGDIR)"
	@$(INSTALL) -d "$(DESTDIR)$(DATADIR)"
	@$(INSTALL) -d "$(DESTDIR)$(RUNDIR)"
	@if [ -f "run/hlquery" ]; then \
		$(INSTALL) -m 0755 run/hlquery "$(DESTDIR)$(BINDIR)/hlquery-wrapper"; \
	fi
	@echo "$(GREEN)✓ System-wide installation complete!$(NC)"

# Package builder helpers
PACKAGE_DIR ?= etc/package
PACKAGE_SCRIPT ?= $(PACKAGE_DIR)/build.sh
PACKAGE_TYPE ?= all
PACKAGE_VERSION ?= $(shell sh src/version.sh | sed 's|^hlquery-||')
PACKAGE_RELEASE ?= 1
PACKAGE_ARCH ?= $(shell uname -m)
PACKAGE_EXTRA_ARGS ?=

.PHONY: package package-all package-deb package-rpm

package: package-$(PACKAGE_TYPE)

package-all: package-deb package-rpm

package-deb:
	@echo "$(CYAN)Building Debian package...$(NC)"
	@if [ ! -x "$(PACKAGE_SCRIPT)" ]; then \
		echo "$(RED) Package builder missing at $(PACKAGE_SCRIPT)$(NC)"; \
		exit 1; \
	fi
	@$(PACKAGE_SCRIPT) --type deb --version $(PACKAGE_VERSION) --release $(PACKAGE_RELEASE) --arch $(PACKAGE_ARCH) $(PACKAGE_EXTRA_ARGS)

package-rpm:
	@echo "$(CYAN)Building RPM package...$(NC)"
	@if [ ! -x "$(PACKAGE_SCRIPT)" ]; then \
		echo "$(RED) Package builder missing at $(PACKAGE_SCRIPT)$(NC)"; \
		exit 1; \
	fi
	@$(PACKAGE_SCRIPT) --type rpm --version $(PACKAGE_VERSION) --release $(PACKAGE_RELEASE) --arch $(PACKAGE_ARCH) $(PACKAGE_EXTRA_ARGS)
