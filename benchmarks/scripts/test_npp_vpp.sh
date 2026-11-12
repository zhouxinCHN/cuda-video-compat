#!/bin/bash

# NPP vs VPP(VAAPI) 视频后处理能力对比测试脚本（操作矩阵）
# 作者: AI Assistant
# 日期: 2026-10-19
# 用法:
#   ./test_npp_vpp.sh --nvidia    # 只测试 NVIDIA (NPP)
#   ./test_npp_vpp.sh --amd       # 只测试 AMD (VAAPI/VPP)
#   ./test_npp_vpp.sh --all       # 测试两者（默认）
#
# 对比维度：视频后处理操作（缩放/格式转换/锐化/旋转翻转/色彩/降噪/去隔行/色调映射）
#
# 结果分类:
#   ✅ 支持    - 滤镜存在且运行成功
#   ❌ 失败    - 滤镜存在但运行失败（如格式不兼容、驱动不支持）
#   N/A       - 该技术栈无此滤镜/能力

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 解析命令行参数
TEST_MODE="all"
while [[ $# -gt 0 ]]; do
    case $1 in
        --nvidia|-n) TEST_MODE="nvidia"; shift ;;
        --amd|-a)    TEST_MODE="amd"; shift ;;
        --all)       TEST_MODE="all"; shift ;;
        --help|-h)
            echo "用法: $0 [--nvidia|--amd|--all]"
            echo "  --nvidia  只测试 NVIDIA (NPP)"
            echo "  --amd     只测试 AMD (VAAPI/VPP)"
            exit 0 ;;
        *) echo "未知选项: $1"; exit 1 ;;
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
    LOG_FILE="$HOME/npp_vpp_nvidia.log"
    RESULTS_FILE="$HOME/npp_vpp_nvidia_results.md"
    TEST_SCOPE="NVIDIA (NPP)"
elif [ "$TEST_MODE" = "amd" ]; then
    LOG_FILE="$HOME/npp_vpp_amd.log"
    RESULTS_FILE="$HOME/npp_vpp_amd_results.md"
    TEST_SCOPE="AMD (VAAPI/VPP)"
else
    LOG_FILE="$HOME/npp_vpp_test.log"
    RESULTS_FILE="$HOME/npp_vpp_results.md"
    TEST_SCOPE="NVIDIA + AMD"
fi

TEST_DIR="$HOME/test_videos/op_matrix"
mkdir -p "$TEST_DIR"
> "$LOG_FILE"

log_section "NPP vs VPP 后处理操作矩阵测试开始"
log_info "测试模式: $TEST_SCOPE"
log_info "日志: $LOG_FILE"
log_info "结果: $RESULTS_FILE"

# 测试超时（秒）
TEST_TIMEOUT=15

# 结果存储
declare -A RES_NPP
declare -A RES_VPP

# ============================================================
# 1. 环境检查
# ============================================================
log_section "1. 环境检查"

HAS_NVIDIA=0; HAS_AMD=0; HAS_VAAPI=0

if [ "$TEST_MODE" = "nvidia" ] || [ "$TEST_MODE" = "all" ]; then
    if nvidia-smi > /dev/null 2>&1; then
        GPU_NAME=$(nvidia-smi --query-gpu=name --format=csv,noheader | head -1)
        log_success "NVIDIA GPU: $GPU_NAME"
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
    log_error "FFmpeg 未安装"; exit 1
fi
log_success "FFmpeg: $(ffmpeg -version | head -1)"

# 检测滤镜是否存在
log_info "检测 NPP/VAAPI 滤镜..."
NPP_FILTERS=$(ffmpeg -hide_banner -filters 2>/dev/null | grep -oE '\b[a-z0-9_]+_npp\b' | sort -u | tr '\n' ' ')
VAAPI_FILTERS=$(ffmpeg -hide_banner -filters 2>/dev/null | grep -oE '\b[a-z0-9_]+_vaapi\b' | sort -u | tr '\n' ' ')
log_info "NPP 滤镜: $NPP_FILTERS"
log_info "VAAPI 滤镜: $VAAPI_FILTERS"

# ============================================================
# 2. 生成测试视频
# ============================================================
log_section "2. 生成测试视频"
cd "$TEST_DIR"

# 标准渐进式视频（大部分操作用）
if [ ! -f "progressive.mp4" ]; then
    if ffmpeg -y -f lavfi -i testsrc=duration=1:size=320x240:rate=30 \
        -vf "format=yuv420p" -c:v libx264 -preset ultrafast -b:v 500k \
        progressive.mp4 >/dev/null 2>&1; then
        log_success "渐进式测试视频已生成 (yuv420p)"
    else
        log_warning "渐进式视频生成失败（libx264 不可用）"
    fi
else
    log_info "渐进式视频已存在"
fi

# 隔行视频（去隔行操作用）
if [ ! -f "interlaced.mp4" ]; then
    if ffmpeg -y -f lavfi -i testsrc=duration=1:size=320x240:rate=30 \
        -vf "tinterlace=mode=interlace_top_first,format=yuv420p" -c:v libx264 -preset ultrafast -b:v 500k \
        interlaced.mp4 >/dev/null 2>&1; then
        log_success "隔行测试视频已生成"
    else
        log_warning "隔行视频生成失败"
    fi
