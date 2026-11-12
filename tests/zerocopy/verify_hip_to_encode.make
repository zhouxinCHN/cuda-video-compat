# verify_hip_to_encode.make
# 实验 D：HIP 写 → VAAPI 编码，端到端内容校验（§1.7 第2步）
#
# 在 AMD 机器上运行：
#   make -f verify_hip_to_encode.make              # 编译
#   make -f verify_hip_to_encode.make run          # 跑 + 软解回读比对
#
# 依赖：AMD GPU + ROCm/HIP + Mesa radeonsi(libva) + ffmpeg

ROCM_DIR ?= /opt/rocm
FFMPEG_BIN ?= ffmpeg
CXX      ?= g++

CXXFLAGS := -std=c++17 -O2 -Wall -Wno-unused-parameter \
            -D__HIP_PLATFORM_AMD__ -I$(ROCM_DIR)/include

LDFLAGS := -L$(ROCM_DIR)/lib -lamdhip64 -lhsa-runtime64 -lva -lva-drm -ldl

TARGET := verify_hip_to_encode

all: $(TARGET)

$(TARGET): verify_hip_to_encode.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

# 运行 + 用 ffmpeg 软解回读，比对源图案（渐变 Y=0..255, UV=128）
run: $(TARGET)
	./$(TARGET)
	@echo "  软解回读比对："
	$(FFMPEG_BIN) -y -i /tmp/hip_to_encode_out.h264 -frames:v 1 -pix_fmt nv12 -f rawvideo /tmp/hip_to_encode_dec.yuv 2>/dev/null
	python3 run_hip_encode_compare.py

clean:
	rm -f $(TARGET)

.PHONY: all run clean
