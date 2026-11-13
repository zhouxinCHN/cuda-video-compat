# Build the NPP Stage A probe on either NVIDIA NPP or the AMD NPP shim.
#
# NVIDIA golden host:
#   make clean all GPU_RUNTIME=cuda \
#     NPP_INCLUDE_DIR=/usr/local/cuda/include \
#     CUDA_INCLUDE_DIR=/usr/local/cuda/include \
#     CUDA_LIB_DIR=/usr/local/cuda/lib64 \
#     NPP_LIB_DIR=/usr/local/cuda/lib64
#
# AMD shim host:
#   make clean all GPU_RUNTIME=shim \
#     NPP_INCLUDE_DIR=/path/to/cuda/include \
#     NPP_LIB_DIR=../../npp_shim \
#     ROCM_DIR=/opt/rocm

GPU_RUNTIME     ?= cuda
ROCM_DIR        ?= $(firstword $(wildcard /opt/rocm-*))
ifeq ($(ROCM_DIR),)
  ROCM_DIR := /opt/rocm
endif
CXX             ?= g++
HIPCC           ?= $(ROCM_DIR)/bin/hipcc
HIPCC_EXTRA     ?=
NPP_INCLUDE_DIR ?=
CUDA_INCLUDE_DIR?= $(NPP_INCLUDE_DIR)
CUDA_LIB_DIR    ?= /usr/local/cuda/lib64
NPP_LIB_DIR     ?= $(CUDA_LIB_DIR)
BUILD_DIR       ?= build

BIN := npp_stage_a_probe
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -I$(NPP_INCLUDE_DIR) -I$(CUDA_INCLUDE_DIR)
NPP_LIBS := -L$(NPP_LIB_DIR) -lnppc -lnppig -lnppicc -lnppidei -lnppif

ifeq ($(GPU_RUNTIME),shim)
  GPU_BRIDGE_SRC := npp_probe_gpu_hip.cpp
  GPU_BRIDGE_CXX := $(HIPCC)
  GPU_BRIDGE_FLAGS := $(HIPCC_EXTRA)
  LDFLAGS := -L$(ROCM_DIR)/lib -Wl,-rpath,$(abspath $(ROCM_DIR)/lib) \
             -Wl,-rpath,$(abspath $(NPP_LIB_DIR))
  LDLIBS := $(NPP_LIBS) -lamdhip64
else ifeq ($(GPU_RUNTIME),cuda)
  GPU_BRIDGE_SRC := npp_probe_gpu_cuda.cpp
  GPU_BRIDGE_CXX := $(CXX)
  GPU_BRIDGE_FLAGS :=
  LDFLAGS := -L$(CUDA_LIB_DIR) -Wl,-rpath,$(abspath $(CUDA_LIB_DIR)) \
             -Wl,-rpath,$(abspath $(NPP_LIB_DIR))
  LDLIBS := $(NPP_LIBS) -lcudart
else
  $(error GPU_RUNTIME must be 'cuda' or 'shim')
endif

.PHONY: all clean run

all: $(BIN)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/npp_stage_a_probe.o: npp_stage_a_probe.cpp npp_probe_gpu.h $(BUILD_DIR)
	@test -n "$(NPP_INCLUDE_DIR)" || \
		{ echo "NPP_INCLUDE_DIR is required (authorized NPP headers)" >&2; exit 2; }
	@test -f "$(NPP_INCLUDE_DIR)/nppi.h" || \
		{ echo "$(NPP_INCLUDE_DIR)/nppi.h not found" >&2; exit 2; }
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/npp_probe_gpu.o: $(GPU_BRIDGE_SRC) npp_probe_gpu.h $(BUILD_DIR)
	$(GPU_BRIDGE_CXX) $(CXXFLAGS) $(GPU_BRIDGE_FLAGS) -c $< -o $@

$(BIN): $(BUILD_DIR)/npp_stage_a_probe.o $(BUILD_DIR)/npp_probe_gpu.o
	$(CXX) $^ -o $@ $(LDFLAGS) $(LDLIBS)

run: $(BIN)
	./$(BIN) --output npp-stage-a-output

clean:
	rm -rf $(BUILD_DIR) $(BIN) npp-stage-a-output
