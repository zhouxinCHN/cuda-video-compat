# tests/vulkan-video/full/Makefile
# Vulkan Video 完整验证程序

ROCM_DIR ?= /opt/rocm
VULKAN_HEADERS ?= $(HOME)/mesa/include
VULKAN_LIB ?= -lvulkan
CXX      ?= g++
CXXFLAGS  = -O2 -std=c++17 -Wall -D__HIP_PLATFORM_AMD__ -I$(ROCM_DIR)/include -I$(VULKAN_HEADERS)
LDFLAGS   = -L$(ROCM_DIR)/lib -Wl,--no-as-needed $(ROCM_DIR)/lib/libamdhip64.so $(VULKAN_LIB)

TARGET = verify_vulkan_video_full
SRC    = verify_vulkan_video_full.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
