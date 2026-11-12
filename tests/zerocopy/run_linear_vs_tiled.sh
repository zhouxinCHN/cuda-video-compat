#!/usr/bin/env bash
# run_linear_vs_tiled.sh
# 试验：线性 vs tiled 视频 surface 的布局与性能代价
#
# 目的：决定 shim 的共享 surface 走"线性直通"还是"tiled + VPP 转换"。
# 依赖 Mesa 的 AMD_DEBUG 视频 tiling 开关，无需写代码。
#
# Mesa 有两个相关开关（radeonsi si_pipe.c）：
#   notiling       (line 71)  DBG(NO_TILING)        "Disable tiling"（全局，含纹理）
#   novideotiling  (line 132) DBG(NO_VIDEO_TILING)  "Disable tiling for video"（仅视频）
# 两者都会让视频 surface 变线性。生产环境不应依赖全局 debug 开关，
# 应主动用 VASurfaceAttribDRMFormatModifiers 限定 DRM_FORMAT_MOD_LINEAR（见文档）。
#
# 验证三件事：
#   1. 布局：R1 导出的 modifier 是否从 tiled 变成线性
#   2. 功能：线性模式下 VAAPI 解码是否仍正确（vs 软件解码）
#   3. 性能：线性 vs tiled 解码耗时差多少
#
# 用法：./run_linear_vs_tiled.sh [input.h264]

set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"; cd "$DIR"
FFMPEG="${FFMPEG:-ffmpeg}"
W=320; H=240
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

echo "=============================================================="
echo " 试验：线性 vs tiled 视频 surface"
echo "=============================================================="

# ---------- 准备码流 ----------
INPUT="${1:-}"
if [ -z "$INPUT" ]; then
  INPUT="$TMP/t.h264"
  $FFMPEG -y -f lavfi -i "testsrc2=size=${W}x${H}:rate=30:duration=2" \
      -c:v libx264 -preset ultrafast -x264-params "keyint=1" "$INPUT" 2>/dev/null
fi

# ---------- 1. 布局检查 ----------
echo ""
echo "[1] 布局：VAAPI 自建 surface 的 DRM modifier"
echo "--------------------------------------------------------------"

modifier_of() {
  # 从 retry_exp3_v2 的 R1 输出抓 modifier
  local env_prefix="$1"
  env $env_prefix ./retry_exp3_v2 2>/dev/null \
    | grep -oE 'modifier=0x[0-9a-f]+' | head -1
}

BASE_MOD=$(modifier_of "")
GLOBAL_MOD=$(modifier_of "AMD_DEBUG=notiling")
VIDEO_MOD=$(modifier_of "AMD_DEBUG=novideotiling")

echo "  默认(tiled)        : ${BASE_MOD:-<未获取>}"
echo "  AMD_DEBUG=notiling      : ${GLOBAL_MOD:-<未获取>}"
echo "  AMD_DEBUG=novideotiling : ${VIDEO_MOD:-<未获取>}"

linear_pass=""
for label in "notiling" "novideotiling"; do
  mod=$(modifier_of "AMD_DEBUG=$label")
  if [ "$mod" = "modifier=0x0" ]; then
    echo "  [PASS] AMD_DEBUG=$label 让导出 modifier 变成线性 (0x0)"
    linear_pass="$label"
  elif [ -n "$mod" ] && [ "$mod" != "$BASE_MOD" ]; then
    echo "  [WARN] AMD_DEBUG=$label 改了 modifier 但不等于 0x0：$mod"
  else
    echo "  [FAIL] AMD_DEBUG=$label 未改变 layout（modifier 仍 $mod）"
  fi
done

# 后续功能/性能用第一个有效的线性开关
LINEAR_OPT=""
for opt in "notiling" "novideotiling"; do
  if [ "$(modifier_of "AMD_DEBUG=$opt")" = "modifier=0x0" ]; then LINEAR_OPT="$opt"; break; fi
