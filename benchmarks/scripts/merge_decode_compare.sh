#!/bin/bash

# merge_decode_compare.sh
# 合并 NVDEC 和 VAAPI 的解码格式矩阵结果，生成对比报告
#
# 用法:
#   ./merge_decode_compare.sh [--nvdec file.md] [--vaapi file.md] [--out out.md]
#
# 默认读取:
#   NVDEC: ~/vaapi_nvdec_nvidia_results.md
#   VAAPI: ~/vaapi_nvdec_amd_results.md
#
# 说明: 两个结果文件需是 test_vaapi_nvdec.sh 生成的 Markdown 格式
#       （AMD 侧结果需先在 AMD 机器上跑 ./test_vaapi_nvdec.sh --amd 生成后拷贝过来）

set -e

NVDEC_FILE="$HOME/vaapi_nvdec_nvidia_results.md"
VAAPI_FILE="$HOME/vaapi_nvdec_amd_results.md"
OUT_FILE="$HOME/nvdec_vaapi_decode_compare.md"

while [[ $# -gt 0 ]]; do
    case $1 in
        --nvdec) NVDEC_FILE="$2"; shift 2 ;;
        --vaapi) VAAPI_FILE="$2"; shift 2 ;;
        --out) OUT_FILE="$2"; shift 2 ;;
        --help|-h)
            echo "用法: $0 [--nvdec nvdec.md] [--vaapi vaapi.md] [--out out.md]"
            exit 0 ;;
        *) echo "未知选项: $1"; exit 1 ;;
    esac
done

# 检查文件
if [ ! -f "$NVDEC_FILE" ]; then
    echo "错误: 找不到 NVDEC 结果文件 $NVDEC_FILE"
    echo "请先在 NVIDIA 机器跑: ./test_vaapi_nvdec.sh --nvidia"
    exit 1
fi
if [ ! -f "$VAAPI_FILE" ]; then
    echo "错误: 找不到 VAAPI 结果文件 $VAAPI_FILE"
    echo "请先在 AMD 机器跑: ./test_vaapi_nvdec.sh --amd，并把结果拷贝到 $VAAPI_FILE"
    exit 1
fi

# 提取结果表格（用 awk 解析 markdown 表格）
# 每行格式: | 格式 | 色度采样 | 位深 | 结果 | 备注 |
extract() {
    # 输入: 文件路径；输出: "格式|色度|位深|结果" 每行一条
    awk -F'|' '
        /^\| *(H\.264|HEVC|AV1|VP9|MPEG-2) / {
            fmt=$2; chroma=$3; depth=$4; res=$5;
            gsub(/^[ \t]+|[ \t]+$/, "", fmt);
            gsub(/^[ \t]+|[ \t]+$/, "", chroma);
            gsub(/^[ \t]+|[ \t]+$/, "", depth);
            gsub(/^[ \t]+|[ \t]+$/, "", res);
            # 统一格式名
            gsub(/H\.264/, "H.264", fmt);
            gsub(/MPEG-2/, "MPEG-2", fmt);
            print fmt "|" chroma "|" depth "|" res;
        }
    ' "$1"
}

# 读取到关联数组
declare -A NVDEC_RES VAAPI_RES

while IFS='|' read -r fmt chroma depth res; do
    key="$fmt|$chroma|$depth"
    NVDEC_RES["$key"]="$res"
done < <(extract "$NVDEC_FILE")

while IFS='|' read -r fmt chroma depth res; do
    key="$fmt|$chroma|$depth"
    VAAPI_RES["$key"]="$res"
done < <(extract "$VAAPI_FILE")

# 生成对比报告
{
    echo "# NVDEC vs VAAPI 解码格式矩阵对比"
    echo ""
    echo "生成时间: $(date '+%Y-%m-%d %H:%M:%S')"
    echo ""
    echo "- NVDEC 结果: $NVDEC_FILE"
    echo "- VAAPI 结果: $VAAPI_FILE"
    echo ""
    echo "## 对比矩阵"
    echo ""
    echo "| 格式 | 色度采样 | 位深 | NVDEC | VAAPI | 差异 |"
    echo "|------|---------|------|:-----:|:-----:|------|"

    # 合并所有 key（按格式排序）
    {
        for k in "${!NVDEC_RES[@]}"; do echo "$k"; done
        for k in "${!VAAPI_RES[@]}"; do echo "$k"; done
    } | sort -u | while IFS='|' read -r fmt chroma depth; do
        key="$fmt|$chroma|$depth"
        nv="${NVDEC_RES[$key]:-未测}"
        va="${VAAPI_RES[$key]:-未测}"

        # 归一化状态
        nv_st=$(echo "$nv" | grep -oE '支持|不支持|无法生成' | head -1)
        va_st=$(echo "$va" | grep -oE '支持|不支持|无法生成' | head -1)

        # 判断差异
        if [ "$nv_st" = "$va_st" ]; then
            diff="一致"
        elif [ "$nv_st" = "支持" ] && [ "$va_st" = "不支持" ]; then
            diff="**NVDEC 领先**"
        elif [ "$nv_st" = "不支持" ] && [ "$va_st" = "支持" ]; then
            diff="**VAAPI 领先**"
        elif [ "$nv_st" = "支持" ] && [ "$va_st" = "无法生成" ]; then
            diff="⚠️ VAAPI 待补测"
        elif [ "$nv_st" = "无法生成" ] && [ "$va_st" = "支持" ]; then
            diff="⚠️ NVDEC 待补测"
        else
            diff="待确认"
        fi

        # 显示状态符号
        nv_disp=$(echo "$nv" | grep -oE '[✅❌⚠️]' | head -1)
        va_disp=$(echo "$va" | grep -oE '[✅❌⚠️]' | head -1)
        [ -z "$nv_disp" ] && nv_disp="?"
        [ -z "$va_disp" ] && va_disp="?"

        echo "| $fmt | $chroma | $depth | $nv_disp $nv_st | $va_disp $va_st | $diff |"
    done

    echo ""
    echo "## 统计"
    echo ""
    # 统计领先项
    nv_lead=0; va_lead=0; same=0
    for k in "${!NVDEC_RES[@]}"; do
        va="${VAAPI_RES[$k]:-}"
        nv_st=$(echo "${NVDEC_RES[$k]}" | grep -oE '支持|不支持|无法生成' | head -1)
        va_st=$(echo "$va" | grep -oE '支持|不支持|无法生成' | head -1)
        [ -z "$va_st" ] && continue
        if [ "$nv_st" = "$va_st" ]; then same=$((same+1));
        elif [ "$nv_st" = "支持" ] && [ "$va_st" = "不支持" ]; then nv_lead=$((nv_lead+1));
        elif [ "$nv_st" = "不支持" ] && [ "$va_st" = "支持" ]; then va_lead=$((va_lead+1));
        fi
    done

    echo "- 一致: $same 项"
    echo "- NVDEC 领先: $nv_lead 项"
    echo "- VAAPI 领先: $va_lead 项"
    echo ""
} > "$OUT_FILE"

echo "✓ 合并报告已生成: $OUT_FILE"
echo ""
cat "$OUT_FILE"
