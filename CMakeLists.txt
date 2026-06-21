cmake_minimum_required(VERSION 3.20)
project(lyghtd LANGUAGES CXX)

# ============================================================================
# lyghtd — C++ drop-in port of lightwalletd (Ycash).
#
# Milestone 0 (this file's job): prove the toolchain. CMake + FetchContent
# build a STATIC, self-contained gRPC + protobuf, generate C++ stubs from our
# vendored .proto files, and link a trivial CompactTxStreamer server that
# starts up and binds a port. No cache, no daemon, no real RPCs yet.
# ============================================================================

# C++23: std::shared_mutex is exactly the cache concurrency model we need
# (shared = concurrent readers, unique = ingest append). C++23 is required by
# glaze (the JSON lib used for the ycashd RPC client); GCC 14 supports it fully.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Default to Release; this is a perf-sensitive serve path.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Static / self-contained binary, matching the Go lightwalletd distribution.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build everything static" FORCE)

# ============================================================================
# VERSION (single source of truth at repo root, like the inzyght template)
# ============================================================================
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LYGHTD_VERSION LIMIT_COUNT 1)
string(STRIP "${LYGHTD_VERSION}" LYGHTD_VERSION)
if(NOT LYGHTD_VERSION)
    message(FATAL_ERROR "VERSION file is empty or missing")
