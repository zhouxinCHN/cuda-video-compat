// hip_read_surface.cpp
// 闭环验证工具：从 VAAPI 导出的 DRM-PRIME fd 导入 HIP，读取指定平面并导出为原始文件。
// 配合脚本使用：
//   run_decode_loop.sh   解码闭环：FFmpeg vaapi 解码 → 导出帧 → 本工具读 Y/UV → 对比参考
//   run_encode_loop.sh   编码闭环：本工具写已知图案 → 导入 VAAPI → 编码 → 回读对比
//
// 用法：
//   hip_read_surface --read <fd> <size> <offset> <out_file> [--dummy]
//     --read  : 从 fd+offset 读取 size 字节到 out_file（验证 HIP 能否从该 fd 取数据）
//   hip_write_pattern --write <fd> <size> <pattern> [--dummy]
//     --write : 向 fd 写入已知图案（验证 HIP 能写 VAAPI 将消费的内存）
//
// --dummy 模式：不依赖真实 VAAPI 解码，直接写入参考图案到导出 buffer，
//               用于单独验证 HIP 读写 VAAPI 共享内存的正确性。

#include <hip/hip_runtime_api.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <string>

using namespace std;

static const char* hipErrStr(hipError_t e) { return hipGetErrorString(e); }

// 导入 fd 为 HIP 外部内存，映射为设备指针
static bool hip_import_fd(int fd, uint64_t size, hipExternalMemory_t* extMem, void** devPtr) {
    hipExternalMemoryHandleDesc desc = {};
    desc.type = hipExternalMemoryHandleTypeOpaqueFd;
    desc.handle.fd = fd;
    desc.size = size;

    hipError_t e = hipImportExternalMemory(extMem, &desc);
    if (e != hipSuccess) { fprintf(stderr, "hipImportExternalMemory: %s\n", hipErrStr(e)); return false; }

    hipExternalMemoryBufferDesc bufDesc = {};
    bufDesc.size = size;
    e = hipExternalMemoryGetMappedBuffer(devPtr, *extMem, &bufDesc);
    if (e != hipSuccess) { fprintf(stderr, "hipExternalMemoryGetMappedBuffer: %s\n", hipErrStr(e)); return false; }
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("用法:\n");
        printf("  %s --read <fd> <size> <offset> <outfile>\n", argv[0]);
        printf("  %s --write <fd> <size> <pattern>   # pattern: 0..255\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--read") == 0) {
        if (argc != 6) { fprintf(stderr, "--read 需要 4 个参数\n"); return 1; }
        int fd = atoi(argv[2]);
        size_t size = (size_t)strtoull(argv[3], nullptr, 0);
        size_t offset = (size_t)strtoull(argv[4], nullptr, 0);
        const char* outfile = argv[5];

        if (hipInit(0) != hipSuccess) { fprintf(stderr, "hipInit failed\n"); return 1; }

        hipExternalMemory_t extMem = nullptr;
        void* devPtr = nullptr;
        // 映射 offset 之后的内存（外部内存描述符从 offset 开始）
        // 注意：HIP 的 bufferDesc 支持 offset。我们映射整个对象，然后用偏移指针。
        if (!hip_import_fd(fd, size + offset, &extMem, &devPtr)) return 1;

        // 从 offset 处读取
        void* readPtr = (char*)devPtr + offset;
        vector<unsigned char> host(size);
        hipError_t e = hipMemcpy(host.data(), readPtr, size, hipMemcpyDeviceToHost);
        if (e != hipSuccess) { fprintf(stderr, "hipMemcpy: %s\n", hipErrStr(e)); return 1; }

        FILE* f = fopen(outfile, "wb");
        if (!f) { fprintf(stderr, "fopen %s: %s\n", outfile, strerror(errno)); return 1; }
        fwrite(host.data(), 1, host.size(), f);
        fclose(f);
        printf("读回 %zu bytes 到 %s\n", host.size(), outfile);

        hipDestroyExternalMemory(extMem);
        return 0;
    }

    if (strcmp(argv[1], "--write") == 0) {
        if (argc != 5) { fprintf(stderr, "--write 需要 3 个参数\n"); return 1; }
        int fd = atoi(argv[2]);
        size_t size = (size_t)strtoull(argv[3], nullptr, 0);
        unsigned char pattern = (unsigned char)atoi(argv[4]);

        if (hipInit(0) != hipSuccess) { fprintf(stderr, "hipInit failed\n"); return 1; }

        hipExternalMemory_t extMem = nullptr;
        void* devPtr = nullptr;
        if (!hip_import_fd(fd, size, &extMem, &devPtr)) return 1;

        // GPU 写已知图案
        hipError_t e = hipMemset(devPtr, pattern, size);
        if (e != hipSuccess) { fprintf(stderr, "hipMemset: %s\n", hipErrStr(e)); return 1; }

        // 等 GPU 完成
        e = hipDeviceSynchronize();
        if (e != hipSuccess) { fprintf(stderr, "hipDeviceSynchronize: %s\n", hipErrStr(e)); return 1; }
        printf("已向 fd=%d 写入 %zu bytes 的 0x%02x 图案\n", fd, size, pattern);

        hipDestroyExternalMemory(extMem);
        return 0;
    }

    fprintf(stderr, "未知命令 %s\n", argv[1]);
    return 1;
}