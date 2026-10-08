cmake_minimum_required(VERSION 3.20)

option(BUILD_TESTING "Build test targets" OFF)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(WIN32)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
  elseif(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
  endif()
endif()

if(NOT DEFINED VCPKG_TARGET_TRIPLET)
  if(WIN32)
    set(VCPKG_TARGET_TRIPLET "x64-windows"
        CACHE STRING "Vcpkg target triplet")
  elseif(APPLE AND DEFINED ENV{VCPKG_ROOT})
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
      set(VCPKG_TARGET_TRIPLET "arm64-osx"
          CACHE STRING "Vcpkg target triplet")
    elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64)$")
      set(VCPKG_TARGET_TRIPLET "x64-osx"
          CACHE STRING "Vcpkg target triplet")
    else()
      message(FATAL_ERROR
        "Unsupported macOS architecture: ${CMAKE_HOST_SYSTEM_PROCESSOR}. "
        "Set VCPKG_TARGET_TRIPLET explicitly.")
    endif()
  elseif(DEFINED ENV{VCPKG_ROOT})
    set(VCPKG_TARGET_TRIPLET "x64-linux"
        CACHE STRING "Vcpkg target triplet")
  endif()
endif()

project(raft_grpc_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(raft_collect_existing_sources out_var)
  set(collected_sources)
  foreach(source_file IN LISTS ARGN)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
      list(APPEND collected_sources "${source_file}")
    endif()
  endforeach()
  set(${out_var} "${collected_sources}" PARENT_SCOPE)
endfunction()

function(raft_add_optional_app_target target_name source_file)
  set(optional_private_libs ${ARGN})
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
    message(STATUS "Skip planned target ${target_name}: ${source_file} not found yet")
    return()
  endif()

  add_executable(${target_name} ${source_file})
  target_link_libraries(${target_name}
    PRIVATE
      raft_core
      ${optional_private_libs}
  )
endfunction()

if(MSVC)
  add_compile_options(/utf-8)
endif()

if(WIN32)
  add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
  )
endif()

# 让 VS Code / clangd 能拿到真实编译参数
# 注意：这个对 Makefile / Ninja 生效，Visual Studio 生成器会忽略
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =========================
# Build parallelism control
# =========================
# Ninja 默认会使用较高并发，gRPC / protobuf / absl 编译时容易占满 CPU 和内存。
# 这里通过 CMake job pool 限制 Ninja 的编译、链接和 proto 生成并发。
#
# 可在重新配置时覆盖：
#   cmake -S . -B build -DRAFT_BUILD_JOBS=2 -DRAFT_LINK_JOBS=1 -DRAFT_PROTO_JOBS=1
#
# 更稳但更慢：
#   -DRAFT_BUILD_JOBS=1 -DRAFT_LINK_JOBS=1 -DRAFT_PROTO_JOBS=1
set(RAFT_BUILD_JOBS 2 CACHE STRING "Max concurrent compile jobs for Ninja")
set(RAFT_LINK_JOBS 1 CACHE STRING "Max concurrent link jobs for Ninja")
set(RAFT_PROTO_JOBS 1 CACHE STRING "Max concurrent protobuf/gRPC codegen jobs for Ninja")

if(CMAKE_GENERATOR MATCHES "Ninja")
  set_property(GLOBAL PROPERTY JOB_POOLS
    raft_compile_pool=${RAFT_BUILD_JOBS}
    raft_link_pool=${RAFT_LINK_JOBS}
    raft_proto_pool=${RAFT_PROTO_JOBS}
  )

  set(CMAKE_JOB_POOL_COMPILE raft_compile_pool)
  set(CMAKE_JOB_POOL_LINK raft_link_pool)

  set(RAFT_PROTO_JOB_POOL_ARGS JOB_POOL raft_proto_pool)
else()
  set(RAFT_PROTO_JOB_POOL_ARGS)
endif()

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(Threads REQUIRED)

message(STATUS "Using protobuf ${Protobuf_VERSION}")
message(STATUS "Using gRPC ${gRPC_VERSION}")

set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(STORAGE_NODE_PROTO_RELATIVE proto/storage_node.proto)
set(STORAGE_NODE_PROTO_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/${STORAGE_NODE_PROTO_RELATIVE})
set(VIEW_PROTO_RELATIVE proto/view.proto)
set(VIEW_PROTO_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/${VIEW_PROTO_RELATIVE})
set(PROTO_FILES
  ${PROTO_DIR}/common.proto
  ${PROTO_DIR}/raft.proto
  ${PROTO_DIR}/metadata.proto
  ${VIEW_PROTO_SCHEMA}
  ${STORAGE_NODE_PROTO_SCHEMA}
)
set(GRPC_PROTO_FILES
  ${PROTO_DIR}/raft.proto
  ${PROTO_DIR}/metadata.proto
  ${VIEW_PROTO_SCHEMA}
  ${STORAGE_NODE_PROTO_SCHEMA}
)
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(PROJECT_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/modules)

