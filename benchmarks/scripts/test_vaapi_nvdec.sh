#!/bin/bash

# VAAPI vs NVDEC 功能对比测试脚本（含格式矩阵）
# 作者: AI Assistant
# 日期: 2026-10-19
# 用法:
#   ./test_vaapi_nvdec.sh --nvidia    # 只测试 NVIDIA GPU (NVDEC)
#   ./test_vaapi_nvdec.sh --amd       # 只测试 AMD GPU (VAAPI)
#   ./test_vaapi_nvdec.sh --all       # 测试两者（默认）
#
# 测试维度:
#   1. 编码格式: H.264 / H.265(HEVC) / AV1 / VP9 / VP8 / MPEG-2
#   2. 色度采样: 4:2:0 / 4:2:2 / 4:4:4
#   3. 位深:     8bit / 10bit / 12bit
#
# 结果分类:
#   ✅ 支持      - 硬件解码成功
#   ❌ 不支持    - 硬件解码失败（如 4:2:2/4:4:4）
#   ⚠️ 无法生成  - 无可用编码器生成该格式测试视频

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# 解析命令行参数
TEST_MODE="all"

while [[ $# -gt 0 ]]; do
    case $1 in
        --nvidia|-n)
            TEST_MODE="nvidia"
            shift
            ;;
        --amd|-a)
            TEST_MODE="amd"
            shift
            ;;
        --all)
            TEST_MODE="all"
            shift
            ;;
        --help|-h)
            echo "用法: $0 [选项]"
            echo ""
            echo "选项:"
            echo "  --nvidia, -n    只测试 NVIDIA GPU (NVDEC)"
            echo "  --amd, -a       只测试 AMD GPU (VAAPI)"
            echo "  --all           测试两者（默认）"
            echo "  --help, -h      显示帮助信息"
            exit 0
            ;;
        *)
            echo "未知选项: $1"
            exit 1
            ;;
    esac
done

# 日志函数
log_info()    { echo -e "${BLUE}[INFO]${NC}    $1" | tee -a "$LOG_FILE"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $1" | tee -a "$LOG_FILE"; }
log_warning() { echo -e "${YELLOW}[WARNING]${NC} $1" | tee -a "$LOG_FILE"; }
log_error()   { echo -e "${RED}[ERROR]${NC}   $1" | tee -a "$LOG_FILE"; }
log_section() {
    echo -e "\n${BLUE}========================================${NC}" | tee -a "$LOG_FILE"
    echo -e "${BLUE}$1${NC}" | tee -a "$LOG_FILE"
    echo -e "${BLUE}========================================${NC}\n" | tee -a "$LOG_FILE"
}

# 初始化
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$TEST_MODE" = "nvidia" ]; then
    LOG_FILE="$HOME/vaapi_nvdec_nvidia.log"
    RESULTS_FILE="$HOME/vaapi_nvdec_nvidia_results.md"
    TEST_SCOPE="NVIDIA (NVDEC)"
elif [ "$TEST_MODE" = "amd" ]; then
    LOG_FILE="$HOME/vaapi_nvdec_amd.log"
    RESULTS_FILE="$HOME/vaapi_nvdec_amd_results.md"
    TEST_SCOPE="AMD (VAAPI)"
else
    LOG_FILE="$HOME/vaapi_nvdec_test.log"
    RESULTS_FILE="$HOME/vaapi_nvdec_results.md"
    TEST_SCOPE="NVIDIA + AMD"
fi

TEST_DIR="$HOME/test_videos/format_matrix"
mkdir -p "$TEST_DIR"

> "$LOG_FILE"

log_section "VAAPI vs NVDEC 格式矩阵测试开始"
log_info "测试模式: $TEST_SCOPE"
log_info "日志文件: $LOG_FILE"
log_info "结果文件: $RESULTS_FILE"
log_info "测试目录: $TEST_DIR"

# 测试超时（秒）：解码失败可能卡死，需强杀
DECODE_TIMEOUT=15

# 结果存储（二维：按测试名）
declare -A RESULTS
declare -A RESULTS_NOTE

# ============================================================
# 1. 环境检查
# ============================================================
log_section "1. 环境检查"

HAS_NVIDIA=0; HAS_AMD=0; HAS_VAAPI=0; HAS_CUDA=0

if [ "$TEST_MODE" = "nvidia" ] || [ "$TEST_MODE" = "all" ]; then
    if nvidia-smi > /dev/null 2>&1; then
        GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader | head -1)
        DRIVER_VERSION=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader | head -1)
        log_success "NVIDIA GPU: $GPU_NAME (驱动 $DRIVER_VERSION)"
        HAS_NVIDIA=1
    else
        log_error "未检测到 NVIDIA 驱动"
        [ "$TEST_MODE" = "nvidia" ] && exit 1
    fi
