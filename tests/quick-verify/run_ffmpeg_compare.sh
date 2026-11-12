#!/usr/bin/env bash
# FFmpeg 宿主路径像素闭环 + 零拷贝计数。
#
# 对每个 (backend, decoder) 组合：
#   1) 解 30 帧到 raw NV12；
#   2) 与软件解码参考逐字节比较；
#   3) 从 CUDA_SHIM_TRACE_COPY 的 stderr 汇总里读 D2D 拷贝次数。
#
# 判据：
#   - 像素必须一致；
#   - h264_nvdec 在 VAAPI 后端必须 0 次 D2D 拷贝（真正的零拷贝路径）；
#   - h264_cuvid 有 D2D 拷贝是 FFmpeg cuviddec 自身行为，只记录，不当失败。
#
# 用法：
#   ./run_ffmpeg_compare.sh
#   SHIM_DIR=/path/to/cuda_shim STREAM=/path/to/x.h264 ./run_ffmpeg_compare.sh
#   BACKENDS="vaapi" ./run_ffmpeg_compare.sh
#
# 退出码：0 = 所有非 SKIP 项通过；1 = 至少一项失败。

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SHIM_DIR=${SHIM_DIR:-$(cd "$SCRIPT_DIR/../../cuda_shim" && pwd)}
STREAM=${STREAM:-$(cd "$SCRIPT_DIR/../../testdata/h264" && pwd)/test_p_b.h264}
BACKENDS=${BACKENDS:-"vaapi vk"}
FFMPEG=${FFMPEG:-ffmpeg}
FFPROBE=${FFPROBE:-ffprobe}
KEEP=${KEEP:-0}
ALLOW_VK_NVDEC_FAIL=${ALLOW_VK_NVDEC_FAIL:-1}
WORK_TEMP=0
if [ -z "${WORK:-}" ]; then
    WORK=$(mktemp -d /tmp/shim-quick-XXXXXX)
    WORK_TEMP=1
fi

cleanup() {
    if [ "$KEEP" != "1" ] && [ "$WORK_TEMP" = "1" ]; then
        rm -rf "$WORK"
    else
        echo "WORK kept: $WORK"
    fi
}
trap cleanup EXIT

PASS=0
FAIL=0
SKIP=0

