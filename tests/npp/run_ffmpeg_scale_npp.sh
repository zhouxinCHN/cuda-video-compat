#!/usr/bin/env bash
# FFmpeg scale_npp end-to-end matrix.
#
# Produces a deterministic raw-video matrix through FFmpeg's own scale_npp
# filter. The same script runs on an NVIDIA host with real NPP and on the AMD
# host with the NPP shim; compare_ffmpeg_scale_npp.py then requires every case
# to be byte-identical.
#
# The input is a generated testsrc fixture, not a decoded stream, so the
# comparison isolates NPP numerics from decoder differences. The fixture is fed
# through lavfi directly: a rawvideo input plus hwupload_cuda still shows a
# cross-frame corruption on this host (tracked separately), while the lavfi
# path is deterministic. A software-only pass renders the same frames to
# source-reference.yuv; its hash is recorded and the comparator rejects runs
# that started from different frames.
#
# Matrix: 5 destination geometries x 4 interpolation modes x 2 surface formats.
#   - nv12    path: P2P3 -> resize -> P3P2 through FFmpeg's scale_npp
#   - yuv420p path: per-plane resize (P3)
#   - "default" omits interp_algo, exercising FFmpeg's default (cubic)
#
# Usage:
#   FFMPEG_BUILD_DIR=/tmp/ffmpeg-npp-src \
#   NPP_RUNTIME=shim OUT_DIR=/tmp/npp-e2e-amd \
#     ./run_ffmpeg_scale_npp.sh
#
# Environment:
#   FFMPEG            ffmpeg executable (default $FFMPEG_BUILD_DIR/ffmpeg or PATH)
#   FFMPEG_BUILD_DIR  FFmpeg build tree; its library dirs are prepended to
#                     LD_LIBRARY_PATH (needed for an uninstalled NPP build)
#   NPP_RUNTIME       shim | cuda | auto (default auto: detected from ldd)
#   SHIM_DIR          expected shim directory for NPP_RUNTIME=shim
#   OUT_DIR           output directory; must not contain a previous manifest
#   SRC_WIDTH/SRC_HEIGHT/SRC_FRAMES  fixture geometry (default 320/240/5)
#   REPEAT            runs per case; all runs must hash identically (default 2)
#   TIMEOUT_SECONDS   per-run timeout (default 60)
#
# Exit code: 0 = all cases passed and were deterministic; 1 = case failure or
# nondeterminism; 2 = environment/argument failure.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
SHIM_DIR=${SHIM_DIR:-"$REPO_ROOT/npp_shim"}
CUDA_SHIM_DIR=${CUDA_SHIM_DIR:-"$REPO_ROOT/cuda_shim"}
FFMPEG_BUILD_DIR=${FFMPEG_BUILD_DIR:-}
FFMPEG=${FFMPEG:-${FFMPEG_BUILD_DIR:+$FFMPEG_BUILD_DIR/ffmpeg}}
FFMPEG=${FFMPEG:-ffmpeg}
NPP_RUNTIME=${NPP_RUNTIME:-auto}
OUT_DIR=${OUT_DIR:-}
SRC_WIDTH=${SRC_WIDTH:-320}
SRC_HEIGHT=${SRC_HEIGHT:-240}
SRC_FRAMES=${SRC_FRAMES:-5}
RATE=${RATE:-5}
REPEAT=${REPEAT:-2}
TIMEOUT_SECONDS=${TIMEOUT_SECONDS:-60}
ROCM_DIR=${ROCM_DIR:-$(ls -d /opt/rocm-* 2>/dev/null | head -1)}
ROCM_DIR=${ROCM_DIR:-/opt/rocm}

die() {
    echo "FAIL: $*" >&2
    exit 2
}

command -v "$FFMPEG" >/dev/null 2>&1 || [ -x "$FFMPEG" ] || \
    die "ffmpeg not found: $FFMPEG"

OUT_DIR=${OUT_DIR:-"$PWD/ffmpeg-scale-npp-${NPP_RUNTIME}"}
[ -e "$OUT_DIR/manifest.tsv" ] && die "$OUT_DIR already contains a manifest; use a fresh OUT_DIR"
mkdir -p "$OUT_DIR/logs" || die "cannot create $OUT_DIR"

