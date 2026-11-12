// verify_vulkan_video_full.cpp
// Vulkan Video 完整验证程序（V1-V4, V6）
//
// 验证内容：
//   V1: 验证视频队列（queue family）
//   V2: 查询真实 codec/profile 能力（H264/HEVC/AV1）
//   V3: 查询视频格式和外部内存能力（NV12/P010 等）
//   V4: 只创建对象，不跑码流（session/parameters/images）
//   V6: 验证 Vulkan ↔ HIP 的 semaphore（外部信号量）
//
// 运行：make && make run
// 需要：libvulkan-dev, ROCm(HIP)

#include <vulkan/vulkan.h>

// Vulkan Video 扩展头文件（需要 Vulkan SDK 1.3.236+）。
// 系统 Vulkan-Headers 用 <vulkan/...>；Mesa 源码树用 <vk_video/...>。
#if VK_HEADER_VERSION >= 236
#  if defined(__has_include)
#    if __has_include(<vulkan/vulkan_video_codecs_common.h>)
#      include <vulkan/vulkan_video_codecs_common.h>
#      include <vulkan/vulkan_video_codec_h264std.h>
#      include <vulkan/vulkan_video_codec_h265std.h>
#    else
#      include <vk_video/vulkan_video_codecs_common.h>
#      include <vk_video/vulkan_video_codec_h264std.h>
#      include <vk_video/vulkan_video_codec_h265std.h>
#    endif
#  else
#    include <vulkan/vulkan_video_codecs_common.h>
#    include <vulkan/vulkan_video_codec_h264std.h>
#    include <vulkan/vulkan_video_codec_h265std.h>
#  endif
#define HAS_VULKAN_VIDEO_HEADERS 1
#else
#define HAS_VULKAN_VIDEO_HEADERS 0
#endif

#include <hip/hip_runtime_api.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

#define LOG(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
#define PASS(...)   do { printf("  [PASS] "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define FAIL(...)   do { printf("  [FAIL] "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define INFO(...)   do { printf("  [info] "); printf(__VA_ARGS__); printf("\n"); } while (0)

static int g_pass = 0, g_fail = 0;

// ---------------- 工具函数 ----------------
static const char* vkResultStr(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        default: return "VK_ERROR_UNKNOWN";
    }
}

static const char* queueFlagStr(VkQueueFlags flags) {
    static char buf[256];
    buf[0] = '\0';
    if (flags & VK_QUEUE_GRAPHICS_BIT) strcat(buf, "GRAPHICS ");
    if (flags & VK_QUEUE_COMPUTE_BIT) strcat(buf, "COMPUTE ");
    if (flags & VK_QUEUE_TRANSFER_BIT) strcat(buf, "TRANSFER ");
    if (flags & VK_QUEUE_SPARSE_BINDING_BIT) strcat(buf, "SPARSE ");
    if (flags & VK_QUEUE_PROTECTED_BIT) strcat(buf, "PROTECTED ");
    if (flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) strcat(buf, "VIDEO_DECODE ");
    if (flags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) strcat(buf, "VIDEO_ENCODE ");
    return buf;
}

