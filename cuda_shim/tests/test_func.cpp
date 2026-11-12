#include <cstdio>
#include <ffnvcodec/dynlink_cuda.h>
extern "C" {
CUresult cuInit(unsigned int);
CUresult cuDriverGetVersion(int*);
CUresult cuDeviceGetCount(int*);
CUresult cuDeviceGet(CUdevice*, int);
CUresult cuDeviceGetName(char*, int, CUdevice);
CUresult cuMemAlloc_v2(CUdeviceptr*, size_t);
CUresult cuMemFree_v2(CUdeviceptr);
}
int main() {
    CUresult r;
    r = cuInit(0);
    printf("cuInit(0) = %d\n", (int)r);
    int ver = 0; r = cuDriverGetVersion(&ver);
    printf("cuDriverGetVersion = %d (ver=%d)\n", (int)r, ver);
    int count = 0; r = cuDeviceGetCount(&count);
    printf("cuDeviceGetCount = %d (count=%d)\n", (int)r, count);
    if (count > 0) {
        CUdevice dev; r = cuDeviceGet(&dev, 0);
        printf("cuDeviceGet(0) = %d (dev=%d)\n", (int)r, dev);
        char name[256]; r = cuDeviceGetName(name, sizeof(name), dev);
        printf("cuDeviceGetName = %d (name=%s)\n", (int)r, name);
        CUdeviceptr p; r = cuMemAlloc_v2(&p, 1024);
        printf("cuMemAlloc_v2(1024) = %d (ptr=%p)\n", (int)r, (void*)(unsigned long long)p);
        if (r == CUDA_SUCCESS) { cuMemFree_v2(p); printf("cuMemFree ok\n"); }
    }
    return 0;
}