fi

if [ "$TEST_MODE" = "amd" ] || [ "$TEST_MODE" = "all" ]; then
    if lspci 2>/dev/null | grep -i amd | grep -qi vga; then
        AMD_GPU=$(lspci | grep -i amd | grep -i vga | head -1 | awk '{print $NF}')
        log_success "AMD GPU: $AMD_GPU"
        HAS_AMD=1
    else
        log_warning "未检测到 AMD 显卡"
        [ "$TEST_MODE" = "amd" ] && exit 1
    fi

    if vainfo > /dev/null 2>&1; then
        log_success "VAAPI 可用"
        HAS_VAAPI=1
    else
        log_warning "VAAPI 不可用"
    fi
fi

if ! command -v ffmpeg > /dev/null 2>&1; then
    log_error "FFmpeg 未安装"
    exit 1
fi
log_success "FFmpeg: $(ffmpeg -version | head -1)"

# 检测可用编码器
log_info "检测可用编码器..."
ENC_LIBX264=$(ffmpeg -hide_banner -encoders 2>/dev/null | grep -q "libx264 " && echo 1 || echo 0)
ENC_LIBX265=$(ffmpeg -hide_banner -encoders 2>/dev/null | grep -q "libx265 " && echo 1 || echo 0)
ENC_LIBVPX=$(ffmpeg -hide_banner -encoders 2>/dev/null | grep -q "libvpx-vp9 " && echo 1 || echo 0)
ENC_NVENC=$(ffmpeg -hide_banner -encoders 2>/dev/null | grep -q "h264_nvenc " && echo 1 || echo 0)
ENC_AV1_NVENC=$(ffmpeg -hide_banner -encoders 2>/dev/null | grep -q "av1_nvenc " && echo 1 || echo 0)
ENC_AV1_VAAPI=$(ffmpeg -hide_banner -encoders 2>/dev/null | grep -q "av1_vaapi " && echo 1 || echo 0)
ENC_AV1_LIBAOM=$(ffmpeg -hide_banner -encoders 2>/dev/null | grep -q "libaom-av1 " && echo 1 || echo 0)
ENC_MPEG2=$(ffmpeg -hide_banner -encoders 2>/dev/null | grep -q "mpeg2video " && echo 1 || echo 0)

log_info "libx264=$([ $ENC_LIBX264 -eq 1 ] && echo 可用 || echo 不可用)  libx265=$([ $ENC_LIBX265 -eq 1 ] && echo 可用 || echo 不可用)"
log_info "libvpx-vp9=$([ $ENC_LIBVPX -eq 1 ] && echo 可用 || echo 不可用)  mpeg2video=$([ $ENC_MPEG2 -eq 1 ] && echo 可用 || echo 不可用)"
log_info "h264_nvenc=$([ $ENC_NVENC -eq 1 ] && echo 可用 || echo 不可用)  av1_nvenc=$([ $ENC_AV1_NVENC -eq 1 ] && echo 可用 || echo 不可用)"
log_info "av1_vaapi=$([ $ENC_AV1_VAAPI -eq 1 ] && echo 可用 || echo 不可用)  libaom-av1=$([ $ENC_AV1_LIBAOM -eq 1 ] && echo 可用 || echo 不可用)"

# ============================================================
# 2. 格式矩阵定义
# ============================================================
# 每个用例: 显示名|文件名|编码器|编码参数|pix_fmt|解码器
# 解码器仅在 NVDEC 模式用；VAAPI 模式用 -hwaccel vaapi 自动选择
log_section "2. 生成格式矩阵测试视频"

