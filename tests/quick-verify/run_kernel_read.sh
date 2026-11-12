#!/usr/bin/env bash
# 运行 mapped dev_ptr kernel 可访问性 + event 顺序验证。
#
# 用法：
#   ./run_kernel_read.sh
#   SHIM_DIR=/path/to/cuda_shim STREAM=/path/to/test_p_b.h264 ./run_kernel_read.sh
#   BACKENDS="vaapi" ./run_kernel_read.sh
#
# 退出码：0 = 所有后端通过；1 = 至少一个后端失败。

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SHIM_DIR=${SHIM_DIR:-$(cd "$SCRIPT_DIR/../../cuda_shim" && pwd)}
STREAM=${STREAM:-$(cd "$SCRIPT_DIR/../../testdata/h264" && pwd)/test_p_b.h264}
BIN=${BIN:-$SHIM_DIR/verify_kernel_read}
BACKENDS=${BACKENDS:-"vaapi vk"}
ALLOW_VK_FAIL=${ALLOW_VK_FAIL:-0}

PASS=0
FAIL=0
SKIP=0

echo "=== mapped dev_ptr kernel read ==="
echo "  shim  : $SHIM_DIR"
echo "  stream: $STREAM"

if [ ! -x "$BIN" ]; then
    echo "FAIL: $BIN not found; run 'make verify_kernel_read' in cuda_shim first" >&2
    exit 2
fi
if [ ! -f "$STREAM" ]; then
    echo "FAIL: stream not found: $STREAM" >&2
    exit 2
fi

FFMPEG_LIB_DIR=${FFMPEG_LIB_DIR:-$HOME/ffmpeg-7.1/libavutil}
export LD_LIBRARY_PATH="$SHIM_DIR:$FFMPEG_LIB_DIR:${LD_LIBRARY_PATH:-}"
export ROCM_PATH=${ROCM_PATH:-/opt/rocm}

for backend in $BACKENDS; do
    echo
    echo "--- backend=$backend ---"
    CUVID_SHIM_BACKEND="$backend" "$BIN" "$STREAM"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "  [PASS] $backend"
        PASS=$((PASS+1))
    elif [ "$backend" = "vk" ] && [ "$ALLOW_VK_FAIL" = "1" ]; then
        echo "  [SKIP] vk failed (ALLOW_VK_FAIL=1); inspect output above"
        SKIP=$((SKIP+1))
    else
        echo "  [FAIL] $backend (exit=$rc)"
        FAIL=$((FAIL+1))
    fi
done

echo
echo "=== summary: PASS=$PASS FAIL=$FAIL SKIP=$SKIP ==="
[ "$FAIL" -eq 0 ] || exit 1
echo "PASS: mapped dev_ptr is kernel-addressable"
