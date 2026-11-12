#!/usr/bin/env bash
# run_multires_linear.sh
# 多分辨率线性 vs tiled 解码性能与正确性
#
# 目的：验证"线性直通"在真实分辨率（1080p/4K）下仍成立——
#   1) 内容仍与软件解码一致
#   2) 性能仍与 tiled 相当（不拖累）
#
# 方法：对每个分辨率（320x240 / 1920x1080 / 3840x2160）：
#   生成 H264 测试码流
#   → 软件解码参考
#   → 默认(tiled) 解码 vs 线性(notiling) 解码，均与参考逐字节比对 + 计时
#
# 用法：./run_multires_linear.sh

set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"; cd "$DIR"
FFMPEG="${FFMPEG:-ffmpeg}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# 分辨率列表：小/中/大/超大
RESOLUTIONS=("320x240" "640x360" "1920x1080" "3840x2160")
DURATION=2

echo "=============================================================="
echo " 多分辨率线性 vs tiled 解码验证"
echo "=============================================================="

pass=0; fail=0

for RES in "${RESOLUTIONS[@]}"; do
  W="${RES%x*}"; H="${RES#*x}"
  echo ""
  echo "---------- ${RES} (${W}x${H}) ----------"

  # 生成码流
  $FFMPEG -y -f lavfi -i "testsrc2=size=${RES}:rate=30:duration=${DURATION}" \
      -c:v libx264 -preset ultrafast -x264-params "keyint=1" "$TMP/in_$W.h264" 2>/dev/null \
      || { echo "  [SKIP] 无法生成 ${RES} 码流"; continue; }

  # 软件参考（首帧 NV12）
  $FFMPEG -y -i "$TMP/in_$W.h264" -frames:v 1 -pix_fmt nv12 -f rawvideo "$TMP/ref_$W.yuv" 2>/dev/null

  # 硬解正确性
  check() {
    local opt="$1" label="$2"
    if [ -n "$opt" ]; then
      env AMD_DEBUG=$opt $FFMPEG -y -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
          -hwaccel_output_format nv12 -i "$TMP/in_$W.h264" -frames:v 1 -pix_fmt nv12 \
          -f rawvideo "$TMP/hw_${W}_$label.yuv" 2>/dev/null
    else
      $FFMPEG -y -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
          -hwaccel_output_format nv12 -i "$TMP/in_$W.h264" -frames:v 1 -pix_fmt nv12 \
          -f rawvideo "$TMP/hw_${W}_$label.yuv" 2>/dev/null
    fi
    if cmp -s "$TMP/ref_$W.yuv" "$TMP/hw_${W}_$label.yuv"; then
      echo "  [PASS] $label：与软件解码一致"
    else
      echo "  [FAIL] $label：与软件解码不一致"
      return 1
    fi
  }
  check "" "tiled" || { fail=$((fail+1)); continue; }
  check "notiling" "linear" || { fail=$((fail+1)); continue; }

  # 性能（用 stream_loop 放大到足够帧数）
  $FFMPEG -y -stream_loop 50 -i "$TMP/in_$W.h264" -c copy "$TMP/long_$W.h264" 2>/dev/null || \
    cp "$TMP/in_$W.h264" "$TMP/long_$W.h264"

  bench() {
    local opt="$1"
    local best=999999
    for i in 1 2 3; do
      local t
      if [ -n "$opt" ]; then
        t=$( { /usr/bin/time -f "%e" env AMD_DEBUG=$opt $FFMPEG -y -hwaccel vaapi \
               -hwaccel_device /dev/dri/renderD128 -hwaccel_output_format vaapi \
               -i "$TMP/long_$W.h264" -f null - >/dev/null 2>/dev/null; } 2>&1 | tail -1 )
      else
        t=$( { /usr/bin/time -f "%e" $FFMPEG -y -hwaccel vaapi \
               -hwaccel_device /dev/dri/renderD128 -hwaccel_output_format vaapi \
               -i "$TMP/long_$W.h264" -f null - >/dev/null 2>/dev/null; } 2>&1 | tail -1 )
      fi
      awk -v x="$t" -v b="$best" 'BEGIN{ if (x < b) exit 0; else exit 1 }' 2>/dev/null && best="$t"
    done
    echo "$best"
  }

  t_tiled=$(bench "")
  t_linear=$(bench "notiling")
  echo "  tiled=${t_tiled}s  linear=${t_linear}s"

  if awk -v a="$t_tiled" -v b="$t_linear" 'BEGIN{ exit !(a>0 && b>0) }' 2>/dev/null; then
    awk -v a="$t_tiled" -v b="$t_linear" -v r="$RES" 'BEGIN{
      d = (b-a)/a*100
      printf "  %s 线性耗时变化: %+.1f%%\n", r, d
      if (d > 10) { print "    => 线性明显更慢，需警惕"; exit 1 }
      else        { print "    => 线性性能可接受（<10%）"; exit 0 }
    }' >/dev/null 2>&1 && pass=$((pass+1)) || fail=$((fail+1))
  fi
done

echo ""
echo "=============================================================="
echo " 汇总：pass=$pass  fail=$fail"
echo "=============================================================="
if [ "$fail" -eq 0 ]; then
  echo " 所有分辨率下线性直通均成立（内容正确 + 性能不拖累）。"
  echo " => 生产共享 surface 采用线性直通，pitch 256 对齐。"
else
  echo " 存在失败分辨率，见上方 [FAIL]。"
fi