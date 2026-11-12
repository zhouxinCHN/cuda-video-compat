# tests/sync/Makefile
# 跨引擎同步验证（VAAPI ↔ HIP）

ROCM_DIR  ?= /opt/rocm
CXX       ?= g++
CXXFLAGS  := -std=c++17 -D__HIP_PLATFORM_AMD__ -Wno-unused-result -I$(ROCM_DIR)/include -I/usr/include -I/usr/include/drm
LDFLAGS   := -L$(ROCM_DIR)/lib -lamdhip64 -lva -lva-drm -ldl -lpthread -ldrm

TARGETS := verify_sync verify_semaphore_async

all: $(TARGETS)

verify_sync: verify_sync.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

verify_semaphore_async: verify_semaphore_async.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

run: verify_sync
	@echo "=== 运行跨引擎同步验证（S1-S8） ==="
	./verify_sync

semaphore: verify_semaphore_async
	@echo "=== 异步跨栈同步探测（HIP semaphore + DRM syncobj/sync_file） ==="
	./verify_semaphore_async

clean:
	rm -f $(TARGETS)

.PHONY: all run semaphore clean