else
    log_info "隔行视频已存在"
fi

# 10bit HDR 视频（色调映射操作用）
if [ ! -f "hdr10.mp4" ]; then
    if ffmpeg -y -f lavfi -i testsrc=duration=1:size=320x240:rate=30 \
        -vf "format=yuv420p10le" -c:v libx265 -preset ultrafast -b:v 500k \
        -x265-params "colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc" \
        hdr10.mp4 >/dev/null 2>&1; then
        log_success "10bit HDR 测试视频已生成"
    else
        log_warning "10bit HDR 视频生成失败"
    fi
else
    log_info "10bit HDR 视频已存在"
fi

# ============================================================
# 3. 操作矩阵定义
# ============================================================
# 每行: 操作名|类别|输入视频|NPP滤镜链|VAAPI滤镜链
# 滤镜链为空表示该技术栈无此能力（结果记 N/A）
log_section "3. 定义操作矩阵"

OP_MATRIX=(
    # 缩放
    "缩放|几何变换|progressive.mp4|scale_npp=w=160:h=120|scale_vaapi=w=160:h=120"
    # 格式转换
    "格式转换(→yuv444p)|色彩转换|progressive.mp4|scale_npp=format=yuv444p|scale_vaapi=format=yuv444p"
    # 锐化（NPP 需先转 yuv444p）
    "锐化|图像增强|progressive.mp4|scale_npp=format=yuv444p,sharpen_npp|sharpness_vaapi=50"
    # 顺时针旋转 90°（NPP 需先转 yuv444p）
    "顺时针旋转90°|几何变换|progressive.mp4|scale_npp=format=yuv444p,transpose_npp=dir=1|transpose_vaapi=dir=1"
    # 逆时针旋转 90°
    "逆时针旋转90°|几何变换|progressive.mp4|scale_npp=format=yuv444p,transpose_npp=dir=2|transpose_vaapi=dir=2"
    # 水平翻转（NPP transpose_npp 无 hflip，仅 VAAPI 有）
    "水平翻转|几何变换|progressive.mp4||transpose_vaapi=dir=5"
    # 垂直翻转
    "垂直翻转|几何变换|progressive.mp4||transpose_vaapi=dir=6"
    # 180°旋转
    "180°旋转|几何变换|progressive.mp4||transpose_vaapi=dir=4"
    # 色彩调整（仅 VAAPI）
    "亮度调整|色彩调整|progressive.mp4||procamp_vaapi=brightness=20"
    "饱和度调整|色彩调整|progressive.mp4||procamp_vaapi=saturation=1.5"
    # 降噪（仅 VAAPI）
    "降噪|图像增强|progressive.mp4||denoise_vaapi=32"
    # 去隔行（仅 VAAPI，需隔行视频）
    "去隔行|视频处理|interlaced.mp4||deinterlace_vaapi=mode=bob"
    # 色调映射（仅 VAAPI，需 10bit HDR）
    "色调映射(HDR→SDR)|色彩转换|hdr10.mp4||tonemap_vaapi=format=p010"
)

# ============================================================
# 4. 测试执行函数
# ============================================================

# NPP 执行：CUDA 硬件解码 → NPP 滤镜链
run_npp() {
    local input=$1 filter_chain=$2
    timeout -s KILL "$TEST_TIMEOUT" bash -c \
        "ffmpeg -y -hwaccel cuda -hwaccel_output_format cuda -c:v h264_cuvid -i '$TEST_DIR/$input' -vf '$filter_chain' -f null - </dev/null" \
        >/dev/null 2>&1 &
    local pid=$!
    wait $pid 2>/dev/null
    return $?
}

# VPP 执行：VAAPI 硬件解码（自动选解码器）→ VAAPI 滤镜链
run_vpp() {
    local input=$1 filter_chain=$2
    timeout -s KILL "$TEST_TIMEOUT" bash -c \
        "ffmpeg -y -hwaccel vaapi -hwaccel_output_format vaapi -i '$TEST_DIR/$input' -vf '$filter_chain' -f null - </dev/null" \
        >/dev/null 2>&1 &
    local pid=$!
    wait $pid 2>/dev/null
    return $?
}

# ============================================================
# 5. 执行测试
# ============================================================

if [ "$TEST_MODE" = "nvidia" ] || [ "$TEST_MODE" = "all" ]; then
    log_section "4. NPP 操作测试"
    for entry in "${OP_MATRIX[@]}"; do
        IFS='|' read -r opname category input npp_chain vpp_chain <<< "$entry"

        if [ -z "$npp_chain" ]; then
            RES_NPP["$opname"]="N/A"
            log_info "NPP 无「$opname」能力，跳过"
            continue
        fi

        if [ ! -f "$TEST_DIR/$input" ]; then
            RES_NPP["$opname"]="⚠️ 无输入"
            log_warning "$opname: 输入视频 $input 不存在"
            continue
        fi

        log_info "NPP 测试 $opname ..."
        if run_npp "$input" "$npp_chain"; then
            RES_NPP["$opname"]="✅ 支持"
            log_success "NPP $opname: 支持"
        else
            RES_NPP["$opname"]="❌ 失败"
            log_error "NPP $opname: 失败"
        fi
    done
