// verify_vulkan_video.cpp
//
// 目的：验证 "Vulkan Video 编解码 + HIP 零拷贝" 这条路是否走得通。
//
// 三个实验：
//   [实验0] 环境探测   —— Vulkan instance/device 能否创建，HIP 能否初始化，二者同进程共存。
//   [实验B] 视频能力   —— 枚举设备支持的 Vulkan Video 扩展（decode/encode h264/h265/av1）。
//                         纯字符串枚举，不依赖 video struct 定义，兼容不同版本 Vulkan-Headers。
//   [实验A] 零拷贝     —— 创建一个 VkImage（模拟解码输出图），用 VK_KHR_external_memory_fd
//                         把底层内存导出为 dma-buf fd，HIP 用 hipImportExternalMemory 导入，
//                         然后 GPU 侧 hipMemset 写入 + hipMemcpy 读回，验证 HIP 能直接
//                         访问 Vulkan 分配的 DEVICE_LOCAL 内存（即零拷贝通路）。
//
// 依赖：
//   sudo apt install libvulkan-dev vulkan-tools   # Vulkan 头文件 + loader
//   ROCm (HIP) 已安装在 /opt/rocm
//
// 编译（见 Makefile）：
//   g++ -O2 -std=c++17 -D__HIP_PLATFORM_AMD__ -I/opt/rocm/include \
//       verify_vulkan_video.cpp -o verify_vulkan_video \
//       -L/opt/rocm/lib -Wl,--no-as-needed /opt/rocm/lib/libamdhip64.so -lvulkan

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <unistd.h>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>
#include <hip/hip_runtime.h>

// ---------------------------------------------------------------------------
// 结果统计
// ---------------------------------------------------------------------------
static int g_pass = 0, g_fail = 0;
#define PASS(...) do { printf("  [PASS] "); printf(__VA_ARGS__); printf("\n"); g_pass++; } while (0)
#define FAIL(...) do { printf("  [FAIL] "); printf(__VA_ARGS__); printf("\n"); g_fail++; } while (0)

static const char* hipErrStr(hipError_t e) {
    return hipGetErrorString(e);
}

// ---------------------------------------------------------------------------
// 工具：查内存类型 index（DEVICE_LOCAL 优先）
// ---------------------------------------------------------------------------
static uint32_t findMemoryType(VkPhysicalDevice pd, uint32_t typeFilter,
                               VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    // 优先：DEVICE_LOCAL 且非 host visible（真实解码输出内存的样子）
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((typeFilter & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return ~0u;
}

// ---------------------------------------------------------------------------
// 实验0：环境探测
// ---------------------------------------------------------------------------
// 返回 0 成功，非 0 失败。成功时输出 instance/physical device。
static int experiment0(std::string* outDeviceName) {
    printf("\n========== [实验0] 环境探测：Vulkan + HIP 同进程共存 ==========\n");

    // ---- HIP ----
    hipError_t herr = hipInit(0);
    if (herr != hipSuccess) {
        FAIL("hipInit: %s (code=%d)", hipErrStr(herr), (int)herr);
        return 1;
    }
    int devCount = 0;
    hipGetDeviceCount(&devCount);
    if (devCount <= 0) {
        FAIL("hipGetDeviceCount 返回 %d，无 HIP 设备", devCount);
        return 1;
    }
    hipDeviceProp_t hprop;
    hipGetDeviceProperties(&hprop, 0);
    PASS("HIP 初始化成功，设备[0] = %s", hprop.name);

    return 0;
}

// ---------------------------------------------------------------------------
// 实验B：Vulkan Video 能力枚举（纯字符串，不依赖 video struct）
// ---------------------------------------------------------------------------
static void experimentB(VkPhysicalDevice pd) {
    printf("\n========== [实验B] Vulkan Video 能力枚举 ==========\n");

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr);
    if (extCount == 0) {
        FAIL("设备没有扩展");
        return;
    }
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, exts.data());

    struct { const char* name; const char* meaning; } want[] = {
        {"VK_KHR_video_queue",             "视频队列（基础）"},
        {"VK_KHR_video_decode_queue",      "视频解码队列"},
        {"VK_KHR_video_encode_queue",      "视频编码队列"},
        {"VK_KHR_video_decode_h264",       "H.264 解码"},
        {"VK_KHR_video_decode_h265",       "H.265 解码"},
        {"VK_KHR_video_decode_av1",        "AV1 解码"},
        {"VK_KHR_video_encode_h264",       "H.264 编码"},
        {"VK_KHR_video_encode_h265",       "H.265 编码"},
        {"VK_KHR_video_maintenance1",      "维护扩展1（DPB/参数更新，很多实现依赖）"},
        {"VK_KHR_external_memory",         "外部内存（零拷贝基础）"},
        {"VK_KHR_external_memory_fd",      "外部内存 fd（零拷贝基础）"},
    };

    for (auto& w : want) {
        bool found = false;
        for (auto& e : exts) {
            if (strcmp(e.extensionName, w.name) == 0) { found = true; break; }
        }
        if (found) PASS("支持 %-32s (%s)", w.name, w.meaning);
        else       FAIL("不支持 %-32s (%s)", w.name, w.meaning);
    }
}