# A build-tree FFmpeg is not installed, so its own libraries must win over any
# system FFmpeg.
if [ -n "$FFMPEG_BUILD_DIR" ]; then
    [ -d "$FFMPEG_BUILD_DIR" ] || die "FFMPEG_BUILD_DIR not found: $FFMPEG_BUILD_DIR"
    BUILD_LIBS=""
    for d in libavfilter libavutil libavcodec libavformat libswscale \
             libswresample libavdevice; do
        [ -d "$FFMPEG_BUILD_DIR/$d" ] && BUILD_LIBS="$BUILD_LIBS:$FFMPEG_BUILD_DIR/$d"
    done
    LD_LIBRARY_PATH="${BUILD_LIBS#:}:$SHIM_DIR:$CUDA_SHIM_DIR:$ROCM_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
fi

run_timeout() {
    if command -v timeout >/dev/null 2>&1; then
        timeout "$TIMEOUT_SECONDS" "$@"
    else
        "$@"
    fi
}

# --- environment -----------------------------------------------------------
{
    echo "date=$(date -Is)"
    echo "ffmpeg=$FFMPEG"
    "$FFMPEG" -hide_banner -version 2>&1 | head -3
    echo "--- ffmpeg npp linkage ---"
    ldd "$FFMPEG" 2>/dev/null | grep -E "libnpp|libcuda|amdhip" || true
    echo "--- libavfilter npp linkage ---"
    AVFILTER=$(ldd "$FFMPEG" 2>/dev/null | awk '/libavfilter\.so/ {print $3; exit}')
    if [ -n "${AVFILTER:-}" ]; then
        ldd "$AVFILTER" 2>/dev/null | grep -E "libnpp|libcuda|amdhip" || true
    fi
    echo "LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}"
} >"$OUT_DIR/environment.log" 2>&1

"$FFMPEG" -hide_banner -h filter=scale_npp >"$OUT_DIR/logs/filter-help.log" 2>&1 || \
    die "this FFmpeg has no scale_npp filter (libnpp build required)"

NPP_LINE=$(ldd "${AVFILTER:-$FFMPEG}" 2>/dev/null | grep -E "libnpp(ig|icc|c)\.so" | head -1)
[ -n "$NPP_LINE" ] || die "FFmpeg is not linked against NPP libraries"
case "$NPP_RUNTIME" in
    auto)
        case "$NPP_LINE" in
            *"$SHIM_DIR"*) NPP_RUNTIME=shim ;;
            *)             NPP_RUNTIME=cuda ;;
        esac
        ;;
    shim)
        case "$NPP_LINE" in
            *"$SHIM_DIR"*) ;;
            *) die "NPP_RUNTIME=shim but FFmpeg resolves $NPP_LINE" ;;
        esac
        ;;
    cuda)
        case "$NPP_LINE" in
            *"$SHIM_DIR"*) die "NPP_RUNTIME=cuda but FFmpeg resolves the shim: $NPP_LINE" ;;
            *) ;;
        esac
        ;;
    *) die "NPP_RUNTIME must be shim, cuda or auto" ;;
esac
echo "npp_runtime=$NPP_RUNTIME" >>"$OUT_DIR/environment.log"
echo "npp_library=$NPP_LINE" >>"$OUT_DIR/environment.log"

# --- fixture ---------------------------------------------------------------
# Software-only render of exactly the frames the filter chain receives. On an
# NVIDIA host this must hash identically; a mismatch means the two hosts
# generated different input frames and no NPP conclusion can be drawn.
SOURCE="$OUT_DIR/source-reference.yuv"
"$FFMPEG" -hide_banner -nostdin -v error \
    -f lavfi -i "testsrc=size=${SRC_WIDTH}x${SRC_HEIGHT}:rate=$RATE:duration=1" \
    -vf format=yuv420p -frames:v "$SRC_FRAMES" -pix_fmt yuv420p \
    -f rawvideo -y "$SOURCE" || die "failed to generate source reference"
EXPECTED_SRC_BYTES=$((SRC_WIDTH * SRC_HEIGHT * 3 / 2 * SRC_FRAMES))
ACTUAL_SRC_BYTES=$(stat -c %s "$SOURCE")
[ "$ACTUAL_SRC_BYTES" -eq "$EXPECTED_SRC_BYTES" ] || \
    die "source reference is $ACTUAL_SRC_BYTES bytes, expected $EXPECTED_SRC_BYTES"