file(MAKE_DIRECTORY ${GENERATED_DIR})

if(CMAKE_CROSSCOMPILING)
  find_program(_PROTOBUF_PROTOC protoc REQUIRED)
  find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin REQUIRED)
else()
  set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
  set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
endif()

set(PROTO_SRCS
  ${GENERATED_DIR}/common.pb.cc
  ${GENERATED_DIR}/raft.pb.cc
  ${GENERATED_DIR}/metadata.pb.cc
  ${GENERATED_DIR}/view.pb.cc
  ${GENERATED_DIR}/storage_node.pb.cc
)
set(PROTO_HDRS
  ${GENERATED_DIR}/common.pb.h
  ${GENERATED_DIR}/raft.pb.h
  ${GENERATED_DIR}/metadata.pb.h
  ${GENERATED_DIR}/view.pb.h
  ${GENERATED_DIR}/storage_node.pb.h
)
set(GRPC_SRCS
  ${GENERATED_DIR}/raft.grpc.pb.cc
  ${GENERATED_DIR}/metadata.grpc.pb.cc
  ${GENERATED_DIR}/view.grpc.pb.cc
  ${GENERATED_DIR}/storage_node.grpc.pb.cc
)
set(GRPC_HDRS
  ${GENERATED_DIR}/raft.grpc.pb.h
  ${GENERATED_DIR}/metadata.grpc.pb.h
  ${GENERATED_DIR}/view.grpc.pb.h
  ${GENERATED_DIR}/storage_node.grpc.pb.h
)

add_custom_command(
  OUTPUT
    ${PROTO_SRCS}
    ${PROTO_HDRS}
  COMMAND ${_PROTOBUF_PROTOC}
  ARGS
    --proto_path=${PROTO_DIR}
    --cpp_out=${GENERATED_DIR}
    ${PROTO_FILES}
  DEPENDS ${PROTO_FILES}
  ${RAFT_PROTO_JOB_POOL_ARGS}
  VERBATIM
)

add_custom_command(
  OUTPUT
    ${GRPC_SRCS}
    ${GRPC_HDRS}
  COMMAND ${_PROTOBUF_PROTOC}
  ARGS
    --proto_path=${PROTO_DIR}
    --grpc_out=${GENERATED_DIR}
    --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN_EXECUTABLE}
    ${GRPC_PROTO_FILES}
  DEPENDS ${PROTO_FILES}
  ${RAFT_PROTO_JOB_POOL_ARGS}
  VERBATIM
)

add_library(common_proto
  ${GENERATED_DIR}/common.pb.cc
  ${GENERATED_DIR}/common.pb.h
)

target_include_directories(common_proto
  PUBLIC
    ${GENERATED_DIR}
)

target_link_libraries(common_proto
  PUBLIC
    protobuf::libprotobuf
)

add_library(raft_proto
  ${GENERATED_DIR}/raft.pb.cc
  ${GENERATED_DIR}/raft.pb.h
  ${GENERATED_DIR}/raft.grpc.pb.cc
  ${GENERATED_DIR}/raft.grpc.pb.h
)

target_include_directories(raft_proto
  PUBLIC
    ${GENERATED_DIR}
)

target_link_libraries(raft_proto
  PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)

add_library(metadata_proto
  ${GENERATED_DIR}/metadata.pb.cc
  ${GENERATED_DIR}/metadata.pb.h
  ${GENERATED_DIR}/metadata.grpc.pb.cc
  ${GENERATED_DIR}/metadata.grpc.pb.h
)

target_include_directories(metadata_proto
  PUBLIC
    ${GENERATED_DIR}
)

target_link_libraries(metadata_proto
  PUBLIC
    common_proto
    protobuf::libprotobuf
    gRPC::grpc++
)

add_library(view_proto
  ${GENERATED_DIR}/view.pb.cc
  ${GENERATED_DIR}/view.pb.h
  ${GENERATED_DIR}/view.grpc.pb.cc
  ${GENERATED_DIR}/view.grpc.pb.h
)

target_include_directories(view_proto
  PUBLIC
    ${GENERATED_DIR}
)

target_link_libraries(view_proto
  PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)

add_library(storage_node_proto
  ${GENERATED_DIR}/storage_node.pb.cc
  ${GENERATED_DIR}/storage_node.pb.h
  ${GENERATED_DIR}/storage_node.grpc.pb.cc
  ${GENERATED_DIR}/storage_node.grpc.pb.h
)

