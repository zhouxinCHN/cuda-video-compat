# cuda_shim/CMakeLists.txt
# 构建 libcuda.so.1 —— CUDA driver API → HIP 的 shim
cmake_minimum_required(VERSION 3.16)
project(cuda_shim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# HIP 运行时头文件
find_package(hip QUIET)
if(NOT hip_FOUND)
    # 手动指定 ROCm 路径（Ubuntu 常见布局）
    set(HIP_INCLUDE_DIR /opt/rocm/include CACHE PATH "ROCm include dir")
    set(HIP_LIB_DIR /opt/rocm/lib CACHE PATH "ROCm lib dir")
    message(STATUS "hip cmake config 未找到，用硬编码路径: ${HIP_INCLUDE_DIR}")
endif()

# nv-codec-headers 路径
set(NV_CODEC_INCLUDE "$ENV{HOME}/nv-codec-headers/include" CACHE PATH "nv-codec-headers include dir")

# 生成 libcuda.so.1（带 soname 版本）
add_library(cuda SHARED src/libcuda.cpp)
set_target_properties(cuda PROPERTIES
    OUTPUT_NAME "cuda"
    VERSION "1.0.0"
    SOVERSION "1")

target_include_directories(cuda PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${NV_CODEC_INCLUDE}
    ${HIP_INCLUDE_DIR})

target_compile_definitions(cuda PRIVATE __HIP_PLATFORM_AMD__)
target_link_directories(cuda PRIVATE ${HIP_LIB_DIR})
target_link_libraries(cuda PRIVATE amdhip64)

# 验证工具（模拟 FFmpeg dynlink 的 dlsym 加载）
add_executable(verify_symbols tests/verify_symbols.cpp)
target_include_directories(verify_symbols PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${NV_CODEC_INCLUDE})
target_link_libraries(verify_symbols PRIVATE dl)
