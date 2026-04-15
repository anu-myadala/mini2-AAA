cmake_minimum_required(VERSION 3.16)
project(mini2 CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

# ------------------------------------------------------------
# gRPC + protobuf discovery.
# We try the cmake config packages first (works with vcpkg, brew, source builds).
# Fallback uses pkg-config which works on Debian/Ubuntu.
# ------------------------------------------------------------

find_package(Protobuf CONFIG)
find_package(gRPC     CONFIG)

if(NOT Protobuf_FOUND OR NOT gRPC_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PROTOBUF REQUIRED protobuf)
    pkg_check_modules(GRPC     REQUIRED grpc++)
endif()

# Locate protoc + grpc plugin.
if(TARGET protobuf::protoc)
    set(_PROTOC $<TARGET_FILE:protobuf::protoc>)
else()
    find_program(_PROTOC protoc REQUIRED)
endif()
if(TARGET gRPC::grpc_cpp_plugin)
    set(_GRPC_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
else()
    find_program(_GRPC_PLUGIN grpc_cpp_plugin REQUIRED)
endif()

# ------------------------------------------------------------
# Generate proto stubs into build/_gen.
# ------------------------------------------------------------
set(GEN_DIR "${CMAKE_BINARY_DIR}/_gen")
file(MAKE_DIRECTORY "${GEN_DIR}")

set(PROTO_SRC "${CMAKE_SOURCE_DIR}/../proto/overlay.proto")
get_filename_component(PROTO_DIR "${PROTO_SRC}" DIRECTORY)

set(PROTO_PB_CC   "${GEN_DIR}/overlay.pb.cc")
set(PROTO_PB_H    "${GEN_DIR}/overlay.pb.h")
set(PROTO_GRPC_CC "${GEN_DIR}/overlay.grpc.pb.cc")
set(PROTO_GRPC_H  "${GEN_DIR}/overlay.grpc.pb.h")

add_custom_command(
    OUTPUT  "${PROTO_PB_CC}" "${PROTO_PB_H}" "${PROTO_GRPC_CC}" "${PROTO_GRPC_H}"
    COMMAND ${_PROTOC}
            --proto_path=${PROTO_DIR}
            --cpp_out=${GEN_DIR}
            --grpc_out=${GEN_DIR}
            --plugin=protoc-gen-grpc=${_GRPC_PLUGIN}
            ${PROTO_SRC}
    DEPENDS ${PROTO_SRC}
    VERBATIM
)

add_library(mini2_proto STATIC
    ${PROTO_PB_CC}
    ${PROTO_GRPC_CC}
)
target_include_directories(mini2_proto PUBLIC "${GEN_DIR}")
if(TARGET gRPC::grpc++)
    target_link_libraries(mini2_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)
else()
    target_include_directories(mini2_proto PUBLIC ${GRPC_INCLUDE_DIRS} ${PROTOBUF_INCLUDE_DIRS})
    target_link_libraries(mini2_proto PUBLIC ${GRPC_LIBRARIES} ${PROTOBUF_LIBRARIES})
endif()

# ------------------------------------------------------------
# Library shared by server + client.
# ------------------------------------------------------------
add_library(mini2_core STATIC
    src/Config.cpp
    src/PartitionStore.cpp
    src/Indices.cpp
    src/QueryEngine.cpp
    src/ChunkBuffer.cpp
    src/QueryCache.cpp
    src/RequestRegistry.cpp
    src/PeerClient.cpp
    src/NodeServer.cpp
)
target_include_directories(mini2_core PUBLIC include "${GEN_DIR}")
target_link_libraries(mini2_core PUBLIC mini2_proto)

# ------------------------------------------------------------
# Executables
# ------------------------------------------------------------
add_executable(node_server src/server_main.cpp)
target_link_libraries(node_server PRIVATE mini2_core)

add_executable(portal_client src/client_main.cpp)
target_link_libraries(portal_client PRIVATE mini2_core)