done
if [ -z "$LINEAR_OPT" ]; then
  echo ""
  echo "  !! 没有开关能产出线性视频 surface，本机无法验证线性解码路径。"
  echo "  !! 需改用主动声明 modifier 的方式（VASurfaceAttribDRMFormatModifiers）。"
  exit 0
fi
echo ""
echo "  使用线性开关: AMD_DEBUG=$LINEAR_OPT"

# ---------- 2. 功能正确性 ----------
echo ""
echo "[2] 功能：两种模式下 VAAPI 解码是否都与软件解码一致"
echo "--------------------------------------------------------------"

$FFMPEG -y -i "$INPUT" -frames:v 1 -pix_fmt nv12 -f rawvideo "$TMP/ref.yuv" 2>/dev/null

check_decode() {
  local env_prefix="$1" label="$2" out="$3"
  env $env_prefix $FFMPEG -y -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
      -hwaccel_output_format nv12 -i "$INPUT" -frames:v 1 -pix_fmt nv12 \
      -f rawvideo "$out" 2>/dev/null
  if cmp -s "$TMP/ref.yuv" "$out"; then
    echo "  [PASS] $label：与软件解码逐字节一致"
    return 0
  else
    echo "  [FAIL] $label：与软件解码不一致"
    return 1
  fi
}

check_decode "" "默认(tiled)"   "$TMP/hw_base.yuv" || true
check_decode "AMD_DEBUG=$LINEAR_OPT" "线性($LINEAR_OPT)" "$TMP/hw_linear.yuv" || true

# ---------- 3. 性能 ----------
echo ""
echo "[3] 性能：解码吞吐对比（2 秒码流，取多次平均）"
echo "--------------------------------------------------------------"

# 用足够长的码流让编码/解码时间有意义：重复输入提高帧数
if [ "$(stat -c%s "$INPUT")" -lt 100000 ]; then
  $FFMPEG -y -stream_loop 20 -i "$INPUT" -c copy "$TMP/long.h264" 2>/dev/null || cp "$INPUT" "$TMP/long.h264"
  PERF_IN="$TMP/long.h264"
else
  PERF_IN="$INPUT"
fi

bench() {
  local env_prefix="$1"
  # 跑 3 次取最好
  local best=999999
  for i in 1 2 3; do
    local t
    t=$( { /usr/bin/time -f "%e" env $env_prefix $FFMPEG -y -hwaccel vaapi \
             -hwaccel_device /dev/dri/renderD128 -hwaccel_output_format vaapi \
             -i "$PERF_IN" -f null - >/dev/null 2>/dev/null; } 2>&1 | tail -1 )
    if awk "BEGIN{exit !($t < $best)}" 2>/dev/null; then best=$t; fi
  done
  echo "$best"
}

t_base=$(bench "")
t_linear=$(bench "AMD_DEBUG=$LINEAR_OPT")

echo "  默认(tiled)     : ${t_base}s"
echo "  线性($LINEAR_OPT): ${t_linear}s"

if awk "BEGIN{exit !($t_base > 0 && $t_linear > 0)}" 2>/dev/null; then
  awk -v a="$t_base" -v b="$t_linear" 'BEGIN{
    d = (b-a)/a*100
    printf "  线性相对 tiled 的耗时变化: %+.1f%%\n", d
    if (d > 10)       print "  => 线性明显更慢（>10%），建议 tiled + VPP 转换"
    else if (d > 2)   print "  => 线性略慢（2~10%），若省一次拷贝划算则选线性"
    else              print "  => 线性性能相当（±2%），优先线性直通（少一次转换）"
  }'
fi

# ---------- 结论 ----------
echo ""
echo "=============================================================="
echo " 决策建议"
echo "=============================================================="
echo "  若 [1] PASS 且 [3] 线性不慢于 10%  → 线性直通（零转换）"
echo "  若 [1] PASS 但 [3] 线性明显更慢    → tiled 解码 + VPP 转线性"
echo "  若 [1] FAIL                        → 该 Mesa 不提供线性视频面，"
echo "                                        只能 tiled + VPP（或等驱动支持）"
echo ""
echo "  注：[2] 两者都应 PASS——若线性模式解码内容错了，无论如何不能用线性。"