endif()
message(STATUS "lyghtd version: ${LYGHTD_VERSION}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h"
    @ONLY
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

# ============================================================================
# FETCHCONTENT — gRPC (static). One pin vendors a matching protobuf, so the
# generated code and the runtime always agree on wire format.
# ============================================================================
include(FetchContent)

# Re-fetch when a GIT_TAG changes here, instead of silently keeping the old
# clone (would otherwise require rm -rf build/_deps/<dep>-* to pick up a bump).
set(FETCHCONTENT_UPDATES_DISCONNECTED OFF)

# gRPC's own knobs: build only what we need, link static, and reuse the
# system zlib/OpenSSL/c-ares/re2 if present is NOT what we want here — we
# vendor everything so the binary is a true drop-in. gRPC bundles them.
set(gRPC_BUILD_TESTS            OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_CSHARP_EXT       OFF CACHE BOOL "" FORCE)
set(gRPC_INSTALL                OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_CPP_PLUGIN  ON  CACHE BOOL "" FORCE)  # need cpp + grpc plugins
set(gRPC_BUILD_GRPC_CSHARP_PLUGIN     OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_NODE_PLUGIN       OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_PHP_PLUGIN        OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_PYTHON_PLUGIN     OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_RUBY_PLUGIN       OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL         ON  CACHE BOOL "" FORCE)  # quiets abseil install warnings
set(ABSL_PROPAGATE_CXX_STD      ON  CACHE BOOL "" FORCE)
set(protobuf_INSTALL            OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
set(RE2_BUILD_TESTING           OFF CACHE BOOL "" FORCE)
set(utf8_range_ENABLE_INSTALL   OFF CACHE BOOL "" FORCE)

# Latest stable gRPC at scaffold time (2026-06). Bumping this one tag bumps
# protobuf too, since gRPC pins a matching protobuf submodule.
FetchContent_Declare(grpc
    GIT_REPOSITORY https://github.com/grpc/grpc.git
    GIT_TAG v1.71.0
    GIT_SHALLOW TRUE
)
message(STATUS "Fetching gRPC v1.71.0 (static, with bundled protobuf)...")
FetchContent_MakeAvailable(grpc)
message(STATUS "gRPC fetched and configured")

# ============================================================================
# FETCHCONTENT — libcurl (static, no TLS). The ycashd JSON-RPC is plaintext
# HTTP on localhost, so we build curl WITHOUT SSL/PSL to keep the binary lean
# and dependency-free (no OpenSSL needed). Used only by the M2 ingestor.
# ============================================================================
set(BUILD_CURL_EXE     OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_TESTS ON  CACHE BOOL "" FORCE)
set(CURL_ENABLE_SSL    OFF CACHE BOOL "" FORCE)   # localhost plaintext RPC only
set(CURL_USE_LIBPSL    OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2   OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB          OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI        OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD          OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2        OFF CACHE BOOL "" FORCE)
set(CURL_STATICLIB     ON  CACHE BOOL "" FORCE)
FetchContent_Declare(curl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG curl-8_18_0
    GIT_SHALLOW TRUE
)
message(STATUS "Fetching libcurl 8.18.0 (static, no TLS)...")
FetchContent_MakeAvailable(curl)

# ============================================================================
# FETCHCONTENT — glaze (header-only JSON; requires C++23). Used to (de)serialize
# ycashd JSON-RPC envelopes in the M2 ingestor.
# ============================================================================
FetchContent_Declare(glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG v7.7.0
    GIT_SHALLOW TRUE
)
message(STATUS "Fetching glaze v7.7.0 (header-only)...")
FetchContent_MakeAvailable(glaze)

# Aliases for the codegen plugins, regardless of whether gRPC exported them
# as targets or we have to point at the built executables.
set(_PROTOBUF_PROTOC      $<TARGET_FILE:protoc>)
set(_GRPC_CPP_PLUGIN_EXE  $<TARGET_FILE:grpc_cpp_plugin>)

# ============================================================================
# PROTO CODEGEN — generate C++ message + gRPC service stubs from ./proto/.
# lyghtd owns its proto copies (handoff requirement); they are the fixed
# wallet-client contract and must match the Go server byte-for-byte.
# ============================================================================
set(PROTO_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_GEN  "${CMAKE_CURRENT_BINARY_DIR}/generated/proto")
file(MAKE_DIRECTORY "${PROTO_GEN}")

set(PROTO_FILES
    "${PROTO_DIR}/compact_formats.proto"
    "${PROTO_DIR}/service.proto"
)

set(PROTO_SRCS)
set(PROTO_HDRS)
foreach(_proto ${PROTO_FILES})
    get_filename_component(_name "${_proto}" NAME_WE)
    set(_pb_cc   "${PROTO_GEN}/${_name}.pb.cc")
    set(_pb_h    "${PROTO_GEN}/${_name}.pb.h")
    set(_grpc_cc "${PROTO_GEN}/${_name}.grpc.pb.cc")
    set(_grpc_h  "${PROTO_GEN}/${_name}.grpc.pb.h")
    add_custom_command(
        OUTPUT  "${_pb_cc}" "${_pb_h}" "${_grpc_cc}" "${_grpc_h}"
        COMMAND ${_PROTOBUF_PROTOC}
        ARGS    --cpp_out  "${PROTO_GEN}"
                --grpc_out "${PROTO_GEN}"
                --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN_EXE}
                -I "${PROTO_DIR}"
                "${_proto}"
        DEPENDS "${_proto}" ${_PROTOBUF_PROTOC} ${_GRPC_CPP_PLUGIN_EXE}
        COMMENT "Generating C++/gRPC stubs for ${_name}.proto"
        VERBATIM
    )
    list(APPEND PROTO_SRCS "${_pb_cc}" "${_grpc_cc}")
    list(APPEND PROTO_HDRS "${_pb_h}"  "${_grpc_h}")
endforeach()

# Bundle generated stubs into one library the server links against.
add_library(lyghtd_proto STATIC ${PROTO_SRCS})
target_include_directories(lyghtd_proto PUBLIC "${PROTO_GEN}")
target_link_libraries(lyghtd_proto PUBLIC
    grpc++
    grpc++_reflection
    libprotobuf
)

# ============================================================================
# CORE LIBRARY — all of lyghtd's logic (cache reader/writer, gRPC service,
# ycashd RPC client, block parser, ingestor). The thin main.cc links it; dev
# validation tools (out of tree) can link it too.
# ============================================================================
add_library(lyghtd_core STATIC
    src/block_cache.cc
    src/compact_tx_streamer.cc
    src/rpc_client.cc
    src/block_parser.cc
    src/ingestor.cc
    src/sha256.cc
)
target_include_directories(lyghtd_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"   # Version.h
)
target_link_libraries(lyghtd_core PUBLIC
    lyghtd_proto
    grpc++
    grpc++_reflection
    CURL::libcurl
    glaze::glaze
)

# ============================================================================
# SERVER EXECUTABLE
# ============================================================================
add_executable(lyghtd src/main.cc src/tls_creds.cc)
# BoringSSL (vendored by gRPC) for in-memory self-signed cert generation.
target_include_directories(lyghtd PRIVATE
    "${grpc_SOURCE_DIR}/third_party/boringssl-with-bazel/src/include")
target_link_libraries(lyghtd PRIVATE lyghtd_core ssl crypto)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(lyghtd PRIVATE -Wall -Wextra -g -O0)
else()
    target_compile_options(lyghtd PRIVATE -Wall -Wextra -O3)
endif()

install(TARGETS lyghtd DESTINATION bin)

# ============================================================================
# DEV / REGRESSION-TESTING TOOLS (off by default)
# ============================================================================
# Validation + benchmark harnesses under tools/. Not built in a normal build;
# enable with -DLYGHTD_BUILD_TOOLS=ON. They link lyghtd_core, so they exercise
# exactly the same compiled code as the server. See tools/README.md.
option(LYGHTD_BUILD_TOOLS "Build dev/regression-testing tools in tools/" OFF)
if(LYGHTD_BUILD_TOOLS)
    foreach(_tool rpc_smoke parse_validate cache_validate bench_client grpc_diff ingest_profile taddr_bench)
        add_executable(${_tool} tools/${_tool}.cc)
        target_link_libraries(${_tool} PRIVATE lyghtd_core)
    endforeach()
    message(STATUS "lyghtd: building dev tools (rpc_smoke parse_validate "
                   "cache_validate bench_client grpc_diff)")
endif()
