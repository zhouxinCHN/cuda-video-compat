#!/bin/bash

# NVDEC SDK 解码直测一键脚本（完整格式矩阵）
# 生成 codec × bit 深度 × 色度采样 的测试码流，demux 出裸流，用 nvdec_benchmark 解码，汇总 JSON
#
# 用法:
#   ./test_nvdec_sdk.sh [--quick] [--json out.json]
#     --quick  用较短时长（2秒）快速测试
#     --json   汇总结果输出文件（默认 ~/nvdec_sdk_results.json）
#
# 前置: 已编译 nvdec_benchmark（./build_nvdec_benchmark.sh）
#
# 说明:
#   - 4:2:2 用软件编码器（libx264/libx265），因 NVENC 硬件不支持 4:2:2
#   - AV1 用 -f obu 提取（NVDEC parser 认 OBU，不认 IVF）
#   - VP9 用 IVF 提取后剥离容器头，得到纯 VP9 帧

set -e

# 颜色
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
log_info()    { echo -e "${BLUE}[INFO]${NC}    $1"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
log_warning() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
log_error()   { echo -e "${RED}[ERROR]${NC}   $1"; }
log_section() { echo -e "\n${BLUE}==== $1 ====${NC}\n"; }

# 参数解析
QUICK=0
OUTJSON="$HOME/nvdec_sdk_results.json"
while [[ $# -gt 0 ]]; do
    case $1 in
        --quick) QUICK=1; shift ;;
        --json) OUTJSON="$2"; shift 2 ;;
        *) echo "未知选项: $1"; exit 1 ;;
    esac
done

DUR=$([ $QUICK -eq 1 ] && echo 2 || echo 5)
RES=$([ $QUICK -eq 1 ] && echo 1280x720 || echo 1920x1080)

log_section "NVDEC SDK 解码直测（完整矩阵）"
log_info "时长: ${DUR}s, 分辨率: $RES"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NVDEC_BIN="$SCRIPT_DIR/nvdec_benchmark"
if [ ! -x "$NVDEC_BIN" ]; then
    log_error "找不到 nvdec_benchmark，请先运行 ./build_nvdec_benchmark.sh"
    exit 1
fi

WORK="$HOME/nvdec_sdk_work"
mkdir -p "$WORK"
cd "$WORK"

# ============================================================
# 测试矩阵
# 字段: 显示名 | codec | 编码器 | pix_fmt | 容器 | demux类型 | bsf
# demux类型: annexb | mpeg2 | obu | ivf_vp9
# ============================================================
CASES=(
    # H.264
    "H264 8bit 4:2:0|h264|h264_nvenc|yuv420p|mp4|annexb|h264_mp4toannexb"
    "H264 8bit 4:2:2|h264|libx264|yuv422p|mp4|annexb|h264_mp4toannexb"
    "H264 8bit 4:4:4|h264|h264_nvenc|yuv444p|mp4|annexb|h264_mp4toannexb"
    "H264 10bit 4:2:0|h264|libx264|yuv420p10le|mp4|annexb|h264_mp4toannexb"
    # HEVC
    "HEVC 8bit 4:2:0|hevc|hevc_nvenc|yuv420p|mp4|annexb|hevc_mp4toannexb"
    "HEVC 8bit 4:2:2|hevc|libx265|yuv422p|mp4|annexb|hevc_mp4toannexb"
    "HEVC 8bit 4:4:4|hevc|hevc_nvenc|yuv444p|mp4|annexb|hevc_mp4toannexb"
    "HEVC 10bit 4:2:0|hevc|hevc_nvenc|yuv420p10le|mp4|annexb|hevc_mp4toannexb"
    "HEVC 12bit 4:2:0|hevc|hevc_nvenc|yuv420p12le|mp4|annexb|hevc_mp4toannexb"
    # AV1
    "AV1 8bit 4:2:0|av1|av1_nvenc|yuv420p|mp4|obu|"
    "AV1 10bit 4:2:0|av1|av1_nvenc|yuv420p10le|mp4|obu|"
    # VP9
    "VP9 8bit 4:2:0|vp9|libvpx-vp9|yuv420p|webm|ivf_vp9|"
    "VP9 10bit 4:2:0|vp9|libvpx-vp9|yuv420p10le|webm|ivf_vp9|"
    # MPEG-2
    "MPEG2 8bit 4:2:0|mpeg2|mpeg2video|yuv420p|mpg|mpeg2|"
    "MPEG2 8bit 4:2:2|mpeg2|mpeg2video|yuv422p|mpg|mpeg2|"
)

# 存储结果
declare -A RES_OK RES_FPS RES_FRAMES RES_ERR

