# verify_decode_to_hip.make
# 实验 C：真实解码 → HIP 读，端到端像素校验（§1.7 第1步）
#
# 在 AMD 机器上运行（从 tests/zerocopy/ 目录）：
#   make -f verify_decode_to_hip.make              # 编译
#   make -f verify_decode_to_hip.make run          # 生成软解参考 + 跑 I-only
#   make -f verify_decode_to_hip.make run-p        # I/P 30 帧
#   make -f verify_decode_to_hip.make run-b        # I/P/B 30 帧
#
# 依赖：AMD GPU + ROCm/HIP + Mesa radeonsi(libva) + FFmpeg 7.1 源码构建产物（9 个 .o）。
# FFmpeg 源码不随仓库分发；用 FFMPEG=/path/to/ffmpeg 指定。

ROCM_DIR ?= /opt/rocm
FFMPEG   ?= $(HOME)/ffmpeg-7.1
FFMPEG_BIN ?= ffmpeg
TESTDATA ?= ../../testdata/h264
CXX      ?= g++
.DEFAULT_GOAL := all

CXXFLAGS := -std=c++17 -O2 -Wall -Wno-unused-parameter -Wno-register \
            -D__HIP_PLATFORM_AMD__ -I$(ROCM_DIR)/include -I$(FFMPEG)

LDFLAGS := -L$(ROCM_DIR)/lib -lamdhip64 -lhsa-runtime64 -lva -lva-drm -ldl -Wl,-rpath,$(FFMPEG)/libavutil

# FFmpeg parser 复用 9 个 .o + libavutil.a（h264_parse.o 供 POC/ref_count）
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

TARGET := verify_decode_to_hip

all: $(TARGET)

$(TARGET): verify_decode_to_hip.cpp $(FFOBJS) $(AVUTIL)
	$(CXX) $(CXXFLAGS) $< $(FFOBJS) $(AVUTIL) -o $@ $(LDFLAGS)

# 软解参考帧（多帧 NV12）
ref_320x240.yuv: $(TESTDATA)/test_320x240.h264
	$(FFMPEG_BIN) -y -i $< -pix_fmt nv12 -f rawvideo $@ 2>/dev/null

ref_p_320x240.yuv: $(TESTDATA)/test_p_320x240.h264
	$(FFMPEG_BIN) -y -i $< -pix_fmt nv12 -f rawvideo $@ 2>/dev/null

ref_b_320x240.yuv: $(TESTDATA)/test_b_320x240.h264
	$(FFMPEG_BIN) -y -i $< -pix_fmt nv12 -f rawvideo $@ 2>/dev/null

run: $(TARGET) ref_320x240.yuv
	./$(TARGET) $(TESTDATA)/test_320x240.h264 ref_320x240.yuv

run-p: $(TARGET) ref_p_320x240.yuv
	./$(TARGET) $(TESTDATA)/test_p_320x240.h264 ref_p_320x240.yuv

run-b: $(TARGET) ref_b_320x240.yuv
	./$(TARGET) $(TESTDATA)/test_b_320x240.h264 ref_b_320x240.yuv

clean:
	rm -f $(TARGET) ref_320x240.yuv ref_p_320x240.yuv ref_b_320x240.yuv

.PHONY: all run run-p run-b clean