FORMAT_CASES=(
    # H.264 (libx264)
    "H.264 4:2:0 8bit|h264_420|libx264|-preset ultrafast|yuv420p|h264_cuvid"
    "H.264 4:2:2 8bit|h264_422|libx264|-preset ultrafast|yuv422p|h264_cuvid"
    "H.264 4:4:4 8bit|h264_444|libx264|-preset ultrafast|yuv444p|h264_cuvid"
    "H.264 4:2:0 10bit|h264_420_10|libx264|-preset ultrafast|yuv420p10le|h264_cuvid"
    # HEVC (libx265)
    "HEVC 4:2:0 8bit|h265_420|libx265|-preset ultrafast|yuv420p|hevc_cuvid"
    "HEVC 4:2:2 8bit|h265_422|libx265|-preset ultrafast|yuv422p|hevc_cuvid"
    "HEVC 4:4:4 8bit|h265_444|libx265|-preset ultrafast|yuv444p|hevc_cuvid"
    "HEVC 4:2:0 10bit|h265_420_10|libx265|-preset ultrafast|yuv420p10le|hevc_cuvid"
    "HEVC 4:2:0 12bit|h265_420_12|libx265|-preset ultrafast|yuv420p12le|hevc_cuvid"
    # AV1 (编码器回退: av1_nvenc → av1_vaapi → libaom-av1)
    "AV1 4:2:0 8bit|av1_420|av1_nvenc|-preset p4|yuv420p|av1_cuvid"
    "AV1 4:2:0 10bit|av1_420_10|av1_nvenc|-preset p4|yuv420p10le|av1_cuvid"
    # VP9 (libvpx-vp9)
    "VP9 4:2:0 8bit|vp9_420|libvpx-vp9|-deadline realtime -cpu-used 8|yuv420p|vp9_cuvid"
    "VP9 4:2:0 10bit|vp9_420_10|libvpx-vp9|-deadline realtime -cpu-used 8|yuv420p10le|vp9_cuvid"
    # MPEG-2 (内置)
    "MPEG-2 4:2:0 8bit|mpeg2_420|mpeg2video|-b:v 2M|yuv420p|mpeg2_cuvid"
    "MPEG-2 4:2:2 8bit|mpeg2_422|mpeg2video|-b:v 2M|yuv422p|mpeg2_cuvid"
)

# 生成所有测试视频（320x240 小尺寸加速，1 秒时长）
cd "$TEST_DIR"

for entry in "${FORMAT_CASES[@]}"; do
    IFS='|' read -r label fname encoder encopts pixfmt decoder <<< "$entry"

    # 根据编码器决定容器
    case "$fname" in
        vp9_*|vp8_*) ext="webm" ;;
        mpeg2_*)     ext="mpg" ;;
        *)           ext="mp4" ;;
    esac
    outfile="$TEST_DIR/$fname.$ext"

    # 判断编码器是否可用 + 确定实际编码器（AV1 支持回退）
    actual_encoder="$encoder"
    actual_encopts="$encopts"
    enc_ok=1
    case "$encoder" in
        libx264)   [ "$ENC_LIBX264" = "1" ] || enc_ok=0 ;;
        libx265)   [ "$ENC_LIBX265" = "1" ] || enc_ok=0 ;;
        libvpx-vp9) [ "$ENC_LIBVPX" = "1" ] || enc_ok=0 ;;
        mpeg2video) [ "$ENC_MPEG2" = "1" ] || enc_ok=0 ;;
        av1_nvenc)
            # AV1 回退链: av1_nvenc → av1_vaapi → libaom-av1
            if [ "$ENC_AV1_NVENC" = "1" ]; then
                actual_encoder="av1_nvenc"; actual_encopts="-preset p4"
            elif [ "$ENC_AV1_VAAPI" = "1" ]; then
                actual_encoder="av1_vaapi"; actual_encopts=""
                log_info "  av1_nvenc 不可用，回退到 av1_vaapi"
            elif [ "$ENC_AV1_LIBAOM" = "1" ]; then
                actual_encoder="libaom-av1"; actual_encopts="-cpu-used 8 -b:v 2M"
                log_info "  av1_nvenc/av1_vaapi 不可用，回退到 libaom-av1（软编，较慢）"
            else
                enc_ok=0
            fi
            ;;
    esac

    if [ "$enc_ok" = "0" ]; then
        RESULTS["$fname"]="⚠️ 无法生成"
        RESULTS_NOTE["$fname"]="缺少编码器 $encoder（无任何 AV1 编码器）"
        log_warning "跳过 $label（编码器 $encoder 不可用）"
        continue
    fi

    # 生成视频
    if [ -f "$outfile" ] && ffprobe -v error -select_streams v:0 -show_entries stream=pix_fmt -of csv=p=0 "$outfile" >/dev/null 2>&1; then
        log_info "$label 已存在，跳过生成"
    else
        rm -f "$outfile"

        # 根据编码器选择生成命令（vaapi 编码器需要 -vaapi_device + hwupload）
        # VAAPI 上传格式：10bit 用 p010，8bit 用 nv12
        case "$actual_encoder" in
            av1_vaapi|h264_vaapi|hevc_vaapi)
                case "$pixfmt" in
                    yuv420p10le) va_fmt="p010" ;;
                    yuv420p12le) va_fmt="p012" ;;
                    yuv444p)     va_fmt="yuv444p" ;;
                    yuv444p10le) va_fmt="yuv444p10le" ;;
                    *)           va_fmt="nv12" ;;
                esac
                gen_cmd="ffmpeg -y -vaapi_device /dev/dri/renderD128 \
                    -f lavfi -i testsrc=duration=1:size=320x240:rate=30 \
                    -vf 'format=$va_fmt,hwupload' \
                    -c:v $actual_encoder $actual_encopts"
                ;;
            *)
                # 通用软件/其它硬件编码器
                gen_cmd="ffmpeg -y -f lavfi -i testsrc=duration=1:size=320x240:rate=30 \
                    -vf \"format=$pixfmt\" -c:v $actual_encoder $actual_encopts"
                ;;
        esac

        if eval "$gen_cmd \"$outfile\"" >/dev/null 2>&1; then
            log_success "$label 生成成功 ($pixfmt, 用 $actual_encoder)"
        else
            RESULTS["$fname"]="⚠️ 无法生成"
            RESULTS_NOTE["$fname"]="编码器 $actual_encoder 不支持 $pixfmt"
            log_warning "$label 生成失败"
            continue
        fi
    fi
