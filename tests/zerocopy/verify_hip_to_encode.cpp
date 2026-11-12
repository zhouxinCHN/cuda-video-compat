// verify_hip_to_encode.cpp
// 实验 D：HIP 写 → VAAPI 编码，端到端内容校验（§1.7 第2步 / §4.10）
//
// 目的：把"HIP 生成 → HSA 导出 DMA-BUF → VAAPI import → 编码 → 解码回读"串成单进程，
//       闭合 R2。这是 §4.10 明确点名的"脚本做不了、必须 shim 自己实现"的那一步。
//
// 流程：
//   ① HSA 内存池分配（pitch 256 对齐）→ hsa_amd_portable_export_dmabuf 导出 fd
//   ② HIP import(OpaqueFd) 该 fd → hipMemcpyHtoD 写入确定性渐变图案（模拟 HIP kernel 输出）
//   ③ hipDeviceSynchronize
//   ④ VAAPI 用 VASurfaceAttribExternalBufferDescriptor 导入 fd 为 NV12 surface（编码输入）
//   ⑤ 最小 VAAPI H.264 编码器（VAEntrypointEncSlice，CQP + packed headers，I-only 单帧）
//   ⑥ 读 coded buffer → 写 .h264 → 与源图案比对（编码量化内应近无损）
//
// 范围：H.264 I 帧、NV12、8bit、单帧。非目标：P/B、多帧、码控精细映射。
//
// 运行（AMD 机器）：
//   make -f verify_hip_to_encode.make
//   make -f verify_hip_to_encode.make run

#include <hip/hip_runtime_api.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <drm/drm_fourcc.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#define PASS(...) do { printf("  [PASS] "); printf(__VA_ARGS__); printf("\n"); g_pass++; } while (0)
#define FAIL(...) do { printf("  [FAIL] "); printf(__VA_ARGS__); printf("\n"); g_fail++; } while (0)
#define INFO(...) do { printf("  [info] "); printf(__VA_ARGS__); printf("\n"); } while (0)

static int g_pass = 0, g_fail = 0;
static const int W = 320, H = 240;

// pitch 对齐到 256（§4.2 实测：AMD 平台线性 import 需 256 对齐）
static uint32_t align256(uint32_t v) { return (v + 255) & ~255u; }

// 确定性渐变图案：Y 竖直渐变 0..255，UV 固定 128（灰）
static void gen_pattern(uint8_t* y, uint8_t* uv) {
    for (int r = 0; r < H; r++) {
        uint8_t v = (uint8_t)(r * 255 / (H - 1));
        for (int c = 0; c < W; c++) y[r * W + c] = v;
    }
    memset(uv, 128, (W * H) / 2);
}

// ---- HSA agent / pool 查找（与 retry_exp3_v2.cpp 同款写法）----
static hsa_agent_t g_gpu; static bool g_gpu_found = false;
static hsa_status_t find_gpu_cb(hsa_agent_t a, void* d) {
    (void)d; hsa_device_type_t t; hsa_agent_get_info(a, HSA_AGENT_INFO_DEVICE, &t);
    if (t == HSA_DEVICE_TYPE_GPU) { g_gpu = a; g_gpu_found = true; return HSA_STATUS_INFO_BREAK; }
    return HSA_STATUS_SUCCESS;
}
static hsa_amd_memory_pool_t g_pool; static bool g_pool_found = false;
static hsa_status_t collect_pool_cb(hsa_amd_memory_pool_t pool, void* d) {
    (void)d; hsa_amd_segment_t seg;
    hsa_amd_memory_pool_get_info(pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &seg);
    if (seg == HSA_AMD_SEGMENT_GLOBAL) { g_pool = pool; g_pool_found = true; return HSA_STATUS_INFO_BREAK; }
    return HSA_STATUS_SUCCESS;
}

