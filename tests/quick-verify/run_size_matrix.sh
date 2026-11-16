#!/usr/bin/env bash
# 1080p / 4K decode correctness matrix.
#
# Generates H.264 streams at the requested sizes (needs an FFmpeg built with
# libx264), decodes them through the shim and compares every frame with the
# software decoder. Both host paths and both candidate backends run.
#
# Usage:
#   FFMPEG_BUILD_DIR=/path/to/ffmpeg ./run_size_matrix.sh
#
# Environment:
#   FFMPEG / FFMPEG_BUILD_DIR  ffmpeg with libx264
#   BACKENDS                   default "vaapi vk"
#   MODES                      default "cuvid nvdec"
#   SIZES                      default "1920x1080 3840x2160"
#   FRAMES                     30 (1080p) and 5 (4K) by default
#   OUT_DIR                    default ./size-matrix
#   EVIDENCE_DIR               optional
#   RADV_PERFTEST              default video_decode
#   TIMEOUT_SECONDS            per decode (default 300)
#
# Exit code: 0 = all combinations match; 1 = mismatch/failure; 2 = setup.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
FFMPEG_BUILD_DIR=${FFMPEG_BUILD_DIR:-}
FFMPEG=${FFMPEG:-${FFMPEG_BUILD_DIR:+$FFMPEG_BUILD_DIR/ffmpeg}}
FFMPEG=${FFMPEG:-ffmpeg}
BACKENDS=${BACKENDS:-"vaapi vk"}
MODES=${MODES:-"cuvid nvdec"}
SIZES=${SIZES:-"1920x1080 3840x2160"}
OUT_DIR=${OUT_DIR:-"$PWD/size-matrix"}
EVIDENCE_DIR=${EVIDENCE_DIR:-}
RADV_PERFTEST=${RADV_PERFTEST:-video_decode}
TIMEOUT_SECONDS=${TIMEOUT_SECONDS:-300}
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

"$FFMPEG" -hide_banner -encoders 2>/dev/null | grep -q " libx264 " || \
    die "libx264 encoder is required to generate the size matrix"

mkdir -p "$OUT_DIR" || die "cannot create $OUT_DIR"
[ -e "$OUT_DIR/summary.txt" ] && die "$OUT_DIR already contains a summary"

frames_for_size() {
    case "$1" in
        3840x2160) echo 5 ;;
        *)         echo 30 ;;
    esac
}

hashes() {
    awk -F',' '!/^#/ && NF >= 6 { gsub(/ /, "", $5); gsub(/ /, "", $6); print $5, $6 }' "$1"
}

: >"$OUT_DIR/summary.txt"
PASSES=0
FAILURES=0

for size in $SIZES; do
    frames=$(frames_for_size "$size")
    stream="$OUT_DIR/$size.h264"
    if [ ! -f "$stream" ]; then
        "$FFMPEG" -hide_banner -nostdin -v error \
            -f lavfi -i "testsrc2=size=$size:rate=30:duration=$((frames / 30 + 1))" \
            -vf format=yuv420p -frames:v "$frames" -c:v libx264 -preset ultrafast \
            -x264-params "keyint=30:bframes=2:scenecut=0" -y "$stream" || \
            die "failed to generate $size stream"
    fi

    ref="$OUT_DIR/ref-$size.md5"
    "$FFMPEG" -hide_banner -nostdin -v error -i "$stream" -fps_mode passthrough \
        -pix_fmt yuv420p -f framemd5 -y "$ref" || die "software decode failed for $size"
    hashes "$ref" >"$OUT_DIR/ref-$size.hashes"
    ref_frames=$(wc -l <"$OUT_DIR/ref-$size.hashes")

    for mode in $MODES; do
        for backend in $BACKENDS; do
            case_name="$size-$mode-$backend"
            log="$OUT_DIR/$case_name.log"
            start=$(date +%s%N)
            case "$mode" in
                cuvid)
                    timeout -s KILL "$TIMEOUT_SECONDS" env CUVID_SHIM_BACKEND=$backend \
                        "$FFMPEG" -hide_banner -nostdin -v error -c:v h264_cuvid \
                        -i "$stream" -fps_mode passthrough -pix_fmt yuv420p \
                        -f framemd5 -y "$OUT_DIR/$case_name.md5" >"$log" 2>&1
                    ;;
                nvdec)
                    timeout -s KILL "$TIMEOUT_SECONDS" env CUVID_SHIM_BACKEND=$backend \
                        "$FFMPEG" -hide_banner -nostdin -v error \
                        -hwaccel nvdec -hwaccel_output_format cuda \
                        -i "$stream" -vf hwdownload,format=nv12 \
                        -fps_mode passthrough -pix_fmt yuv420p \
                        -f framemd5 -y "$OUT_DIR/$case_name.md5" >"$log" 2>&1
                    ;;
                *) die "unknown mode '$mode'" ;;
            esac
            rc=$?
            elapsed_ms=$(( ($(date +%s%N) - start) / 1000000 ))
            if [ $rc -ne 0 ]; then
                echo "FAIL: $case_name exited $rc (see $log)" >&2
                echo "FAIL $case_name exit=$rc" >>"$OUT_DIR/summary.txt"
                FAILURES=$((FAILURES + 1))
                continue
            fi
            hashes "$OUT_DIR/$case_name.md5" >"$OUT_DIR/$case_name.hashes"
            if diff -q "$OUT_DIR/ref-$size.hashes" "$OUT_DIR/$case_name.hashes" >/dev/null; then
                echo "PASS: $case_name $ref_frames frames, ${elapsed_ms} ms"
                echo "PASS $case_name frames=$ref_frames elapsed_ms=$elapsed_ms" >>"$OUT_DIR/summary.txt"
                PASSES=$((PASSES + 1))
            else
                echo "FAIL: $case_name differs from software" >&2
                echo "FAIL $case_name mismatch" >>"$OUT_DIR/summary.txt"
                FAILURES=$((FAILURES + 1))
            fi
        done
    done
done

echo "RESULT: $PASSES passed, $FAILURES failed" | tee -a "$OUT_DIR/summary.txt"

if [ -n "$EVIDENCE_DIR" ]; then
    mkdir -p "$EVIDENCE_DIR" || die "cannot create $EVIDENCE_DIR"
    cp "$OUT_DIR/summary.txt" "$EVIDENCE_DIR/"
    cp "$OUT_DIR"/*.log "$EVIDENCE_DIR/" 2>/dev/null || true
    echo "Evidence archived: $EVIDENCE_DIR"
fi

[ "$FAILURES" -eq 0 ] || exit 1
echo "PASS: 1080p/4K matrix matches the software decoder"
