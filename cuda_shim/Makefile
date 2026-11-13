# cuda_shim/Makefile
# 构建 libcuda.so.1（CUDA driver API → HIP shim）+ 验证工具。
# 零依赖（仅需 g++ + ROCm + nv-codec-headers），替代 CMake。
#
# 路径覆盖（按机器实际情况）：
#   make ROCM_DIR=/opt/rocm-7.2.4 NVCODEC_DIR=/path/to/nv-codec-headers/include

# ROCm 路径：优先用命令行传入，其次自动探测 /opt/rocm-* 最新版本
ROCM_DIR     ?= $(firstword $(wildcard /opt/rocm-*))
ifeq ($(ROCM_DIR),)
  ROCM_DIR := /opt/rocm
endif
NVCODEC_DIR  ?= $(HOME)/nv-codec-headers/include
FFMPEG_DIR   ?= $(HOME)/ffmpeg-7.1
CXX          ?= g++
CXXFLAGS     := -std=c++17 -fPIC -D__HIP_PLATFORM_AMD__ \
                -Iinclude -I$(NVCODEC_DIR) -I$(ROCM_DIR)/include
HIP_LIB      := $(ROCM_DIR)/lib/libamdhip64.so
# 设备端测试需要 hipcc；个别机器上 ROCm clang 找不到系统 libstdc++，
# 可用 HIPCC_EXTRA 补 -I/-L（见 ../docs/verification/验证手册.md）。
HIPCC        ?= $(ROCM_DIR)/bin/hipcc
HIPCC_EXTRA  ?=
# Some systems ship only libvulkan.so.1 (no -dev symlink). Override with:
#   make VULKAN_LIB=-l:libvulkan.so.1
VULKAN_LIB   ?= -lvulkan

all: libcuda.so.1 libnvcuvid.so.1 verify_symbols verify_cuvid_symbols verify_cuvid_session verify_cuvid_parser test_func test_func_full

# 生成带 soname 的 libcuda.so.1，并建 libcuda.so 开发软链（-lcuda 需要）
libcuda.so.1: src/libcuda.cpp include/cuda_shim_types.h
	$(CXX) $(CXXFLAGS) -shared $< -o $@ -Wl,-soname,libcuda.so.1 $(HIP_LIB)
	ln -sf libcuda.so.1 libcuda.so

NVCUVID_SRC := src/libnvcuvid.cpp src/vaapi_h264_decoder.cpp src/hip_frame_mapper.cpp \
	src/h264_picture_translate.cpp src/h264_direct_params.cpp src/cuvid_parser.cpp \
	src/backend_dispatch.cpp src/vulkan_h264_decoder.cpp
NVCUVID_HEADERS := src/vaapi_h264_decoder.h src/hip_frame_mapper.h src/h264_picture_translate.h \
	src/h264_direct_params.h src/cuvid_parser.h src/h264_bit_reader.h \
	src/decoder_backend.h src/backend_dispatch.h src/vulkan_h264_decoder.h src/vk_sequence_cache.h

# Mesa Vulkan 头（Vulkan Video 后端需要；含 vk_video/* std 头）
MESA_DIR ?= $(HOME)/mesa

# FFmpeg parser-reuse objects (see tests/parser/verify_parser_reuse.cpp experiment A).
FF_OBJS := h2645_parse.o h264_ps.o h2645_vui.o h2645data.o h264data.o \
	h264_parse.o golomb.o mathtables.o refstruct.o
FF_OBJ_FILES := $(addprefix $(FFMPEG_DIR)/libavcodec/,$(FF_OBJS))
AVUTIL_LIB := $(FFMPEG_DIR)/libavutil/libavutil.so

# The FFmpeg .o files are prebuilt by FFmpeg's own build; do not let make
# rebuild them via its implicit .c -> .o rule (they need FFmpeg's config.h).
$(FF_OBJ_FILES): ;
$(AVUTIL_LIB): ;

libnvcuvid.so.1: $(NVCUVID_SRC) $(NVCUVID_HEADERS) $(FF_OBJ_FILES) $(AVUTIL_LIB)
	$(CXX) $(CXXFLAGS) -Wno-register -I$(FFMPEG_DIR) -I$(MESA_DIR)/include -shared $(NVCUVID_SRC) \
		$(FF_OBJ_FILES) -o $@ -Wl,-soname,libnvcuvid.so.1 \
		$(HIP_LIB) -lva -lva-drm -ldrm $(VULKAN_LIB) -L$(FFMPEG_DIR)/libavutil -lavutil \
		-lz -lm -lpthread -ldl
	ln -sf libnvcuvid.so.1 libnvcuvid.so

# 符号导出验证（模拟 FFmpeg dynlink dlsym）
verify_symbols: tests/verify_symbols.cpp
	$(CXX) -std=c++17 -Iinclude -I$(NVCODEC_DIR) $< -o $@ -ldl