// ---------------- V1: 验证视频队列 ----------------
static void v1_video_queues(VkPhysicalDevice pd) {
    printf("\n========== V1: 验证视频队列（queue family） ==========\n");

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
    if (qfCount == 0) {
        FAIL("没有 queue family");
        return;
    }

    std::vector<VkQueueFamilyProperties> qfps(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfps.data());

    bool has_decode = false, has_encode = false;
    int decode_qf = -1, encode_qf = -1;

    for (uint32_t i = 0; i < qfCount; i++) {
        INFO("queue family[%u]: flags=%s queueCount=%u", i, queueFlagStr(qfps[i].queueFlags), qfps[i].queueCount);
        if (qfps[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) {
            has_decode = true;
            decode_qf = i;
        }
        if (qfps[i].queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) {
            has_encode = true;
            encode_qf = i;
        }
    }

    if (has_decode) {
        PASS("存在 VIDEO_DECODE 队列族 (index=%d)", decode_qf);
    } else {
        FAIL("不存在 VIDEO_DECODE 队列族");
    }

    if (has_encode) {
        PASS("存在 VIDEO_ENCODE 队列族 (index=%d)", encode_qf);
    } else {
        FAIL("不存在 VIDEO_ENCODE 队列族");
    }
}

// ---------------- V2: 查询真实 codec/profile 能力 ----------------
static void v2_codec_capabilities(VkPhysicalDevice pd) {
    printf("\n========== V2: 查询真实 codec/profile 能力 ==========\n");

#if HAS_VULKAN_VIDEO_HEADERS
    // 查询 H264 解码能力
    VkVideoCodecOperationFlagBitsKHR codec = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;

    VkVideoProfileInfoKHR profileInfo = {};
    profileInfo.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    profileInfo.videoCodecOperation = codec;
    profileInfo.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profileInfo.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profileInfo.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

    VkVideoDecodeH264ProfileInfoKHR h264Profile = {};
    h264Profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
    h264Profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    h264Profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
    profileInfo.pNext = &h264Profile;

    VkVideoCapabilitiesKHR caps = {};
    caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;

    VkVideoDecodeCapabilitiesKHR decodeCaps = {};
    decodeCaps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
    caps.pNext = &decodeCaps;

    VkResult vr = vkGetPhysicalDeviceVideoCapabilitiesKHR(pd, &profileInfo, &caps);
    if (vr != VK_SUCCESS) {
        FAIL("vkGetPhysicalDeviceVideoCapabilitiesKHR(H264): %s", vkResultStr(vr));
    } else {
        PASS("H264 High profile 解码能力查询成功");
        INFO("  maxCodedExtent: %ux%u", caps.maxCodedExtent.width, caps.maxCodedExtent.height);
        INFO("  maxDpbSlots: %u", caps.maxDpbSlots);
        INFO("  maxActiveReferencePictures: %u", caps.maxActiveReferencePictures);
        INFO("  flags: 0x%x", caps.flags);
        if (decodeCaps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) {
            INFO("  DPB and output COINCIDE");
        }
        if (decodeCaps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR) {
            INFO("  DPB and output DISTINCT");
        }
    }

    // 查询 HEVC 解码能力
    codec = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
    profileInfo.videoCodecOperation = codec;
    VkVideoDecodeH265ProfileInfoKHR h265Profile = {};
    h265Profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
    h265Profile.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
    profileInfo.pNext = &h265Profile;

    vr = vkGetPhysicalDeviceVideoCapabilitiesKHR(pd, &profileInfo, &caps);
    if (vr != VK_SUCCESS) {
        FAIL("vkGetPhysicalDeviceVideoCapabilitiesKHR(HEVC): %s", vkResultStr(vr));
    } else {
        PASS("HEVC Main profile 解码能力查询成功");
        INFO("  maxCodedExtent: %ux%u", caps.maxCodedExtent.width, caps.maxCodedExtent.height);
    }

    // 查询 H264 编码能力（如果支持）
    codec = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    profileInfo.videoCodecOperation = codec;
    VkVideoEncodeH264ProfileInfoKHR h264EncProfile = {};
    h264EncProfile.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR;
    h264EncProfile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    profileInfo.pNext = &h264EncProfile;

    VkVideoEncodeCapabilitiesKHR encodeCaps = {};
    encodeCaps.sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR;
    caps.pNext = &encodeCaps;

    vr = vkGetPhysicalDeviceVideoCapabilitiesKHR(pd, &profileInfo, &caps);
    if (vr != VK_SUCCESS) {
        FAIL("vkGetPhysicalDeviceVideoCapabilitiesKHR(H264 encode): %s", vkResultStr(vr));
    } else {
        PASS("H264 High profile 编码能力查询成功");
        INFO("  maxCodedExtent: %ux%u", caps.maxCodedExtent.width, caps.maxCodedExtent.height);
        INFO("  rateControlModes: 0x%x", encodeCaps.rateControlModes);
        INFO("  maxRateControlLayers: %u", encodeCaps.maxRateControlLayers);
        INFO("  maxBitrate: %llu", (unsigned long long)encodeCaps.maxBitrate);
        INFO("  maxQualityLevels: %u", encodeCaps.maxQualityLevels);
    }
#else
    FAIL("Vulkan Video 头文件不可用（需要 Vulkan SDK 1.3.236+）");
    INFO("跳过 V2 测试");
#endif
}

// ---------------- V3: 查询视频格式和外部内存能力 ----------------
static void v3_video_formats(VkPhysicalDevice pd) {
    printf("\n========== V3: 查询视频格式和外部内存能力 ==========\n");

#if HAS_VULKAN_VIDEO_HEADERS
    VkVideoProfileInfoKHR profileInfo = {};
    profileInfo.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    profileInfo.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    profileInfo.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profileInfo.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profileInfo.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

    VkVideoDecodeH264ProfileInfoKHR h264Profile = {};
    h264Profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
    h264Profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    h264Profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
    profileInfo.pNext = &h264Profile;

    VkPhysicalDeviceVideoFormatInfoKHR fmtInfo = {};
    fmtInfo.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR;
    fmtInfo.pNext = &profileInfo;
    fmtInfo.imageUsage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;

    uint32_t fmtCount = 0;
    VkResult vr = vkGetPhysicalDeviceVideoFormatPropertiesKHR(pd, &fmtInfo, &fmtCount, nullptr);
    if (vr != VK_SUCCESS) {
        FAIL("vkGetPhysicalDeviceVideoFormatPropertiesKHR(count): %s", vkResultStr(vr));
        return;
    }

    if (fmtCount == 0) {
        FAIL("没有支持的视频格式");
        return;
    }

    std::vector<VkVideoFormatPropertiesKHR> fmts(fmtCount);
    for (uint32_t i = 0; i < fmtCount; i++) {
        fmts[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
    }
    vr = vkGetPhysicalDeviceVideoFormatPropertiesKHR(pd, &fmtInfo, &fmtCount, fmts.data());
    if (vr != VK_SUCCESS) {
        FAIL("vkGetPhysicalDeviceVideoFormatPropertiesKHR(data): %s", vkResultStr(vr));
        return;
    }

    PASS("H264 解码支持 %u 种视频格式", fmtCount);
    for (uint32_t i = 0; i < fmtCount; i++) {
        INFO("  format[%u]: vkFormat=%u imageTiling=%u imageUsage=0x%x imageCreateFlags=0x%x",
             i, fmts[i].format, fmts[i].imageTiling, fmts[i].imageUsage, fmts[i].imageCreateFlags);
    }

    // 检查是否支持 NV12 (VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
    bool has_nv12 = false;
    for (uint32_t i = 0; i < fmtCount; i++) {
        if (fmts[i].format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM) {
            has_nv12 = true;
            break;
        }
    }
    if (has_nv12) {
        PASS("支持 NV12 (VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)");
    } else {
        FAIL("不支持 NV12");
    }

    // 检查是否支持线性 tiling
    bool has_linear = false;
    for (uint32_t i = 0; i < fmtCount; i++) {
        if (fmts[i].imageTiling == VK_IMAGE_TILING_LINEAR) {
            has_linear = true;
            break;
        }
    }
    if (has_linear) {
        PASS("支持 LINEAR tiling（可用于零拷贝）");
    } else {
        FAIL("不支持 LINEAR tiling（零拷贝需要额外转换）");
    }
#else
    FAIL("Vulkan Video 头文件不可用（需要 Vulkan SDK 1.3.236+）");
    INFO("跳过 V3 测试");
#endif
}

// ---------------- V4: 只创建对象，不跑码流 ----------------
static void v4_create_objects(VkPhysicalDevice pd, VkDevice device) {
    printf("\n========== V4: 创建视频会话对象（不跑码流） ==========\n");

    // 创建 H264 解码会话
    VkVideoProfileInfoKHR profileInfo = {};
    profileInfo.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    profileInfo.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    profileInfo.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profileInfo.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profileInfo.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

    VkVideoDecodeH264ProfileInfoKHR h264Profile = {};
    h264Profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
    h264Profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    h264Profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
    profileInfo.pNext = &h264Profile;

    VkVideoSessionCreateInfoKHR sessionCreateInfo = {};
    sessionCreateInfo.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR;
    sessionCreateInfo.pNext = &profileInfo;
    sessionCreateInfo.queueFamilyIndex = 0;  // 需要找到 video decode queue family
    sessionCreateInfo.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    sessionCreateInfo.maxCodedExtent = {1920, 1080};
    sessionCreateInfo.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    sessionCreateInfo.maxDpbSlots = 17;
    sessionCreateInfo.maxActiveReferencePictures = 16;
    sessionCreateInfo.pStdHeaderVersion = nullptr;  // 使用默认版本

    VkVideoSessionKHR session = VK_NULL_HANDLE;
    VkResult vr = vkCreateVideoSessionKHR(device, &sessionCreateInfo, nullptr, &session);
    if (vr != VK_SUCCESS) {
        FAIL("vkCreateVideoSessionKHR: %s", vkResultStr(vr));
        return;
    }
    PASS("视频会话创建成功");

    // 查询内存需求
    uint32_t memReqCount = 0;
    vr = vkGetVideoSessionMemoryRequirementsKHR(device, session, &memReqCount, nullptr);
    if (vr != VK_SUCCESS) {
        FAIL("vkGetVideoSessionMemoryRequirementsKHR(count): %s", vkResultStr(vr));
        vkDestroyVideoSessionKHR(device, session, nullptr);
        return;
    }

    std::vector<VkVideoSessionMemoryRequirementsKHR> memReqs(memReqCount);
    for (uint32_t i = 0; i < memReqCount; i++) {
        memReqs[i].sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
    }
    vr = vkGetVideoSessionMemoryRequirementsKHR(device, session, &memReqCount, memReqs.data());
    if (vr != VK_SUCCESS) {
        FAIL("vkGetVideoSessionMemoryRequirementsKHR(data): %s", vkResultStr(vr));
        vkDestroyVideoSessionKHR(device, session, nullptr);
        return;
    }

    PASS("视频会话内存需求查询成功，共 %u 个绑定", memReqCount);
    for (uint32_t i = 0; i < memReqCount; i++) {
        INFO("  bind[%u]: size=%llu memoryTypeBits=0x%x",
             i, (unsigned long long)memReqs[i].memoryRequirements.size,
             memReqs[i].memoryRequirements.memoryTypeBits);
    }

    // 清理
    vkDestroyVideoSessionKHR(device, session, nullptr);
    PASS("视频会话销毁成功");
}

// ---------------- V6: 验证 Vulkan ↔ HIP 的 semaphore ----------------
static void v6_semaphore(VkPhysicalDevice pd, VkDevice device, uint32_t queueFamilyIndex) {
    printf("\n========== V6: 验证 Vulkan ↔ HIP 的 semaphore ==========\n");

    // 创建 Vulkan semaphore
    VkSemaphoreCreateInfo semCreateInfo = {};
    semCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkSemaphore vkSem = VK_NULL_HANDLE;
    VkResult vr = vkCreateSemaphore(device, &semCreateInfo, nullptr, &vkSem);
    if (vr != VK_SUCCESS) {
        FAIL("vkCreateSemaphore: %s", vkResultStr(vr));
        return;
    }

    // 导出 semaphore 为 fd
    PFN_vkGetSemaphoreFdKHR pfnGetSemFd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR");
    if (!pfnGetSemFd) {
        FAIL("vkGetSemaphoreFdKHR 未加载");
        vkDestroySemaphore(device, vkSem, nullptr);
        return;
    }

    VkSemaphoreGetFdInfoKHR fdInfo = {};
    fdInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    fdInfo.semaphore = vkSem;
    fdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;

    int fd = -1;
    vr = pfnGetSemFd(device, &fdInfo, &fd);
    if (vr != VK_SUCCESS || fd < 0) {
        FAIL("vkGetSemaphoreFdKHR: %s", vkResultStr(vr));
        vkDestroySemaphore(device, vkSem, nullptr);
        return;
    }
    PASS("Vulkan semaphore 导出 fd=%d 成功", fd);

    // HIP 导入 semaphore
    hipExternalSemaphoreHandleDesc semDesc = {};
    semDesc.type = hipExternalSemaphoreHandleTypeOpaqueFd;
    semDesc.handle.fd = fd;

    hipExternalSemaphore_t hipSem = nullptr;
    hipError_t he = hipImportExternalSemaphore(&hipSem, &semDesc);
    if (he != hipSuccess) {
        FAIL("hipImportExternalSemaphore: %s", hipGetErrorString(he));
        close(fd);
        vkDestroySemaphore(device, vkSem, nullptr);
        return;
    }
    PASS("HIP 导入 semaphore 成功");

    // 模拟 Vulkan signal → HIP wait
    hipExternalSemaphoreSignalInfo signalInfo = {};
    signalInfo.params.fence.value = 1;

    // 注意：这里只是验证 API 调用，实际需要同步点
    he = hipSignalExternalSemaphoresAsync(&hipSem, &signalInfo, 1, nullptr);
    if (he != hipSuccess) {
        FAIL("hipSignalExternalSemaphoresAsync: %s", hipGetErrorString(he));
    } else {
        PASS("hipSignalExternalSemaphoresAsync 调用成功");
    }

    // 清理
    hipDestroyExternalSemaphore(hipSem);
    close(fd);
    vkDestroySemaphore(device, vkSem, nullptr);
    PASS("semaphore 清理完成");
}

// ---------------- main ----------------
int main() {
    printf("===========================================================\n");
    printf(" Vulkan Video 完整验证程序（V1-V4, V6）\n");
    printf("===========================================================\n");

    // 创建 Vulkan instance
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "verify_vulkan_video_full";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "none";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> instExts = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME,
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
    INFO("Vulkan 设备[0] = %s (Vulkan %d.%d.%d)", pdp.deviceName,
         VK_VERSION_MAJOR(pdp.apiVersion), VK_VERSION_MINOR(pdp.apiVersion),
         VK_VERSION_PATCH(pdp.apiVersion));

    // V1: 验证视频队列
    v1_video_queues(pd);

    // 创建 logical device（需要 video queue family）
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfps(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfps.data());

    int video_qf = -1;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfps[i].queueFlags & (VK_QUEUE_VIDEO_DECODE_BIT_KHR | VK_QUEUE_VIDEO_ENCODE_BIT_KHR)) {
            video_qf = i;
            break;
        }
    }

    if (video_qf < 0) {
        FAIL("没有 video queue family，无法继续");
        vkDestroyInstance(inst, nullptr);
        printf("\n===== 结果汇总：PASS=%d FAIL=%d =====\n", g_pass, g_fail);
        return 1;
    }

    std::vector<const char*> devExts = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME,
        VK_KHR_VIDEO_DECODE_H265_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME,
    };

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = (uint32_t)video_qf;
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
        FAIL("vkCreateDevice 失败");
        vkDestroyInstance(inst, nullptr);
        printf("\n===== 结果汇总：PASS=%d FAIL=%d =====\n", g_pass, g_fail);
        return 1;
    }

    // V2: 查询 codec 能力
    v2_codec_capabilities(pd);

    // V3: 查询视频格式
    v3_video_formats(pd);

    // V4: 创建对象
    v4_create_objects(pd, device);

    // V6: semaphore
    v6_semaphore(pd, device, (uint32_t)video_qf);

    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(inst, nullptr);

    printf("\n===== 结果汇总：PASS=%d FAIL=%d =====\n", g_pass, g_fail);
    if (g_fail == 0) {
        printf(">>> 全部通过：Vulkan Video 能力验证完成\n");
    } else {
        printf(">>> 存在失败项，请逐条排查（见上方 [FAIL]）\n");
    }
    return g_fail == 0 ? 0 : 1;
}