target_include_directories(storage_node_proto
  PUBLIC
    ${GENERATED_DIR}
)

target_link_libraries(storage_node_proto
  PUBLIC
    protobuf::libprotobuf
    gRPC::grpc++
)

if(WIN32)
  target_link_libraries(common_proto PUBLIC Ws2_32)
  target_link_libraries(raft_proto PUBLIC Ws2_32)
  target_link_libraries(metadata_proto PUBLIC Ws2_32)
  target_link_libraries(view_proto PUBLIC Ws2_32)
  target_link_libraries(storage_node_proto PUBLIC Ws2_32)
endif()

add_library(raft_core
  modules/store/common/store_types.cpp
  modules/store/chunk/chunk_store.cpp
  modules/store/chunk/local_disk_chunk_store.cpp
  modules/store/io/durable_file.cpp
  modules/store/index/chunk_index.cpp
  modules/store/maintenance/garbage_collector.cpp
  modules/store/maintenance/gc_task_store.cpp
  modules/store/maintenance/rebalance_manager.cpp
  modules/store/maintenance/repair_manager.cpp
  modules/store/maintenance/scrub_manager.cpp
  modules/store/node/storage_node_client.cpp
  modules/store/node/storage_node_registry.cpp
  modules/store/node/storage_node_service.cpp
  modules/store/placement/placement_manager.cpp
  modules/store/placement/replica_policy.cpp
  modules/store/runtime/storage_executor.cpp
  modules/store/upload/upload_coordinator.cpp
  modules/raft/runtime/min_heap_timer.cpp
  modules/raft/runtime/thread_pool.cpp
  modules/raft/common/command.cpp
  modules/raft/common/metadata_command.cpp
  modules/raft/node/raft_node.cpp
  modules/raft/service/raft_service_impl.cpp
  modules/raft/service/metadata_service_impl.cpp
  modules/raft/state_machine/metadata_state_machine.cpp
  modules/raft/storage/raft_storage.cpp
  modules/raft/storage/snapshot_storage.cpp
  modules/raft/replication/replicator.cpp
)

# 008 规划模块占位：
# 仅在未来源文件实际落地时，才把 cluster / view / store/transfer 接入 `raft_core`。
# 这样当前不会因为规划路径尚未实现而导致 configure / build 失败。
set(RAFT_008_PLANNED_CORE_SOURCES
  modules/cluster/cluster_config.cpp
  modules/cluster/node_identity.cpp
  modules/view/view_registry.cpp
  modules/view/view_service_impl.cpp
  modules/view/view_client.cpp
  modules/store/transfer/object_transfer.cpp
  modules/store/transfer/metadata_transfer_client.cpp
  modules/store/transfer/storage_transfer_client.cpp
)

raft_collect_existing_sources(RAFT_008_EXISTING_CORE_SOURCES
  ${RAFT_008_PLANNED_CORE_SOURCES}
)

if(RAFT_008_EXISTING_CORE_SOURCES)
  target_sources(raft_core
    PRIVATE
      ${RAFT_008_EXISTING_CORE_SOURCES}
  )
else()
  message(STATUS "008 planned raft_core sources are not present yet; keep placeholder wiring only")
endif()

target_include_directories(raft_core
  PUBLIC
    ${PROJECT_INCLUDE_DIR}
  PRIVATE
    ${GENERATED_DIR}
)

target_link_libraries(raft_core
  PUBLIC
    raft_proto
    storage_node_proto
  PRIVATE
    metadata_proto
    Threads::Threads
)

add_executable(raft_demo
  apps/main.cpp
)

target_link_libraries(raft_demo
  PRIVATE
    raft_core
)

add_executable(raft_metadata_client
  apps/raft_metadata_client.cpp
)

target_link_libraries(raft_metadata_client
  PRIVATE
    metadata_proto
)

# 008 规划 app target 占位：
# 仅当对应入口文件存在时才创建 target，避免当前阶段因占位路径缺失而破坏构建。
raft_add_optional_app_target(view_node_app
  apps/view_node_app.cpp
  view_proto
)

raft_add_optional_app_target(metadata_node_app
  apps/metadata_node_app.cpp
  view_proto
  metadata_proto
)

raft_add_optional_app_target(storage_node_app
  apps/storage_node_app.cpp
  view_proto
  storage_node_proto
)

raft_add_optional_app_target(storage_client
  apps/storage_client.cpp
  view_proto
  metadata_proto
  storage_node_proto
)

raft_add_optional_app_target(storage_bench
  apps/storage_bench.cpp
)

if(BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()