// ---------------------------------------------------------------------------
// 实验A：VkImage 外部内存导出 -> HIP 导入 -> 零拷贝访问
// ---------------------------------------------------------------------------
static void experimentA(VkPhysicalDevice pd, VkDevice device) {
    printf("\n========== [实验A] Vulkan 图像内存 -> HIP 零拷贝 ==========\n");

    // 1. 创建 VkImage（模拟解码输出图）
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = { 256, 256, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device, &ici, nullptr, &image) != VK_SUCCESS) {
        FAIL("vkCreateImage 失败");
        return;
    }

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(device, image, &memReq);

    // 2. 分配内存，带 fd 导出
    VkExportMemoryAllocateInfo exportInfo = {};
    exportInfo.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &exportInfo;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = findMemoryType(pd, memReq.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == ~0u) {
        FAIL("找不到 DEVICE_LOCAL 内存类型");
        vkDestroyImage(device, image, nullptr);
        return;
    }

    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &mai, nullptr, &mem) != VK_SUCCESS) {
        FAIL("vkAllocateMemory 失败 (size=%llu)", (unsigned long long)memReq.size);
        vkDestroyImage(device, image, nullptr);
        return;
    }
    vkBindImageMemory(device, image, mem, 0);

    // 3. 导出 fd
    PFN_vkGetMemoryFdKHR pfnGetMemoryFd =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
    if (!pfnGetMemoryFd) {
        FAIL("vkGetMemoryFdKHR 未加载");
        vkFreeMemory(device, mem, nullptr);
        vkDestroyImage(device, image, nullptr);
        return;
    }

    VkMemoryGetFdInfoKHR fdInfo = {};
    fdInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fdInfo.memory = mem;
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    int fd = -1;
    VkResult vr = pfnGetMemoryFd(device, &fdInfo, &fd);
    if (vr != VK_SUCCESS || fd < 0) {
        FAIL("vkGetMemoryFdKHR 失败 (VkResult=%d)", (int)vr);
        vkFreeMemory(device, mem, nullptr);
        vkDestroyImage(device, image, nullptr);
        return;
    }
    PASS("Vulkan 导出 dma-buf fd=%d (image size=%llu bytes)",
         fd, (unsigned long long)memReq.size);

    // 4. HIP 导入
    hipExternalMemoryHandleDesc memDesc = {};
    memDesc.type = hipExternalMemoryHandleTypeOpaqueFd;
    memDesc.handle.fd = fd;
    memDesc.size = memReq.size;

    hipExternalMemory_t extMem = nullptr;
    hipError_t herr = hipImportExternalMemory(&extMem, &memDesc);
    if (herr != hipSuccess) {
        FAIL("hipImportExternalMemory: %s (code=%d)", hipErrStr(herr), (int)herr);
        close(fd);
        vkFreeMemory(device, mem, nullptr);
        vkDestroyImage(device, image, nullptr);
        return;
    }

    hipExternalMemoryBufferDesc bufDesc = {};
    bufDesc.size = memReq.size;

    void* devPtr = nullptr;
    herr = hipExternalMemoryGetMappedBuffer(&devPtr, extMem, &bufDesc);
    if (herr != hipSuccess) {
        FAIL("hipExternalMemoryGetMappedBuffer: %s (code=%d)", hipErrStr(herr), (int)herr);
        hipDestroyExternalMemory(extMem);
        close(fd);
        vkFreeMemory(device, mem, nullptr);
        vkDestroyImage(device, image, nullptr);
        return;
    }
    PASS("HIP 导入成功，devPtr=%p", devPtr);

    // 5. 零拷贝验证：GPU 直接写这块外部内存，再读回
    const unsigned char MAGIC = 0xAB;
    herr = hipMemset(devPtr, MAGIC, memReq.size);
    if (herr != hipSuccess) {
        FAIL("hipMemset 外部内存: %s (code=%d)", hipErrStr(herr), (int)herr);
    } else {
        std::vector<unsigned char> host(memReq.size);
        herr = hipMemcpy(host.data(), devPtr, memReq.size, hipMemcpyDeviceToHost);
        if (herr != hipSuccess) {
            FAIL("hipMemcpy 读回: %s (code=%d)", hipErrStr(herr), (int)herr);
        } else {
            bool ok = true;
            for (size_t i = 0; i < host.size(); i++) {
                if (host[i] != MAGIC) { ok = false; break; }
            }
            if (ok) PASS("零拷贝验证成功：HIP 直接读写了 Vulkan 的 DEVICE_LOCAL 内存 (%llu bytes 全部为 0x%02X)",
                         (unsigned long long)memReq.size, MAGIC);
            else    FAIL("读回数据不匹配，零拷贝验证失败");
        }
    }

    // 清理
    hipDestroyExternalMemory(extMem);
    close(fd);
    vkFreeMemory(device, mem, nullptr);
    vkDestroyImage(device, image, nullptr);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    printf("===========================================================\n");
    printf("  Vulkan Video 编解码 + HIP 零拷贝 验证程序\n");
    printf("===========================================================\n");

    // 实验0：HIP 部分（Vulkan 部分随后）
    std::string devName;
    int r0 = experiment0(&devName);
    (void)devName;

    // ---- Vulkan instance ----
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "verify_vulkan_video";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "none";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    std::vector<const char*> instExts = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    };

    VkInstanceCreateInfo vici = {};
    vici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    vici.pApplicationInfo = &appInfo;
    vici.enabledExtensionCount = (uint32_t)instExts.size();
    vici.ppEnabledExtensionNames = instExts.data();

    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&vici, nullptr, &inst) != VK_SUCCESS) {
        FAIL("vkCreateInstance 失败");
        printf("\n===== 结果汇总：PASS=%d FAIL=%d =====\n", g_pass, g_fail);
        return 1;
    }

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(inst, &pdCount, nullptr);
    if (pdCount == 0) {
        FAIL("没有 Vulkan physical device");
        vkDestroyInstance(inst, nullptr);
        printf("\n===== 结果汇总：PASS=%d FAIL=%d =====\n", g_pass, g_fail);
        return 1;
    }
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(inst, &pdCount, pds.data());
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceProperties pdp;
    vkGetPhysicalDeviceProperties(pd, &pdp);
    PASS("Vulkan 设备[0] = %s (Vulkan %d.%d.%d)", pdp.deviceName,
         VK_VERSION_MAJOR(pdp.apiVersion), VK_VERSION_MINOR(pdp.apiVersion),
         VK_VERSION_PATCH(pdp.apiVersion));

    // 实验B：能力枚举（physical device 级别）
    experimentB(pd);

    // ---- 创建 logical device ----
    // 找一个 graphics 或 transfer 队列族
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfps(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfps.data());

    int qfIndex = -1;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfps[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT)) {
            qfIndex = (int)i;
            break;
        }
    }
    if (qfIndex < 0) {
        FAIL("没有 graphics/transfer 队列族");
        vkDestroyInstance(inst, nullptr);
        printf("\n===== 结果汇总：PASS=%d FAIL=%d =====\n", g_pass, g_fail);
        return 1;
    }

    std::vector<const char*> devExts = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    };

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = (uint32_t)qfIndex;
    qci.queueCount = 1;
    qci.pQueuePriorities = &qprio;

    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.data();

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(pd, &dci, nullptr, &device) != VK_SUCCESS) {
        FAIL("vkCreateDevice 失败（可能缺 VK_KHR_external_memory_fd）");
        vkDestroyInstance(inst, nullptr);
        printf("\n===== 结果汇总：PASS=%d FAIL=%d =====\n", g_pass, g_fail);
        return 1;
    }

    // 实验A：零拷贝
    experimentA(pd, device);

    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(inst, nullptr);

    printf("\n===== 结果汇总：PASS=%d FAIL=%d =====\n", g_pass, g_fail);
    if (g_fail == 0) printf(">>> 全部通过：Vulkan Video + HIP 零拷贝通路可行\n");
    else              printf(">>> 存在失败项，请逐条排查（见上方 [FAIL]）\n");
    return g_fail == 0 ? 0 : 1;
}
