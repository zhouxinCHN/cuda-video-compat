#!/usr/bin/env bash
# cuda_shim upload/download regression test.
#
# FFmpeg's hwupload_cuda issues cuMemcpy2DAsync from pageable host memory and
# then recycles the source frame. CUDA treats such a copy as completed when the
# call returns; the shim previously forwarded it to hipMemcpyParam2DAsync,
# which could still read the host buffer after the caller reused it. The
# symptom was deterministic cross-frame corruption in rawvideo input
# (frame 1 of a 5-frame yuv420p clip had chroma written into luma).
#
# This test needs no NPP golden: a software conversion renders the exact
# reference for the same source frames, then the CUDA upload/download round
# trip must reproduce it byte-for-byte, repeatedly.
#
# Usage:
#   FFMPEG_BUILD_DIR=/path/to/ffmpeg FFMPEG_LIBS=1 \
#     ./verify_hwupload_roundtrip.sh
#
# Environment:
#   FFMPEG            ffmpeg executable (default $FFMPEG_BUILD_DIR/ffmpeg or PATH)
#   FFMPEG_BUILD_DIR  FFmpeg build tree; its library dirs are prepended to
#                     LD_LIBRARY_PATH
#   CUDA_SHIM_DIR     cuda_shim directory holding libcuda.so.1
#   OUT_DIR           scratch/output directory (default ./hwupload-roundtrip)
#   SRC_WIDTH/SRC_HEIGHT/SRC_FRAMES  fixture geometry (default 320/240/5)
#   FORMATS           pixel formats to test (default "yuv420p nv12")
#   REPEAT            runs per format (default 3)
#   TIMEOUT_SECONDS   per-run timeout (default 60)
#
# Exit code: 0 = every run matches the software reference; 1 = mismatch or
# failure; 2 = environment/argument failure.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
CUDA_SHIM_DIR=${CUDA_SHIM_DIR:-"$REPO_ROOT/cuda_shim"}
FFMPEG_BUILD_DIR=${FFMPEG_BUILD_DIR:-}
FFMPEG=${FFMPEG:-${FFMPEG_BUILD_DIR:+$FFMPEG_BUILD_DIR/ffmpeg}}
FFMPEG=${FFMPEG:-ffmpeg}
OUT_DIR=${OUT_DIR:-"$PWD/hwupload-roundtrip"}
SRC_WIDTH=${SRC_WIDTH:-320}
SRC_HEIGHT=${SRC_HEIGHT:-240}
SRC_FRAMES=${SRC_FRAMES:-5}
RATE=${RATE:-5}
FORMATS=${FORMATS:-"yuv420p nv12"}
REPEAT=${REPEAT:-3}
TIMEOUT_SECONDS=${TIMEOUT_SECONDS:-60}
ROCM_DIR=${ROCM_DIR:-$(ls -d /opt/rocm-* 2>/dev/null | head -1)}
ROCM_DIR=${ROCM_DIR:-/opt/rocm}

die() {
    echo "FAIL: $*" >&2
    exit 2
}

command -v "$FFMPEG" >/dev/null 2>&1 || [ -x "$FFMPEG" ] || \
    die "ffmpeg not found: $FFMPEG"

if [ -n "$FFMPEG_BUILD_DIR" ]; then
    [ -d "$FFMPEG_BUILD_DIR" ] || die "FFMPEG_BUILD_DIR not found: $FFMPEG_BUILD_DIR"
    BUILD_LIBS=""
    for d in libavfilter libavutil libavcodec libavformat libswscale \
             libswresample libavdevice; do
        [ -d "$FFMPEG_BUILD_DIR/$d" ] && BUILD_LIBS="$BUILD_LIBS:$FFMPEG_BUILD_DIR/$d"
    done
    LD_LIBRARY_PATH="${BUILD_LIBS#:}:$CUDA_SHIM_DIR:$ROCM_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
fi

mkdir -p "$OUT_DIR/logs" || die "cannot create $OUT_DIR"

run_timeout() {
    if command -v timeout >/dev/null 2>&1; then
        timeout "$TIMEOUT_SECONDS" "$@"
    else
        "$@"
    fi
}

SOURCE="$OUT_DIR/source-${SRC_WIDTH}x${SRC_HEIGHT}.yuv"
"$FFMPEG" -hide_banner -nostdin -v error \
    -f lavfi -i "testsrc=size=${SRC_WIDTH}x${SRC_HEIGHT}:rate=$RATE:duration=1" \
    -vf format=yuv420p -frames:v "$SRC_FRAMES" -pix_fmt yuv420p \
    -f rawvideo -y "$SOURCE" || die "failed to generate source"

{
    echo "date=$(date -Is)"
    "$FFMPEG" -hide_banner -version 2>&1 | head -3
    echo "--- libcuda ---"
    ldd "$FFMPEG" 2>/dev/null | grep -E "libcuda|amdhip" || true
    echo "CUDA_SHIM_DIR=$CUDA_SHIM_DIR"
} >"$OUT_DIR/environment.log" 2>&1

FAILURES=0
COUNT=0
for fmt in $FORMATS; do
    ref="$OUT_DIR/reference-$fmt.yuv"
    "$FFMPEG" -hide_banner -nostdin -v error \
        -f rawvideo -pix_fmt yuv420p -s "${SRC_WIDTH}x${SRC_HEIGHT}" -r "$RATE" \
        -i "$SOURCE" -vf "format=$fmt" -frames:v "$SRC_FRAMES" \
        -f rawvideo -pix_fmt "$fmt" -y "$ref" || \
        die "failed to render $fmt reference"

    for run in $(seq 1 "$REPEAT"); do
        out="$OUT_DIR/$fmt-$run.yuv"
        log="$OUT_DIR/logs/$fmt-$run.log"
        run_timeout "$FFMPEG" -hide_banner -nostdin -v error -threads 1 \
            -init_hw_device cuda=cu:0 -filter_hw_device cu \
            -f rawvideo -pix_fmt yuv420p -s "${SRC_WIDTH}x${SRC_HEIGHT}" \
            -r "$RATE" -i "$SOURCE" \
            -vf "format=$fmt,hwupload_cuda,hwdownload,format=$fmt" \
            -frames:v "$SRC_FRAMES" -f rawvideo -pix_fmt "$fmt" -y "$out" \
            >"$log" 2>&1
        rc=$?
        if [ $rc -ne 0 ]; then
            echo "FAIL: $fmt run $run exited $rc (see $log)" >&2
            FAILURES=$((FAILURES + 1))
            continue
        fi
        if ! cmp -s "$ref" "$out"; then
            offset=$(cmp -l "$ref" "$out" 2>/dev/null | head -1 | awk '{print $1 - 1}')
            echo "FAIL: $fmt run $run differs from software reference at byte ${offset:-?}" >&2
            FAILURES=$((FAILURES + 1))
        else
            echo "PASS: $fmt run $run matches software reference ($(stat -c %s "$out") bytes)"
            COUNT=$((COUNT + 1))
        fi
    done
done

if [ "$FAILURES" -ne 0 ]; then
    echo "RESULT: FAIL ($FAILURES run(s) failed, $COUNT passed)" >&2
    exit 1
fi
echo "RESULT: PASS ($COUNT upload/download round trips byte-identical)"
