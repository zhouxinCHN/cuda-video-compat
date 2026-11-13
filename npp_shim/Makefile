# NPP compatibility shim — Stage A
#
# The repository does not contain NVIDIA NPP headers. Build against a licensed
# CUDA/NPP header set supplied by the user/CI:
#
#   make NPP_INCLUDE_DIR=/usr/include ROCM_DIR=/opt/rocm
#
# CUDA 12 headers can build the shim for development, but the target ABI and
# golden baseline remain NPP 11.5.1.107. Record the header version used.

ROCM_DIR     ?= $(firstword $(wildcard /opt/rocm-*))
ifeq ($(ROCM_DIR),)
  ROCM_DIR := /opt/rocm
endif

NPP_INCLUDE_DIR ?=
CXX             ?= g++
HIPCC           ?= $(ROCM_DIR)/bin/hipcc
HIPCC_EXTRA     ?=
HIP_LINK        ?= -L$(ROCM_DIR)/lib -lamdhip64
OPT             ?= -O2
BUILD_DIR       ?= build

NPP_CXXFLAGS := -std=c++17 -fPIC $(OPT) -Wall -Wextra -I$(NPP_INCLUDE_DIR) -Isrc
HIP_CXXFLAGS := -std=c++17 -fPIC $(OPT) -Wall -Wextra -Isrc $(HIPCC_EXTRA)
SONAME = -Wl,-soname,$@

LIBS := libnppc.so.11 libnppig.so.11 libnppicc.so.11 \
        libnppidei.so.11 libnppif.so.11

.PHONY: all clean check-headers check-symbols

all: check-headers $(LIBS)
	@for lib in $(LIBS); do ln -sf "$$lib" "$${lib%.11}"; done
	@echo "Built NPP Stage A shims in $(CURDIR)"
	@echo "Run: make check-symbols"

check-headers:
	@if [ -z "$(NPP_INCLUDE_DIR)" ]; then \
		echo "ERROR: NPP_INCLUDE_DIR is required (authorized NPP headers)." >&2; \
		exit 2; \
	fi
	@if [ ! -f "$(NPP_INCLUDE_DIR)/nppi.h" ]; then \
		echo "ERROR: $(NPP_INCLUDE_DIR)/nppi.h not found." >&2; \
		echo "Use an authorized CUDA/NPP header tree; headers are not vendored." >&2; \
		exit 2; \
	fi

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/nppc_core.o: src/nppc_core.cpp $(BUILD_DIR)
	$(CXX) $(NPP_CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/nppig_resize.o: src/nppig_resize.cpp src/npp_common.h src/npp_gpu_bridge.h $(BUILD_DIR)
	$(CXX) $(NPP_CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/nppig_resize_hip.o: src/nppig_resize_hip.cpp src/npp_gpu_bridge.h $(BUILD_DIR)
	$(HIPCC) $(HIP_CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/nppicc_color.o: src/nppicc_color.cpp src/npp_common.h src/npp_gpu_bridge.h $(BUILD_DIR)
	$(CXX) $(NPP_CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/nppicc_color_hip.o: src/nppicc_color_hip.cpp src/npp_gpu_bridge.h $(BUILD_DIR)
	$(HIPCC) $(HIP_CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/npp_placeholder.o: src/npp_placeholder.cpp $(BUILD_DIR)
	$(CXX) $(NPP_CXXFLAGS) -c $< -o $@

libnppc.so.11: $(BUILD_DIR)/nppc_core.o
	$(CXX) -shared $^ -o $@ $(SONAME)

libnppig.so.11: $(BUILD_DIR)/nppig_resize.o $(BUILD_DIR)/nppig_resize_hip.o
	$(CXX) -shared $^ -o $@ $(SONAME) $(HIP_LINK) -lm

libnppicc.so.11: $(BUILD_DIR)/nppicc_color.o $(BUILD_DIR)/nppicc_color_hip.o
	$(CXX) -shared $^ -o $@ $(SONAME) $(HIP_LINK) -lm

libnppidei.so.11: $(BUILD_DIR)/npp_placeholder.o
	$(CXX) -shared $^ -o $@ $(SONAME)

libnppif.so.11: $(BUILD_DIR)/npp_placeholder.o
	$(CXX) -shared $^ -o $@ $(SONAME)

check-symbols:
	@nm -D --defined-only libnppc.so.11 | grep nppGetLibVersion
	@nm -D --defined-only libnppig.so.11 | grep nppiResizeSqrPixel_8u_C1R
	@nm -D --defined-only libnppicc.so.11 | grep nppiYCbCr420_8u_P2P3R
	@nm -D --defined-only libnppicc.so.11 | grep nppiYCbCr420_8u_P3P2R
	@echo "Stage A NPP symbols exported"

clean:
	rm -rf $(BUILD_DIR)
	rm -f libnppc.so.11 libnppig.so.11 libnppicc.so.11 \
	      libnppidei.so.11 libnppif.so.11 \
	      libnppc.so libnppig.so libnppicc.so libnppidei.so libnppif.so