verify_cuvid_symbols: tests/verify_cuvid_symbols.cpp
	$(CXX) -std=c++17 $< -o $@ -ldl

verify_cuvid_session: tests/verify_cuvid_session.cpp libnvcuvid.so.1
	$(CXX) -std=c++17 -I$(NVCODEC_DIR) $< -o $@ -L. -lnvcuvid \
		-L$(FFMPEG_DIR)/libavutil -lavutil \
		-Wl,-rpath,$(abspath .) -Wl,-rpath,$(abspath $(FFMPEG_DIR)/libavutil)

verify_cuvid_parser: tests/verify_cuvid_parser.cpp libnvcuvid.so.1
	$(CXX) -std=c++17 -I$(NVCODEC_DIR) $< -o $@ -L. -lnvcuvid \
		-L$(FFMPEG_DIR)/libavutil -lavutil \
		-Wl,-rpath,$(abspath .) -Wl,-rpath,$(abspath $(FFMPEG_DIR)/libavutil)

# mapped dev_ptr 的 kernel 可访问性 + 真实 event 顺序验证。
# 需要 libnvcuvid 中已实现的 parser/decoder；测试本身只链接 CUVID + HIP。
verify_kernel_read: tests/verify_kernel_read.cpp libnvcuvid.so.1
	$(HIPCC) -x hip -std=c++17 $(CXXFLAGS) $(HIPCC_EXTRA) $< -o $@ \
		-L. -lnvcuvid -L$(ROCM_DIR)/lib -lamdhip64 \
		-L$(FFMPEG_DIR)/libavutil -lavutil \
		-Wl,-rpath,$(abspath .) -Wl,-rpath,$(ROCM_DIR)/lib \
		-Wl,-rpath,$(abspath $(FFMPEG_DIR)/libavutil)

# 功能冒烟测试（依赖 libcuda.so，首次构建必须先产出）
test_func: tests/test_func.cpp libcuda.so.1
	$(CXX) -std=c++17 -I$(NVCODEC_DIR) $< -o $@ -L. -lcuda -Wl,-rpath,$(abspath .)

# 功能真实性测试（数据校验）
test_func_full: tests/test_func_full.cpp libcuda.so.1
	$(CXX) -std=c++17 -I$(NVCODEC_DIR) $< -o $@ -L. -lcuda -Wl,-rpath,$(abspath .)

check: verify_symbols verify_cuvid_symbols libcuda.so.1 libnvcuvid.so.1
	./verify_symbols ./libcuda.so.1
	./verify_cuvid_symbols ./libnvcuvid.so.1

check_func: test_func_full
	./test_func_full

check_func_smoke: test_func
	./test_func

# parser 复用验证：静态链接 FFmpeg 解析 + 自填 VAAPI（shim M2 技术前提）
#
# 关键经验：直接链整个 libavcodec.a 会拖入所有编解码器的外部依赖（aom/zlib/...），
# 链接失败。只提取 SPS/PPS 解析真正需要的少量 .o 即可。
FFOBJ_DIR := build/ffobjs
FF_OBJS   := h2645_parse.o h264_ps.o h2645_vui.o h2645data.o h264data.o \
             golomb.o mathtables.o refstruct.o

$(FFOBJ_DIR)/%.o: $(FFMPEG_DIR)/libavcodec/libavcodec.a
	@mkdir -p $(FFOBJ_DIR)
	cd $(FFOBJ_DIR) && ar x $(abspath $<) $(notdir $@)

parser_reuse: ../tests/parser/verify_parser_reuse.cpp $(addprefix $(FFOBJ_DIR)/,$(FF_OBJS))
	$(CXX) -std=c++17 -D__HIP_PLATFORM_AMD__ \
	    -I$(FFMPEG_DIR) -I$(ROCM_DIR)/include $< -o $@ \
	    $(addprefix $(FFOBJ_DIR)/,$(FF_OBJS)) \
	    $(FFMPEG_DIR)/libavutil/libavutil.a \
	    -lva -lva-drm -ldrm -lz -lm -lpthread -ldl

check_parser: parser_reuse
	@echo "=== parser 复用验证（需要一个 Annex-B H264 码流） ==="
	@test -n "$(H264)" || { echo "用法: make check_parser H264=your.h264"; exit 1; }
	./parser_reuse $(H264)

clean:
	rm -f libcuda.so.1 libcuda.so libnvcuvid.so.1 libnvcuvid.so \
		verify_symbols verify_cuvid_symbols verify_cuvid_session \
		verify_cuvid_parser verify_kernel_read test_func test_func_full parser_reuse

.PHONY: all check check_func check_func_smoke parser_reuse check_parser clean