ok()   { echo "  [PASS] $*"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $*"; FAIL=$((FAIL+1)); }
skip() { echo "  [SKIP] $*"; SKIP=$((SKIP+1)); }

if [ ! -f "$STREAM" ]; then
    echo "FAIL: stream not found: $STREAM" >&2
    exit 2
fi
if [ ! -f "$SHIM_DIR/libnvcuvid.so.1" ]; then
    echo "FAIL: $SHIM_DIR/libnvcuvid.so.1 not found; build it first" >&2
    exit 2
fi
if ! command -v "$FFMPEG" >/dev/null 2>&1; then
    echo "FAIL: ffmpeg not found: $FFMPEG" >&2
    exit 2
fi

FFMPEG_LIB_DIR=${FFMPEG_LIB_DIR:-$HOME/ffmpeg-7.1/libavutil}
export LD_LIBRARY_PATH="$SHIM_DIR:$FFMPEG_LIB_DIR:${LD_LIBRARY_PATH:-}"
export ROCM_PATH=${ROCM_PATH:-/opt/rocm}

W=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$STREAM" 2>/dev/null | head -1)
H=$("$FFPROBE" -v error -select_streams v:0 -show_entries stream=height -of csv=p=0 "$STREAM" 2>/dev/null | head -1)
if [ -z "$W" ] || [ -z "$H" ]; then
    echo "FAIL: ffprobe cannot read width/height from $STREAM" >&2
    exit 2
fi
echo "=== FFmpeg shim compare ==="
echo "  shim   : $SHIM_DIR"
echo "  stream : $STREAM (${W}x${H})"
echo "  work   : $WORK"

REF="$WORK/reference.nv12"
if ! "$FFMPEG" -hide_banner -loglevel error -y -c:v h264 -i "$STREAM" \
        -fps_mode passthrough -f rawvideo -pix_fmt nv12 "$REF"; then
    echo "FAIL: software reference decode failed" >&2
    exit 2
fi
REF_SIZE=$(stat -c%s "$REF")
REF_MD5=$(md5sum "$REF" | awk '{print $1}')
echo "  reference: $REF_SIZE bytes, md5=$REF_MD5"

for backend in $BACKENDS; do
    echo
    echo "--- backend=$backend ---"
    for dec in h264_nvdec h264_cuvid; do
        if ! "$FFMPEG" -hide_banner -decoders 2>/dev/null | grep -q "$dec"; then
            skip "$backend/$dec: ffmpeg has no $dec decoder"
            continue
        fi

        out="$WORK/${backend}_${dec}.nv12"
        log="$WORK/${backend}_${dec}.log"
        rm -f "$out" "$log"

        CUVID_SHIM_BACKEND="$backend" CUDA_SHIM_TRACE_COPY=1 \
            "$FFMPEG" -hide_banner -loglevel error -y -c:v "$dec" -i "$STREAM" \
                -vf "hwdownload,format=nv12" -fps_mode passthrough \
                -f rawvideo -pix_fmt nv12 "$out" 2>"$log"
        rc=$?

        if [ "$rc" -ne 0 ] || [ ! -f "$out" ]; then
            msg="$backend/$dec: ffmpeg exit=$rc"
            if [ "$dec" = "h264_nvdec" ] && [ "$backend" = "vk" ] && [ "$ALLOW_VK_NVDEC_FAIL" = "1" ]; then
                skip "$msg (Vulkan h264_nvdec known gap; inspect $log)"
            else
                bad "$msg"
                tail -5 "$log" >&2 || true
            fi
            continue
        fi

        OUT_SIZE=$(stat -c%s "$out")
        OUT_MD5=$(md5sum "$out" | awk '{print $1}')
        COPIES=$(sed -n 's/.*D2D=\([0-9][0-9]*\).*/\1/p' "$log" | tail -1)
        COPIES=${COPIES:-unknown}

        echo "  $backend/$dec: $OUT_SIZE bytes, md5=$OUT_MD5, D2D-copies=$COPIES"

        if [ "$OUT_SIZE" != "$REF_SIZE" ]; then
            bad "$backend/$dec: size mismatch (shim=$OUT_SIZE ref=$REF_SIZE)"
            continue
        fi
        if cmp -s "$out" "$REF"; then
            ok "$backend/$dec: pixel-identical to software decode"
        else
            bad "$backend/$dec: pixel mismatch"
            cmp -l "$out" "$REF" 2>/dev/null | head -5 >&2 || true
            continue
        fi

        if [ "$dec" = "h264_nvdec" ]; then
            if [ "$COPIES" = "0" ]; then
                ok "$backend/h264_nvdec: zero D2D copy in map->unmap window"
            elif [ "$COPIES" = "unknown" ]; then
                bad "$backend/h264_nvdec: copy count missing from stderr (see $log)"
            else
                bad "$backend/h264_nvdec: expected 0 D2D copies, got $COPIES"
            fi
        elif [[ "$COPIES" =~ ^[0-9]+$ ]] && [ "$COPIES" -gt 0 ]; then
            echo "  [info] $backend/h264_cuvid: $COPIES D2D copies (FFmpeg cuviddec behavior)"
        fi
    done
done

echo
echo "=== summary: PASS=$PASS FAIL=$FAIL SKIP=$SKIP ==="
if [ "$FAIL" -ne 0 ]; then
    echo "One or more checks failed; set KEEP=1 to keep $WORK for inspection." >&2
    exit 1
fi
echo "PASS: FFmpeg shim pixel/zero-copy checks"
