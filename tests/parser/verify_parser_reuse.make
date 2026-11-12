# verify_parser_reuse.make
# 实验 A：FFmpeg parser 复用复现（§4.6）
#
# 只链接 8 个 .o（不链整个 libavcodec.a），验证"解析层复用、填充层自写"路线的
# 静态链接可行性。libavutil.a 只提供 av_malloc/av_log/av_buffer 等基础运行时。
#
# 用法：
#   make -f verify_parser_reuse.make            # 编译
#   ./verify_parser_reuse ../../testdata/h264/test_320x240.h264     # 运行
#
# 依赖：本机 ffmpeg-7.1 源码已构建（libavcodec/*.o、libavutil/libavutil.a 已存在）

FFMPEG  ?= $(HOME)/ffmpeg-7.1
TESTDATA ?= ../../testdata/h264
CXX     ?= g++
.DEFAULT_GOAL := all
CXXFLAGS := -std=c++17 -O2 -Wall -Wno-unused-parameter -I$(FFMPEG)
LDFLAGS  := -Wl,-rpath,$(FFMPEG)/libavutil

# 8 个关键 .o（文档 §4.6 确定的最小集合）
OBJS := \
  $(FFMPEG)/libavcodec/h2645_parse.o \
  $(FFMPEG)/libavcodec/h264_ps.o \
  $(FFMPEG)/libavcodec/h2645_vui.o \
  $(FFMPEG)/libavcodec/h2645data.o \
  $(FFMPEG)/libavcodec/h264data.o \
  $(FFMPEG)/libavcodec/golomb.o \
  $(FFMPEG)/libavcodec/mathtables.o \
  $(FFMPEG)/libavcodec/refstruct.o

AVUTIL := $(firstword $(wildcard $(FFMPEG)/libavutil/libavutil.a) $(wildcard $(FFMPEG)/libavutil/libavutil.so))

# FFmpeg objects are prebuilt by FFmpeg's own build; do not rebuild them
# via Make's implicit .c -> .o rule (they need FFmpeg's generated config.h).
$(OBJS) $(AVUTIL): ;

TARGET := verify_parser_reuse

all: $(TARGET)

$(TARGET): verify_parser_reuse.cpp $(OBJS) $(AVUTIL)
	$(CXX) $(CXXFLAGS) $< $(OBJS) $(AVUTIL) -o $@ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET) $(TESTDATA)/test_320x240.h264

clean:
	rm -f $(TARGET)

.PHONY: all run clean