fi

if [ "$TEST_MODE" = "amd" ] || [ "$TEST_MODE" = "all" ]; then
    log_section "5. VPP(VAAPI) 操作测试"
    if [ "$HAS_VAAPI" = "1" ]; then
        [ "$HAS_AMD" = "1" ] && export LIBVA_DRIVER_NAME=radeonsi
        for entry in "${OP_MATRIX[@]}"; do
            IFS='|' read -r opname category input npp_chain vpp_chain <<< "$entry"

            if [ -z "$vpp_chain" ]; then
                RES_VPP["$opname"]="N/A"
                log_info "VAAPI 无「$opname」能力，跳过"
                continue
            fi

            if [ ! -f "$TEST_DIR/$input" ]; then
                RES_VPP["$opname"]="⚠️ 无输入"
                log_warning "$opname: 输入视频 $input 不存在"
                continue
            fi

            log_info "VAAPI 测试 $opname ..."
            if run_vpp "$input" "$vpp_chain"; then
                RES_VPP["$opname"]="✅ 支持"
                log_success "VAAPI $opname: 支持"
            else
                RES_VPP["$opname"]="❌ 失败"
                log_error "VAAPI $opname: 失败"
            fi
        done
    else
        log_warning "跳过 VAAPI 测试（不可用）"
    fi
fi

# ============================================================
# 6. 生成报告
# ============================================================
log_section "6. 生成对比报告"

cat > "$RESULTS_FILE" << EOF
# NPP vs VPP(VAAPI) 后处理操作矩阵对比报告

生成时间: $(date '+%Y-%m-%d %H:%M:%S')
测试模式: $TEST_SCOPE

## 环境信息

- **NVIDIA GPU**: $([ $HAS_NVIDIA -eq 1 ] && echo "$GPU_NAME" || echo "未检测到")
- **AMD GPU**: $([ $HAS_AMD -eq 1 ] && echo "$AMD_GPU" || echo "未检测到")
- **VAAPI**: $([ $HAS_VAAPI -eq 1 ] && echo "可用" || echo "不可用")
- **FFmpeg**: $(ffmpeg -version | head -1)

## 操作矩阵结果

| 操作 | 类别 | NPP | VPP(VAAPI) |
|------|------|-----|-----------|
EOF

for entry in "${OP_MATRIX[@]}"; do
    IFS='|' read -r opname category input npp_chain vpp_chain <<< "$entry"
    rn="${RES_NPP[$opname]:-未测试}"
    rv="${RES_VPP[$opname]:-未测试}"
    echo "| $opname | $category | $rn | $rv |" >> "$RESULTS_FILE"
done

cat >> "$RESULTS_FILE" << EOF

## 说明

- ✅ 支持：滤镜存在且运行成功
- ❌ 失败：滤镜存在但运行失败（格式不兼容/驱动限制）
- N/A：该技术栈无此滤镜能力

## 关键结论

### NPP 能力特点
- 提供基础几何变换：缩放、旋转、锐化
- 锐化/旋转需先转换格式（nv12 → yuv444p），否则失败甚至段错误
- 无色彩调整、降噪、去隔行、色调映射等视频专用处理

### VPP(VAAPI) 能力特点
- 视频专用处理能力丰富：去隔行、降噪、色彩调整、色调映射
- 几何变换比 NPP 更全面（含水平/垂直翻转、180°旋转）
- 跨平台标准（Intel/AMD），但需对应驱动支持

### 选型建议
- **AI 推理前处理（缩放/格式转换）**: NPP 性能优，与 CUDA 零拷贝
- **视频后处理（去隔行/降噪/色彩）**: VAAPI 独占，NPP 无此能力
- **几何变换**: VAAPI 更全面（翻转/180°），NPP 仅基础旋转

EOF

log_success "对比报告已生成: $RESULTS_FILE"

# ============================================================
# 7. 控制台摘要
# ============================================================
log_section "7. 测试结果摘要"

echo ""
printf "%-22s %-12s %-14s %-14s\n" "操作" "类别" "NPP" "VPP(VAAPI)"
echo "----------------------------------------------------------------------------"
for entry in "${OP_MATRIX[@]}"; do
    IFS='|' read -r opname category input npp_chain vpp_chain <<< "$entry"
    rn="${RES_NPP[$opname]:-未测试}"
    rv="${RES_VPP[$opname]:-未测试}"
    printf "%-22s %-12s %-14s %-14s\n" "$opname" "$category" "$rn" "$rv"
done
echo ""

log_section "测试完成"
log_info "详细报告: $RESULTS_FILE"
log_info "完整日志: $LOG_FILE"