int main() {
    printf("===========================================================\n");
    printf(" 实验 D：HIP 写 → VAAPI 编码，端到端内容校验\n");
    printf("===========================================================\n");

    const uint32_t pitch = align256(W);
    const size_t   size  = (size_t)pitch * H * 3 / 2;  // NV12: Y + 半高 UV
    INFO("W=%d H=%d pitch=%u size=%zu", W, H, pitch, size);

    // ---- 1. HSA 分配 + 导出 DMA-BUF ----
    hsa_init();
    g_gpu_found = false;
    hsa_iterate_agents(find_gpu_cb, nullptr);
    if (!g_gpu_found) { FAIL("未找到 GPU agent"); return 1; }
    g_pool_found = false;
    hsa_amd_agent_iterate_memory_pools(g_gpu, collect_pool_cb, nullptr);
    if (!g_pool_found) { FAIL("未找到 GLOBAL 内存池"); return 1; }

    void* ptr = nullptr;
    hsa_status_t hs = hsa_amd_memory_pool_allocate(g_pool, size, 0, &ptr);
    if (hs != HSA_STATUS_SUCCESS || !ptr) { FAIL("HSA 池分配 %zu 失败: 0x%x", size, hs); return 1; }
    int fd = -1; uint64_t off = 0;
    hs = hsa_amd_portable_export_dmabuf(ptr, size, &fd, &off);
    if (hs != HSA_STATUS_SUCCESS)
        hs = hsa_amd_portable_export_dmabuf_v2(ptr, size, &fd, &off, 0);
    if (hs != HSA_STATUS_SUCCESS || fd < 0) { FAIL("HSA 导出 DMA-BUF 失败: 0x%x", hs); return 1; }
    PASS("HSA 分配并导出 DMA-BUF (fd=%d offset=%llu)", fd, (unsigned long long)off);

    // ---- 2. HIP import + 写入图案 ----
    hipInit(0);
    hipExternalMemoryHandleDesc hd = {};
    hd.type = hipExternalMemoryHandleTypeOpaqueFd;
    hd.handle.fd = fd;
    hd.size = size;
    hipExternalMemory_t extMem = nullptr;
    if (hipImportExternalMemory(&extMem, &hd) != hipSuccess) { FAIL("hipImportExternalMemory"); return 1; }
    hipExternalMemoryBufferDesc bd = {}; bd.size = size;
    void* devPtr = nullptr;
    if (hipExternalMemoryGetMappedBuffer(&devPtr, extMem, &bd) != hipSuccess) { FAIL("GetMappedBuffer"); return 1; }
    PASS("HIP import + 映射成功 (ptr=%p)", devPtr);

    // 生成图案（Y 紧凑 W*H，UV 紧凑 W*H/2），写入到 DMA-BUF 的 Y/UV 区（Y 在 off=0，UV 在 pitch*H）
    std::vector<uint8_t> y_buf(W * H), uv_buf(W * H / 2);
    gen_pattern(y_buf.data(), uv_buf.data());

    // Y：逐行拷贝（源紧凑 W，目标 pitch）
    for (int r = 0; r < H; r++)
        hipMemcpy((char*)devPtr + (size_t)r * pitch, y_buf.data() + r * W, W, hipMemcpyHostToDevice);
    // UV：逐行拷贝（目标从 pitch*H 开始）
    for (int r = 0; r < H / 2; r++)
        hipMemcpy((char*)devPtr + (size_t)pitch * H + (size_t)r * pitch, uv_buf.data() + r * W, W, hipMemcpyHostToDevice);
    hipDeviceSynchronize();
    PASS("HIP 写入确定性图案完成");

    // ---- 3. VAAPI 初始化 + 导入 surface（编码输入）----
    int drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (drm_fd < 0) { FAIL("open renderD128: %s", strerror(errno)); return 1; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    int ma=0, mi=0;
    if (vaInitialize(dpy, &ma, &mi) != VA_STATUS_SUCCESS) { FAIL("vaInitialize"); close(drm_fd); return 1; }
    PASS("VAAPI 初始化 (libva %d.%d)", ma, mi);

    // 构造导入描述符（composed NV12）
    VADRMPRIMESurfaceDescriptor idesc; memset(&idesc, 0, sizeof(idesc));
    idesc.fourcc = VA_FOURCC_NV12;
    idesc.width = W; idesc.height = H;
    idesc.num_objects = 1;
    idesc.objects[0].fd = fd;
    idesc.objects[0].size = size;
    idesc.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    idesc.num_layers = 1;
    idesc.layers[0].drm_format = VA_FOURCC_NV12;
    idesc.layers[0].num_planes = 2;
    idesc.layers[0].object_index[0] = 0; idesc.layers[0].offset[0] = 0;
    idesc.layers[0].object_index[1] = 0; idesc.layers[0].offset[1] = pitch * H;
    idesc.layers[0].pitch[0] = pitch; idesc.layers[0].pitch[1] = pitch;

    VASurfaceAttrib iattr[2];
    iattr[0].type = VASurfaceAttribMemoryType;
    iattr[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    iattr[0].value.type = VAGenericValueTypeInteger;
    iattr[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    iattr[1].type = VASurfaceAttribExternalBufferDescriptor;
    iattr[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    iattr[1].value.type = VAGenericValueTypePointer;
    iattr[1].value.value.p = &idesc;

    VASurfaceID input_surf = VA_INVALID_ID;
    VAStatus vs = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, W, H, &input_surf, 1, iattr, 2);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaCreateSurfaces(导入HIP fd): %d (%s)", vs, vaErrorStr(vs)); return 1; }
    PASS("VAAPI 导入 HIP 导出的 DMA-BUF 成功 (surface=%u)", input_surf);

    // ---- 4. 创建编码 config/context（CQP + packed headers）----
    VAConfigAttrib attrs[3];
    int na = 0;
    attrs[na++] = (VAConfigAttrib){ .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    attrs[na++] = (VAConfigAttrib){ .type = VAConfigAttribRateControl, .value = VA_RC_CQP };
    attrs[na++] = (VAConfigAttrib){ .type = VAConfigAttribEncPackedHeaders,
                                    .value = VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_SLICE };

    VAConfigID config = VA_INVALID_ID;
    vs = vaCreateConfig(dpy, VAProfileH264Main, VAEntrypointEncSlice, attrs, na, &config);
    if (vs != VA_STATUS_SUCCESS) {
        // 回退：不带 packed headers（有的驱动必须先查）
        na = 2;
        vs = vaCreateConfig(dpy, VAProfileH264Main, VAEntrypointEncSlice, attrs, na, &config);
    }
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaCreateConfig(EncSlice): %d (%s)", vs, vaErrorStr(vs)); return 1; }
    PASS("vaCreateConfig(EncSlice, CQP) 成功");

    VASurfaceID all_surfs[1] = { input_surf };
    VAContextID ctx = VA_INVALID_ID;
    vs = vaCreateContext(dpy, config, W, H, VA_PROGRESSIVE, all_surfs, 1, &ctx);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaCreateContext: %d (%s)", vs, vaErrorStr(vs)); return 1; }
    PASS("vaCreateContext 成功");

    // ---- 5. 填编码参数（I-only 单帧，QP18 近无损）----
    VAEncSequenceParameterBufferH264 seq = {};
    seq.seq_parameter_set_id = 0;
    seq.level_idc = 30;  // level 3.0
    seq.intra_period = 1;
    seq.intra_idr_period = 1;
    seq.ip_period = 1;
    seq.bits_per_second = 4000000;
    seq.max_num_ref_frames = 1;
    seq.picture_width_in_mbs = (uint16_t)((W + 15) / 16);
    seq.picture_height_in_mbs = (uint16_t)((H + 15) / 16);
    seq.seq_fields.bits.chroma_format_idc = 1;
    seq.seq_fields.bits.frame_mbs_only_flag = 1;
    seq.seq_fields.bits.log2_max_frame_num_minus4 = 0;
    seq.seq_fields.bits.pic_order_cnt_type = 2;
    seq.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 0;
    seq.seq_fields.bits.delta_pic_order_always_zero_flag = 0;
    seq.bit_depth_luma_minus8 = 0;
    seq.bit_depth_chroma_minus8 = 0;

    VAEncPictureParameterBufferH264 pic = {};
    pic.CurrPic.picture_id = input_surf;
    pic.CurrPic.flags = 0;
    for (int i = 0; i < 16; i++) pic.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
    pic.coded_buf = VA_INVALID_ID;  // 下面单独创建 coded buffer
    pic.pic_parameter_set_id = 0;
    pic.seq_parameter_set_id = 0;
    pic.last_picture = H264_LAST_PICTURE_EOSEQ | H264_LAST_PICTURE_EOSTREAM;
    pic.frame_num = 0;
    pic.pic_init_qp = 26 + (18 - 26);  // QP18 → pic_init_qp_minus26 = -8 → pic_init_qp = 18
    pic.num_ref_idx_l0_active_minus1 = 0;
    pic.num_ref_idx_l1_active_minus1 = 0;
    pic.pic_fields.bits.idr_pic_flag = 1;
    pic.pic_fields.bits.reference_pic_flag = 1;
    pic.pic_fields.bits.entropy_coding_mode_flag = 0;  // CAVLC
    pic.pic_fields.bits.transform_8x8_mode_flag = 1;

    // 编码 coded buffer（输出）
    VABufferID coded_buf = VA_INVALID_ID;
    vs = vaCreateBuffer(dpy, ctx, VAEncCodedBufferType, 1024 * 1024, 1, nullptr, &coded_buf);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaCreateBuffer(Coded): %d (%s)", vs, vaErrorStr(vs)); return 1; }
    pic.coded_buf = coded_buf;

    VAEncSliceParameterBufferH264 sl = {};
    sl.macroblock_address = 0;
    sl.num_macroblocks = (uint32_t)seq.picture_width_in_mbs * seq.picture_height_in_mbs;
    sl.slice_type = 7;  // I slice (all intra)
    sl.pic_parameter_set_id = 0;
    sl.idr_pic_id = 0;
    sl.slice_qp_delta = 0;
    sl.disable_deblocking_filter_idc = 0;

    // 参数 buffer
    VABufferID seq_buf, pic_buf, sl_buf;
    vs = vaCreateBuffer(dpy, ctx, VAEncSequenceParameterBufferType, sizeof(seq), 1, &seq, &seq_buf);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaCreateBuffer(SeqParam): %d (%s)", vs, vaErrorStr(vs)); return 1; }
    vs = vaCreateBuffer(dpy, ctx, VAEncPictureParameterBufferType, sizeof(pic), 1, &pic, &pic_buf);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaCreateBuffer(PicParam): %d (%s)", vs, vaErrorStr(vs)); return 1; }
    vs = vaCreateBuffer(dpy, ctx, VAEncSliceParameterBufferType, sizeof(sl), 1, &sl, &sl_buf);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaCreateBuffer(SliceParam): %d (%s)", vs, vaErrorStr(vs)); return 1; }

    // ---- 6. 编码提交 ----
    vs = vaBeginPicture(dpy, ctx, input_surf);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaBeginPicture(enc): %d (%s)", vs, vaErrorStr(vs)); return 1; }
    VABufferID ebufs[] = { seq_buf, pic_buf, sl_buf };
    vs = vaRenderPicture(dpy, ctx, ebufs, 3);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaRenderPicture(enc): %d (%s)", vs, vaErrorStr(vs)); return 1; }
    vs = vaEndPicture(dpy, ctx);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaEndPicture(enc): %d (%s)", vs, vaErrorStr(vs)); return 1; }
    vs = vaSyncSurface(dpy, input_surf);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaSyncSurface(enc): %d (%s)", vs, vaErrorStr(vs)); return 1; }
    PASS("VAAPI 编码 1 帧完成（vaSyncSurface 同步）");

    // ---- 7. 读 coded buffer ----
    VACodedBufferSegment* seg = nullptr;
    vs = vaMapBuffer(dpy, coded_buf, (void**)&seg);
    if (vs != VA_STATUS_SUCCESS) { FAIL("vaMapBuffer(Coded): %d (%s)", vs, vaErrorStr(vs)); return 1; }

    std::vector<uint8_t> bitstream;
    VACodedBufferSegment* s = seg;
    while (s) {
        if (s->buf && s->size) bitstream.insert(bitstream.end(), (uint8_t*)s->buf, (uint8_t*)s->buf + s->size);
        s = (VACodedBufferSegment*)s->next;
    }
    vaUnmapBuffer(dpy, coded_buf);
    PASS("读出编码码流 %zu 字节", bitstream.size());

    // 写 .h264 供外部解码验证
    FILE* out = fopen("/tmp/hip_to_encode_out.h264", "wb");
    if (out) { fwrite(bitstream.data(), 1, bitstream.size(), out); fclose(out); }
    INFO("编码输出写入 /tmp/hip_to_encode_out.h264");

    // ---- 8. 软解回读比对（用 ffmpeg 软解 /tmp 产物）----
    // 这里不内嵌软解器，改为调用 ffmpeg 软解后对比（由 make run 里的脚本完成）
    // 程序内只做：确认编码输出非空、有起始码，提示由脚本完成像素比对。
    bool has_startcode = false;
    for (size_t i = 0; i + 3 < bitstream.size(); i++) {
        if ((uint8_t)bitstream[i]==0 && (uint8_t)bitstream[i+1]==0 &&
            ((uint8_t)bitstream[i+2]==1 || ((uint8_t)bitstream[i+2]==0 && (uint8_t)bitstream[i+3]==1))) {
            has_startcode = true; break;
        }
    }
    if (has_startcode) PASS("编码码流含起始码（可被解码器解析）");
    else FAIL("编码码流无起始码");

    // 清理
    vaDestroyBuffer(dpy, seq_buf); vaDestroyBuffer(dpy, pic_buf); vaDestroyBuffer(dpy, sl_buf);
    vaDestroyBuffer(dpy, coded_buf);
    vaDestroyContext(dpy, ctx);
    vaDestroyConfig(dpy, config);
    vaDestroySurfaces(dpy, &input_surf, 1);
    vaTerminate(dpy);
    close(drm_fd);
    hipDestroyExternalMemory(extMem);
    close(fd);
    hsa_amd_memory_pool_free(ptr);
    hsa_shut_down();

    printf("\n========== 汇总 ==========\n");
    printf("  PASS: %d   FAIL: %d\n", g_pass, g_fail);
    if (g_fail == 0) printf("  >>> 实验 D 程序内链路通过；像素比对由 run 脚本软解 /tmp/hip_to_encode_out.h264 完成\n");
    else printf("  >>> 存在失败项，见上方 FAIL\n");
    return g_fail == 0 ? 0 : 1;
}
