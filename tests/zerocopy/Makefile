# tests/zerocopy/Makefile
# 零拷贝验证实验（ROCm + Mesa 同 GPU 内存互操作）
# 在 AMD 机器（RX 7600 / RX 9070）上运行：make && make run
#
# 注意：若 ROCm 装在别处，用 make ROCM_DIR=/path/to/rocm 覆盖。

ROCM_DIR  ?= /opt/rocm
CXX       ?= g++
HIPCC     ?= $(ROCM_DIR)/bin/hipcc
NVCODEC_DIR ?= $(HOME)/nv-codec-headers/include
SHIM_DIR  ?= ../../cuda_shim
CXXFLAGS  := -std=c++17 -D__HIP_PLATFORM_AMD__ -Wno-unused-result -I$(ROCM_DIR)/include
LDFLAGS   := -L$(ROCM_DIR)/lib -lamdhip64 -lhsa-runtime64 -lva -lva-drm -ldl

TARGETS := verify_zerocopy retry_exp3 retry_exp3_v2 hip_read_surface verify_modifier_linear verify_p010_linear

all: $(TARGETS)

verify_zerocopy: verify_zerocopy.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

retry_exp3: retry_exp3.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

retry_exp3_v2: retry_exp3_v2.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

hip_read_surface: hip_read_surface.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

verify_modifier_linear: verify_modifier_linear.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ -lva -lva-drm

verify_p010_linear: verify_p010_linear.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS) -lva -lva-drm

# CUVID-level dynamic-resolution host: CUVID parser + decoder recreation.
verify_resolution_change: verify_resolution_change.cpp verify_resolution_change_hip.cpp
	$(CXX) -std=c++17 -O2 -I$(NVCODEC_DIR) -c verify_resolution_change.cpp \
		-o verify_resolution_change_host.o
	$(HIPCC) -std=c++17 -O2 -c verify_resolution_change_hip.cpp \
		-o verify_resolution_change_hip.o
	$(CXX) verify_resolution_change_host.o verify_resolution_change_hip.o -o $@ \
		-L$(SHIM_DIR) -Wl,-rpath,$(abspath $(SHIM_DIR)) -lnvcuvid \
		-L$(ROCM_DIR)/lib -Wl,-rpath,$(abspath $(ROCM_DIR)/lib) -lamdhip64

run: verify_zerocopy
	@echo "=== 运行零拷贝验证（实验1/2/3） ==="
	./verify_zerocopy

retry: retry_exp3
	@echo "=== 运行实验3失败定位（内存池拓扑 + 标准答案 + 多池导入） ==="
	./retry_exp3

retry2: retry_exp3_v2
	@echo "=== 运行实验3边界2定位 v2（往返自检 + 线性导入） ==="
	./retry_exp3_v2

loop-decode: hip_read_surface
	@echo "=== 闭环验证1：真实解码 → 导出 → HIP 读 ==="
	./run_decode_loop.sh

loop-encode: hip_read_surface
	@echo "=== 闭环验证2：HIP 写 → VAAPI 编码 → 回读 ==="
	./run_encode_loop.sh

linear: retry_exp3_v2
	@echo "=== 线性 vs tiled 布局与性能试验 ==="
	./run_linear_vs_tiled.sh

multires: retry_exp3_v2
	@echo "=== 多分辨率线性 vs tiled 解码验证 ==="
	./run_multires_linear.sh

modifier-linear: verify_modifier_linear
	@echo "=== 生产路径：modifier 限定 LINEAR 创建 surface ==="
	./verify_modifier_linear

p010: verify_p010_linear
	@echo "=== 10-bit (P010) 线性 surface 验证 ==="
	./verify_p010_linear

dynres: verify_resolution_change
	@echo "=== 动态分辨率：CUVID 宿主重建解码器 ==="
	./run_resolution_change.sh

clean:
	rm -f $(TARGETS) verify_resolution_change verify_resolution_change_host.o \
	      verify_resolution_change_hip.o

.PHONY: all run retry retry2 loop-decode loop-encode linear multires modifier-linear p010 dynres clean
