# verify_portability.make
# 实验 E：目标平台可移植性探测（§4.9 / §1.7 第0步）
#
# 运行（目标平台或 AMD 机器）：
#   make -f verify_portability.make
#   ./verify_portability

ROCM_DIR ?= /opt/rocm
CXX      ?= g++

CXXFLAGS := -std=c++17 -O2 -Wall -Wno-unused-parameter \
            -D__HIP_PLATFORM_AMD__ -I$(ROCM_DIR)/include -I/usr/include/libdrm

LDFLAGS := -L$(ROCM_DIR)/lib -lamdhip64 -lhsa-runtime64 -lva -lva-drm -ldl -ldrm

TARGET := verify_portability

all: $(TARGET)

$(TARGET): verify_portability.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