done

# ============================================================
# 3. 解码测试
# ============================================================

# NVDEC 解码测试函数（返回 0=支持, 非0=不支持）
nvdec_decode() {
    local file=$1 decoder=$2
    # timeout -s KILL 强制杀（4:2:2 等格式失败后 ffmpeg 会卡死）
    # 后台运行 + wait 抑制 bash 的 "Killed" job 通知
    timeout -s KILL "$DECODE_TIMEOUT" bash -c \
        "ffmpeg -y -c:v $decoder -i '$file' -f null - </dev/null" >/dev/null 2>&1 &
    local pid=$!
    wait $pid 2>/dev/null
    return $?
}

# VAAPI 解码测试函数
vaapi_decode() {
    local file=$1
    timeout -s KILL "$DECODE_TIMEOUT" bash -c \
        "ffmpeg -y -hwaccel vaapi -hwaccel_output_format vaapi -i '$file' -f null - </dev/null" >/dev/null 2>&1 &
    local pid=$!
    wait $pid 2>/dev/null
    return $?
}

# NVDEC 测试
if [ "$TEST_MODE" = "nvidia" ] || [ "$TEST_MODE" = "all" ]; then
    log_section "3. NVDEC 解码测试"

    for entry in "${FORMAT_CASES[@]}"; do
        IFS='|' read -r label fname encoder encopts pixfmt decoder <<< "$entry"

        # 跳过无法生成的
        [ -n "${RESULTS[$fname]}" ] && continue

        case "$fname" in
            vp9_*|vp8_*) ext="webm" ;;
            mpeg2_*)     ext="mpg" ;;
            *)           ext="mp4" ;;
        esac
        file="$TEST_DIR/$fname.$ext"

        if [ ! -f "$file" ]; then
            RESULTS["$fname"]="⚠️ 无法生成"
            continue
        fi

        log_info "测试 $label ..."
        if nvdec_decode "$file" "$decoder"; then
            RESULTS["$fname"]="✅ 支持"
            log_success "$label: 支持"
        else
            RESULTS["$fname"]="❌ 不支持"
            RESULTS_NOTE["$fname"]="NVDEC 无法解码该格式"
            log_error "$label: 不支持"
        fi
    done
fi