# ============================================================
# 逐个测试
# ============================================================
for entry in "${CASES[@]}"; do
    IFS='|' read -r label codec encoder pixfmt container dmux bsf <<< "$entry"

    log_section "测试: $label"
    base="$(echo "$label" | tr ' :' '__')"

    # 1. 生成视频
    log_info "生成视频 ($encoder, $pixfmt)..."
    srcfile="$WORK/${base}.$container"

    # 不同编码器用不同参数
    case "$encoder" in
        libx264)     enc_args="-preset ultrafast -b:v 5M" ;;
        libx265)     enc_args="-preset ultrafast -b:v 5M" ;;
        libvpx-vp9)  enc_args="-deadline realtime -cpu-used 8 -b:v 5M" ;;
        *)           enc_args="-preset p4 -b:v 5M" ;;
    esac

    if ! ffmpeg -y -f lavfi -i testsrc=duration=$DUR:size=$RES:rate=30 \
        -vf "format=$pixfmt" -c:v $encoder $enc_args \
        "$srcfile" >/dev/null 2>&1; then
        log_warning "  生成失败（编码器不支持 $pixfmt）"
        RES_OK[$base]="gen_fail"
        continue
    fi

    # 2. demux 出裸码流
    log_info "demux 出裸码流 ($dmux)..."
    case "$dmux" in
        annexb)
            esfile="$WORK/${base}.es"
            ffmpeg -y -i "$srcfile" -c:v copy -bsf:v "$bsf" -f "$codec" "$esfile" >/dev/null 2>&1
            ;;
        mpeg2)
            esfile="$WORK/${base}.es"
            ffmpeg -y -i "$srcfile" -c:v copy -f mpeg2video "$esfile" >/dev/null 2>&1
            ;;
        obu)
            esfile="$WORK/${base}.obu"
            ffmpeg -y -i "$srcfile" -c:v copy -f obu "$esfile" >/dev/null 2>&1
            ;;
        ivf_vp9)
            # VP9: 先出 ivf，再剥离 32 字节容器头 + 每帧 12 字节帧头
            ivffile="$WORK/${base}.ivf"
            esfile="$WORK/${base}.es"
            ffmpeg -y -i "$srcfile" -c:v copy -f ivf "$ivffile" >/dev/null 2>&1
            python3 - "$ivffile" "$esfile" << 'PYEOF'
import struct, sys
with open(sys.argv[1],'rb') as f: data = f.read()
assert data[:4] == b'DKIF', 'not ivf'
off = 32; out = b''
while off < len(data):
    fsize = struct.unpack('<I', data[off:off+4])[0]
    off += 12
    out += data[off:off+fsize]
    off += fsize
open(sys.argv[2],'wb').write(out)
PYEOF
            ;;
    esac

    if [ ! -s "$esfile" ]; then
        log_warning "  demux 失败"
        RES_OK[$base]="demux_fail"
        continue
    fi

    # 3. NVDEC SDK 解码
    log_info "NVDEC SDK 解码..."
    result=$("$NVDEC_BIN" --input "$esfile" --codec "$codec" 2>/dev/null)

    frames=$(echo "$result" | python3 -c "import json,sys; print(json.load(sys.stdin)['total_frames'])" 2>/dev/null || echo 0)
    errors=$(echo "$result" | python3 -c "import json,sys; print(json.load(sys.stdin)['decode_errors'])" 2>/dev/null || echo -1)
    fps=$(echo "$result" | python3 -c "import json,sys; print(json.load(sys.stdin)['fps'])" 2>/dev/null || echo 0)
    ok=$(echo "$result" | python3 -c "import json,sys; print(json.load(sys.stdin)['ok'])" 2>/dev/null || echo False)

    RES_FRAMES[$base]=$frames
    RES_ERR[$base]=$errors
    RES_FPS[$base]=$fps
    if [ "$ok" = "True" ] && [ "$frames" -gt 0 ]; then
        RES_OK[$base]="ok"
        log_success "  ✓ 解码成功: $frames 帧, fps=$fps, 错误=$errors"
    else
        RES_OK[$base]="fail"
        log_error "  ✗ 解码失败: $frames 帧, 错误=$errors"
    fi
done

# ============================================================
# 汇总 JSON
# ============================================================
log_section "汇总结果"

{
    echo "{"
    echo "  \"framework\": \"NVDEC-SDK\","
    echo "  \"resolution\": \"$RES\","
    echo "  \"duration_sec\": $DUR,"
    echo "  \"results\": ["
    first=1
    for entry in "${CASES[@]}"; do
        IFS='|' read -r label codec encoder pixfmt container dmux bsf <<< "$entry"
        base="$(echo "$label" | tr ' :' '__')"
        codec_name=$(echo "$label" | awk '{print $1}')
        depth=$(echo "$label" | awk '{print $2}')
        chroma=$(echo "$label" | awk '{print $3}')
        st="${RES_OK[$base]:-unknown}"
        fr="${RES_FRAMES[$base]:-0}"
        er="${RES_ERR[$base]:--1}"
        fp="${RES_FPS[$base]:-0}"

        [ $first -eq 1 ] || echo ","
        first=0
        printf '    {"codec":"%s","chroma":"%s","bit_depth":"%s","status":"%s","frames":%s,"errors":%s,"fps":%s}' \
            "$codec_name" "$chroma" "$depth" "$st" "$fr" "$er" "$fp"
    done
    echo ""
    echo "  ]"
    echo "}"
} > "$OUTJSON"

log_success "结果已写入 $OUTJSON"

# ============================================================
# 控制台摘要
# ============================================================
echo ""
printf "%-10s %-8s %-8s %-12s %-8s %-8s\n" "格式" "色度" "位深" "状态" "帧数" "fps"
echo "----------------------------------------------------------------------------"
for entry in "${CASES[@]}"; do
    IFS='|' read -r label codec encoder pixfmt container dmux bsf <<< "$entry"
    base="$(echo "$label" | tr ' :' '__')"
    st="${RES_OK[$base]:-unknown}"
    fr="${RES_FRAMES[$base]:-0}"
    fp="${RES_FPS[$base]:-0}"
    chroma=$(echo "$label" | awk '{print $3}')
    depth=$(echo "$label" | awk '{print $2}')
    case "$st" in
        ok) st_disp="✅ 成功" ;;
        fail) st_disp="❌ 失败" ;;
        gen_fail) st_disp="⚠️ 无法生成" ;;
        demux_fail) st_disp="⚠️ demux失败" ;;
        *) st_disp="$st" ;;
    esac
    printf "%-10s %-8s %-8s %-12s %-8s %-8s\n" "$(echo "$label" | awk '{print $1}')" "$chroma" "$depth" "$st_disp" "$fr" "$fp"
done
echo ""

log_success "测试完成，详细结果: $OUTJSON"
