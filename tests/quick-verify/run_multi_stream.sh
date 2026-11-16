#!/usr/bin/env bash
# Multi-stream decode verification in one process.
#
# Decodes N streams concurrently in a single FFmpeg process (one decoder per
# input, each on its own thread) and compares every output with the software
# decoder. Two resolutions alternate so a process-global parameter-set cache
# would be caught immediately.
#
# Usage:
#   FFMPEG_BUILD_DIR=/path/to/ffmpeg ./run_multi_stream.sh
#
# Environment:
#   FFMPEG / FFMPEG_BUILD_DIR  ffmpeg binary and build tree
#   BACKENDS                   default "vaapi vk"
#   STREAMS                    number of concurrent inputs (default 8)
#   OUT_DIR                    default ./multi-stream
#   EVIDENCE_DIR               optional archive directory
#   RADV_PERFTEST              default video_decode
#   TIMEOUT_SECONDS            default 180
#
# Exit code: 0 = every stream matches; 1 = mismatch/decode failure; 2 = setup.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
STREAM_DIR=${STREAM_DIR:-"$REPO_ROOT/testdata/h264"}
FFMPEG_BUILD_DIR=${FFMPEG_BUILD_DIR:-}
FFMPEG=${FFMPEG:-${FFMPEG_BUILD_DIR:+$FFMPEG_BUILD_DIR/ffmpeg}}
FFMPEG=${FFMPEG:-ffmpeg}
BACKENDS=${BACKENDS:-"vaapi vk"}
STREAMS=${STREAMS:-8}
OUT_DIR=${OUT_DIR:-"$PWD/multi-stream"}
EVIDENCE_DIR=${EVIDENCE_DIR:-}
RADV_PERFTEST=${RADV_PERFTEST:-video_decode}
TIMEOUT_SECONDS=${TIMEOUT_SECONDS:-180}
SHIM_DIR=${SHIM_DIR:-"$REPO_ROOT/cuda_shim"}
ROCM_DIR=${ROCM_DIR:-$(ls -d /opt/rocm-* 2>/dev/null | head -1)}
ROCM_DIR=${ROCM_DIR:-/opt/rocm}

die() {
    echo "FAIL: $*" >&2
    exit 2
}

command -v "$FFMPEG" >/dev/null 2>&1 || [ -x "$FFMPEG" ] || \
    die "ffmpeg not found: $FFMPEG"

if [ -n "$FFMPEG_BUILD_DIR" ]; then
    BUILD_LIBS=""
    for d in libavfilter libavutil libavcodec libavformat libswscale \
             libswresample libavdevice libpostproc; do
        [ -d "$FFMPEG_BUILD_DIR/$d" ] && BUILD_LIBS="$BUILD_LIBS:$FFMPEG_BUILD_DIR/$d"
    done
    LD_LIBRARY_PATH="${BUILD_LIBS#:}:$SHIM_DIR:$ROCM_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
fi
export RADV_PERFTEST

mkdir -p "$OUT_DIR" || die "cannot create $OUT_DIR"
[ -e "$OUT_DIR/summary.txt" ] && die "$OUT_DIR already contains a summary"

SMALL="$STREAM_DIR/test_reschange_320x240.h264"
LARGE="$STREAM_DIR/test_reschange_640x480.h264"
[ -f "$SMALL" ] && [ -f "$LARGE" ] || die "multi-stream sources missing"

# Software references for the two source types.
"$FFMPEG" -hide_banner -nostdin -v error -i "$SMALL" -fps_mode passthrough \
    -pix_fmt yuv420p -f framemd5 -y "$OUT_DIR/ref-small.md5" || die "small ref failed"
"$FFMPEG" -hide_banner -nostdin -v error -i "$LARGE" -fps_mode passthrough \
    -pix_fmt yuv420p -f framemd5 -y "$OUT_DIR/ref-large.md5" || die "large ref failed"

hashes() {
    awk -F',' '!/^#/ && NF >= 6 { gsub(/ /, "", $6); print $6 }' "$1"
}

: >"$OUT_DIR/summary.txt"
FAILURES=0
PASSES=0

INPUTS=()
TYPES=()
for index in $(seq 0 $((STREAMS - 1))); do
    if [ $((index % 2)) -eq 0 ]; then
        INPUTS+=("$SMALL")
        TYPES+=("small")
    else
        INPUTS+=("$LARGE")
        TYPES+=("large")
    fi
done

for backend in $BACKENDS; do
    case "$backend" in
        vaapi|vk) ;;
        *) die "unknown backend '$backend'" ;;
    esac

    args=()
    for input in "${INPUTS[@]}"; do
        args+=(-c:v h264_cuvid -i "$input")
    done
    for index in $(seq 0 $((STREAMS - 1))); do
        args+=(-map "${index}:v" -fps_mode passthrough -pix_fmt yuv420p
               -f framemd5 -y "$OUT_DIR/$backend-$index.md5")
    done

    log="$OUT_DIR/decode-$backend.log"
    start=$(date +%s)
    if ! timeout -s KILL "$TIMEOUT_SECONDS" env CUVID_SHIM_BACKEND=$backend \
            "$FFMPEG" -hide_banner -nostdin -v error "${args[@]}" >"$log" 2>&1; then
        echo "FAIL: $backend decode failed (see $log)" >&2
        echo "FAIL $backend decode" >>"$OUT_DIR/summary.txt"
        FAILURES=$((FAILURES + 1))
        continue
    fi
    elapsed=$(( $(date +%s) - start ))

    backend_ok=1
    for index in $(seq 0 $((STREAMS - 1))); do
        ref="$OUT_DIR/ref-${TYPES[$index]}.md5"
        cand="$OUT_DIR/$backend-$index.md5"
        if ! diff -q <(hashes "$ref") <(hashes "$cand") >/dev/null; then
            echo "FAIL: $backend stream $index (${TYPES[$index]}) differs" >&2
            echo "FAIL $backend stream$index ${TYPES[$index]}" >>"$OUT_DIR/summary.txt"
            backend_ok=0
            FAILURES=$((FAILURES + 1))
        fi
    done
    if [ "$backend_ok" -eq 1 ]; then
        echo "PASS: $backend $STREAMS concurrent streams match software (${elapsed}s)"
        echo "PASS $backend streams=$STREAMS elapsed=${elapsed}s" >>"$OUT_DIR/summary.txt"
        PASSES=$((PASSES + 1))
    fi
done

echo "RESULT: $PASSES passed, $FAILURES failed" | tee -a "$OUT_DIR/summary.txt"

if [ -n "$EVIDENCE_DIR" ]; then
    mkdir -p "$EVIDENCE_DIR" || die "cannot create $EVIDENCE_DIR"
    cp "$OUT_DIR/summary.txt" "$EVIDENCE_DIR/"
    cp "$OUT_DIR"/decode-*.log "$EVIDENCE_DIR/" 2>/dev/null || true
    echo "Evidence archived: $EVIDENCE_DIR"
fi

[ "$FAILURES" -eq 0 ] || exit 1
echo "PASS: multi-stream decode matches software on all requested backends"