# VAAPI 测试
if [ "$TEST_MODE" = "amd" ] || [ "$TEST_MODE" = "all" ]; then
    log_section "4. VAAPI 解码测试"

    if [ "$HAS_VAAPI" = "1" ]; then
        if [ "$HAS_AMD" = "1" ]; then
            export LIBVA_DRIVER_NAME=radeonsi
        fi

        for entry in "${FORMAT_CASES[@]}"; do
            IFS='|' read -r label fname encoder encopts pixfmt decoder <<< "$entry"

            [ -n "${RESULTS[$fname]}" ] && continue

            case "$fname" in
                vp9_*|vp8_*) ext="webm" ;;
                mpeg2_*)     ext="mpg" ;;
                *)           ext="mp4" ;;
            esac
            file="$TEST_DIR/$fname.$ext"

            if [ ! -f "$file" ]; then
                RESULTS["$fname"]="⚠️ 无法生成"
                continue
            fi

            log_info "测试 $label ..."
            if vaapi_decode "$file"; then
                RESULTS["$fname"]="✅ 支持"
                log_success "$label: 支持"
            else
                RESULTS["$fname"]="❌ 不支持"
                RESULTS_NOTE["$fname"]="VAAPI 无法解码该格式"
                log_error "$label: 不支持"
            fi
        done
    else
        log_warning "跳过 VAAPI 测试（不可用）"
    fi
fi

# ============================================================
# 4. 生成报告
# ============================================================
log_section "5. 生成对比报告"

cat > "$RESULTS_FILE" << EOF
# VAAPI vs NVDEC 格式矩阵对比报告

生成时间: $(date '+%Y-%m-%d %H:%M:%S')
测试模式: $TEST_SCOPE

## 环境信息

- **NVIDIA GPU**: $([ $HAS_NVIDIA -eq 1 ] && echo "$GPU_NAME" || echo "未检测到")
- **AMD GPU**: $([ $HAS_AMD -eq 1 ] && echo "$AMD_GPU" || echo "未检测到")
- **VAAPI**: $([ $HAS_VAAPI -eq 1 ] && echo "可用" || echo "不可用")
- **FFmpeg**: $(ffmpeg -version | head -1)

## 格式矩阵结果

| 格式 | 色度采样 | 位深 | 结果 | 备注 |
|------|---------|------|------|------|
EOF

# 生成结果表格
for entry in "${FORMAT_CASES[@]}"; do
    IFS='|' read -r label fname encoder encopts pixfmt decoder <<< "$entry"
    res="${RESULTS[$fname]:-⚠️ 未测试}"
    note="${RESULTS_NOTE[$fname]:-}"
    # 拆分 label 为 格式/采样/位深
    fmt=$(echo "$label" | awk '{print $1}')
    chroma=$(echo "$label" | awk '{print $2}')
    depth=$(echo "$label" | awk '{print $3}')
    echo "| $fmt | $chroma | $depth | $res | $note |" >> "$RESULTS_FILE"
done

cat >> "$RESULTS_FILE" << EOF

## 说明

- ✅ 支持：硬件解码成功
- ❌ 不支持：硬件解码失败（NVDEC/VAAPI 不支持该格式）
- ⚠️ 无法生成：缺少对应编码器无法生成测试视频

## 关键结论

### 色度采样支持
- NVDEC 对 **4:2:2** 和 **4:4:4** 的 H.264/HEVC 通常不支持（CUDA_ERROR_NOT_SUPPORTED）
- VAAPI 对 4:2:2 支持情况因显卡和驱动而异

### 位深支持
- NVDEC 通常支持 8bit 和 10bit，12bit 因格式而异
- HEVC 的 10bit/12bit 支持较好

### 建议
- **最常见场景（H.264/H.265 4:2:0 8bit）**: NVDEC 和 VAAPI 都完全支持
- **专业视频（4:2:2）**: 需检查具体 GPU 型号的支持矩阵
- **高动态范围（10bit HEVC）**: NVDEC 支持良好

EOF

log_success "对比报告已生成: $RESULTS_FILE"

# ============================================================
# 5. 控制台摘要
# ============================================================
log_section "6. 测试结果摘要"

echo ""
printf "%-22s %-10s %-8s %s\n" "格式" "色度采样" "位深" "结果"
echo "----------------------------------------------------------------------"
for entry in "${FORMAT_CASES[@]}"; do
    IFS='|' read -r label fname encoder encopts pixfmt decoder <<< "$entry"
    fmt=$(echo "$label" | awk '{print $1}')
    chroma=$(echo "$label" | awk '{print $2}')
    depth=$(echo "$label" | awk '{print $3}')
    res="${RESULTS[$fname]:-⚠️ 未测试}"
    printf "%-22s %-10s %-8s %s\n" "$fmt" "$chroma" "$depth" "$res"
done
echo ""

log_section "测试完成"
log_info "详细报告: $RESULTS_FILE"
log_info "完整日志: $LOG_FILE"
