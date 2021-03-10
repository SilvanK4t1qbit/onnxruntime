# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

file(GLOB_RECURSE onnxruntime_wasm_src CONFIGURE_DEPENDS
  "${ONNXRUNTIME_ROOT}/wasm/api.h"
  "${ONNXRUNTIME_ROOT}/wasm/api.cc"
)

source_group(TREE ${REPO_ROOT} FILES ${onnxruntime_wasm_src})

add_executable(onnxruntime_wasm
  ${onnxruntime_wasm_src}
)

add_compile_definitions(
  ENABLE_ORT_WASM
  MLAS_NO_ONNXRUNTIME_THREADPOOL
)

string(APPEND CMAKE_CXX_FLAGS_DEBUG " -g4 --source-map-base http://localhost:8000/")

target_compile_options(onnx PRIVATE -Wno-unused-parameter -Wno-unused-variable)

# Override re2 compiler options to remove -pthread
set_property(TARGET re2 PROPERTY COMPILE_OPTIONS )

target_link_libraries(onnxruntime_wasm PRIVATE
  protobuf::libprotobuf
  onnx
  onnx_proto
  onnxruntime_common
  onnxruntime_flatbuffers
  onnxruntime_framework
  onnxruntime_graph
  onnxruntime_mlas
  onnxruntime_optimizer
  onnxruntime_providers
  onnxruntime_session
  onnxruntime_util
  re2::re2
)

set_target_properties(onnxruntime_wasm PROPERTIES LINK_FLAGS "                                                   \
                      -s \"EXTRA_EXPORTED_RUNTIME_METHODS=['UTF8ToString','stringToUTF8','lengthBytesUTF8']\"    \
                      -s EXPORT_NAME=onnxjs                                                                      \
                      -s WASM=1                                                                                  \
                      -s NO_EXIT_RUNTIME=0                                                                       \
                      -s ALLOW_MEMORY_GROWTH=1                                                                   \
                      -s SAFE_HEAP=0                                                                             \
                      -s MODULARIZE=1                                                                            \
                      -s SAFE_HEAP_LOG=0                                                                         \
                      -s STACK_OVERFLOW_CHECK=0                                                                  \
                      -s EXPORT_ALL=0                                                                            \
                      -s LLD_REPORT_UNDEFINED                                                                    \
                      -s VERBOSE=0                                                                               \
                      -s ASSERTIONS=1                                                                            \
                      -s NO_FILESYSTEM=1                                                                         \
                      --no-entry")
if (CMAKE_BUILD_TYPE STREQUAL "Debug" OR onnxruntime_ENABLE_WEBASSEMBLY_TEST)
  set_property(TARGET onnxruntime_wasm APPEND_STRING PROPERTY LINK_FLAGS " -s DEMANGLE_SUPPORT=1 -s DISABLE_EXCEPTION_CATCHING=0")
else()
  set_property(TARGET onnxruntime_wasm APPEND_STRING PROPERTY LINK_FLAGS " -s DEMANGLE_SUPPORT=0 -s DISABLE_EXCEPTION_CATCHING=1")
endif()