sha256sum "$SOURCE" | awk '{print $1}' >"$OUT_DIR/source.sha256"
echo "source_sha256=$(cat "$OUT_DIR/source.sha256")" >>"$OUT_DIR/environment.log"

# --- matrix ----------------------------------------------------------------
GEOMETRIES="160x120 214x118 640x480 320x240 100x100"
ALGOS="nn linear cubic default"
FORMATS="nv12 yuv420p"
TOTAL_BYTES=0
COUNT=0
FAILURES=0

printf 'case\tsrc_w\tsrc_h\tdst_w\tdst_h\tpix_fmt\tinterp_algo\tframes\tbytes\tsha256\n' \
    >"$OUT_DIR/manifest.tsv"

for geom in $GEOMETRIES; do
    dw=${geom%x*}
    dh=${geom#*x}
    for algo in $ALGOS; do
        for fmt in $FORMATS; do
            case_name="${geom}-${fmt}-${algo}"
            out="$OUT_DIR/$case_name.yuv"
            log="$OUT_DIR/logs/$case_name.log"

            if [ "$algo" = default ]; then
                algo_opt=""
            else
                algo_opt=":interp_algo=$algo"
            fi

            vf="format=${fmt},hwupload_cuda,scale_npp=w=${dw}:h=${dh}${algo_opt}:format=${fmt},hwdownload,format=${fmt}"
            args=(-hide_banner -nostdin -v error -threads 1
                  -init_hw_device cuda=cu:0 -filter_hw_device cu
                  -f lavfi -i "testsrc=size=${SRC_WIDTH}x${SRC_HEIGHT}:rate=$RATE:duration=1"
                  -vf "$vf" -frames:v "$SRC_FRAMES"
                  -f rawvideo -pix_fmt "$fmt" -y "$out")

            first_hash=""
            ok=1
            for run in $(seq 1 "$REPEAT"); do
                run_timeout "$FFMPEG" "${args[@]}" >"$log" 2>&1
                rc=$?
                if [ $rc -ne 0 ]; then
                    echo "FAIL: $case_name run $run exited $rc (see $log)" >&2
                    ok=0
                    break
                fi
                hash=$(sha256sum "$out" 2>/dev/null | awk '{print $1}')
                if [ -z "$first_hash" ]; then
                    first_hash=$hash
                elif [ "$hash" != "$first_hash" ]; then
                    echo "FAIL: $case_name is nondeterministic between runs ($first_hash vs $hash)" >&2
                    ok=0
                    break
                fi
            done
            [ $ok -eq 1 ] || { FAILURES=$((FAILURES + 1)); continue; }

            expected_bytes=$((dw * dh * 3 / 2 * SRC_FRAMES))
            actual_bytes=$(stat -c %s "$out")
            if [ "$actual_bytes" -ne "$expected_bytes" ]; then
                echo "FAIL: $case_name is $actual_bytes bytes, expected $expected_bytes" >&2
                FAILURES=$((FAILURES + 1))
                continue
            fi

            printf '%s\t%d\t%d\t%d\t%d\t%s\t%s\t%d\t%d\t%s\n' \
                "$case_name" "$SRC_WIDTH" "$SRC_HEIGHT" "$dw" "$dh" "$fmt" \
                "$algo" "$SRC_FRAMES" "$actual_bytes" "$first_hash" \
                >>"$OUT_DIR/manifest.tsv"
            TOTAL_BYTES=$((TOTAL_BYTES + actual_bytes))
            COUNT=$((COUNT + 1))
            echo "PASS: $case_name $actual_bytes bytes ${first_hash:0:16}"
        done
    done
done

(cd "$OUT_DIR" && sha256sum ./*.yuv 2>/dev/null | sort -k2) >"$OUT_DIR/SHA256SUMS"

if [ "$FAILURES" -ne 0 ]; then
    echo "RESULT: FAIL ($FAILURES case(s) failed, $COUNT passed)" >&2
    exit 1
fi
[ "$COUNT" -gt 0 ] || die "no cases ran"

echo "RESULT: PASS ($COUNT cases, $TOTAL_BYTES bytes, runtime=$NPP_RUNTIME)"
