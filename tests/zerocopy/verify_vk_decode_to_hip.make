# verify_vk_decode_to_hip.make
# P0：Vulkan Video 解码 → 拷贝(可导出 buffer) → HIP 导入读回，逐字节校验
#
# 运行（AMD 验证机，从 tests/zerocopy/ 目录）：
#   make -f verify_vk_decode_to_hip.make              # 编译
#   make -f verify_vk_decode_to_hip.make run-b        # I/P/B 30 帧端到端校验（P0 核心）
#   make -f verify_vk_decode_to_hip.make run-p        # I/P 30 帧
#   make -f verify_vk_decode_to_hip.make run-i        # I-only
#
# 依赖：AMD GPU + ROCm/HIP + Mesa(RADV, 带 Vulkan 头) + libvulkan-dev + FFmpeg 7.1 构建产物。

ROCM_DIR  ?= /opt/rocm
FFMPEG    ?= $(HOME)/ffmpeg-7.1
MESA      ?= $(HOME)/mesa
FFMPEG_BIN ?= ffmpeg
TESTDATA  ?= ../../testdata/h264
CXX       ?= g++
VULKAN_LIB ?= -lvulkan
.DEFAULT_GOAL := all

# -I$(MESA)/include 让 vulkan.h 及其 vk_video/* std 头可见。
CXXFLAGS := -std=c++17 -O2 -Wall -Wno-unused-parameter -Wno-register \
            -D__HIP_PLATFORM_AMD__ \
            -I$(ROCM_DIR)/include -I$(FFMPEG) -I$(MESA)/include

# Vulkan Video 命令通过 vkGetDeviceProcAddr 加载；这里只链接 loader + HIP。
LDFLAGS := -L$(ROCM_DIR)/lib -lamdhip64 -lhsa-runtime64 $(VULKAN_LIB) -ldl -Wl,-rpath,$(FFMPEG)/libavutil

# 与 VAAPI 版一致的 9 个 FFmpeg 解析对象。
FFOBJS := \
  $(FFMPEG)/libavcodec/h2645_parse.o \
  $(FFMPEG)/libavcodec/h264_ps.o \
  $(FFMPEG)/libavcodec/h2645_vui.o \
  $(FFMPEG)/libavcodec/h2645data.o \
  $(FFMPEG)/libavcodec/h264data.o \
  $(FFMPEG)/libavcodec/h264_parse.o \
  $(FFMPEG)/libavcodec/golomb.o \
  $(FFMPEG)/libavcodec/mathtables.o \
  $(FFMPEG)/libavcodec/refstruct.o
AVUTIL := $(firstword $(wildcard $(FFMPEG)/libavutil/libavutil.a) $(wildcard $(FFMPEG)/libavutil/libavutil.so))

# FFmpeg objects are prebuilt; do not let Make rebuild them via implicit rules.
$(FFOBJS) $(AVUTIL): ;

TARGET := verify_vk_decode_to_hip

all: $(TARGET)

$(TARGET): verify_vk_decode_to_hip.cpp $(FFOBJS) $(AVUTIL)
	$(CXX) $(CXXFLAGS) $< $(FFOBJS) $(AVUTIL) -o $@ $(LDFLAGS)

ref_320x240.yuv: $(TESTDATA)/test_320x240.h264
	$(FFMPEG_BIN) -y -i $< -pix_fmt nv12 -f rawvideo $@ 2>/dev/null

ref_p_320x240.yuv: $(TESTDATA)/test_p_320x240.h264
	$(FFMPEG_BIN) -y -i $< -pix_fmt nv12 -f rawvideo $@ 2>/dev/null

ref_b_320x240.yuv: $(TESTDATA)/test_b_320x240.h264
	$(FFMPEG_BIN) -y -i $< -pix_fmt nv12 -f rawvideo $@ 2>/dev/null

run-i: $(TARGET) ref_320x240.yuv
	./$(TARGET) $(TESTDATA)/test_320x240.h264 ref_320x240.yuv

run-p: $(TARGET) ref_p_320x240.yuv
	./$(TARGET) $(TESTDATA)/test_p_320x240.h264 ref_p_320x240.yuv

run-b: $(TARGET) ref_b_320x240.yuv
	./$(TARGET) $(TESTDATA)/test_b_320x240.h264 ref_b_320x240.yuv

clean:
	rm -f $(TARGET) ref_320x240.yuv ref_p_320x240.yuv ref_b_320x240.yuv

.PHONY: all run-i run-p run-b clean
