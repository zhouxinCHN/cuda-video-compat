#!/usr/bin/env bash
# Dynamic-resolution verification at the CUVID level.
#
# FFmpeg's CLI cannot change an initialized AVHWFramesContext, so the scenario
# is driven by a small host (verify_resolution_change) that recreates the CUVID
# decoder on every sequence change, like a player would. The stream is
# 11 frames of 320x240 followed by 11 frames of 640x480.
#
# For each backend the host dumps every displayed frame as tightly packed NV12.
# The script compares those dumps with the software decoder's NV12 output for
# each segment.
#
# Usage:
#   FFMPEG_BUILD_DIR=/path/to/ffmpeg ./run_resolution_change.sh
#
# Environment:
#   FFMPEG / FFMPEG_BUILD_DIR  software reference decoder
#   BACKENDS                   default "vaapi vk"
#   OUT_DIR                    default ./resolution-change
#   EVIDENCE_DIR               optional archive directory
#   RADV_PERFTEST              default video_decode
#
# Exit code: 0 = all frames match; 1 = mismatch/decode failure; 2 = setup.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
STREAM_DIR=${STREAM_DIR:-"$REPO_ROOT/testdata/h264"}
HOST=${HOST:-"$SCRIPT_DIR/verify_resolution_change"}
FFMPEG_BUILD_DIR=${FFMPEG_BUILD_DIR:-}
FFMPEG=${FFMPEG:-${FFMPEG_BUILD_DIR:+$FFMPEG_BUILD_DIR/ffmpeg}}
FFMPEG=${FFMPEG:-ffmpeg}
BACKENDS=${BACKENDS:-"vaapi vk"}
OUT_DIR=${OUT_DIR:-"$PWD/resolution-change"}
EVIDENCE_DIR=${EVIDENCE_DIR:-}
RADV_PERFTEST=${RADV_PERFTEST:-video_decode}
SHIM_DIR=${SHIM_DIR:-"$REPO_ROOT/cuda_shim"}
ROCM_DIR=${ROCM_DIR:-$(ls -d /opt/rocm-* 2>/dev/null | head -1)}
ROCM_DIR=${ROCM_DIR:-/opt/rocm}

die() {
    echo "FAIL: $*" >&2
    exit 2
}

[ -x "$HOST" ] || die "$HOST not built (make -C tests/zerocopy verify_resolution_change)"
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

SEG_A="$STREAM_DIR/test_reschange_320x240.h264"
SEG_B="$STREAM_DIR/test_reschange_640x480.h264"
[ -f "$SEG_A" ] && [ -f "$SEG_B" ] || die "resolution-change streams missing"
cat "$SEG_A" "$SEG_B" >"$OUT_DIR/reschange.h264"

# Software reference per segment (fixed dimensions).
"$FFMPEG" -hide_banner -nostdin -v error -i "$SEG_A" -fps_mode passthrough \
    -pix_fmt nv12 -f rawvideo -y "$OUT_DIR/ref-320x240.nv12" || \
    die "software decode failed for 320x240 segment"
"$FFMPEG" -hide_banner -nostdin -v error -i "$SEG_B" -fps_mode passthrough \
    -pix_fmt nv12 -f rawvideo -y "$OUT_DIR/ref-640x480.nv12" || \
    die "software decode failed for 640x480 segment"

: >"$OUT_DIR/summary.txt"
FAILURES=0
PASSES=0

check_frames() {
    local backend=$1 frame_dir=$2
    python3 - "$OUT_DIR" "$frame_dir" <<'PY'
import sys
from pathlib import Path

out_dir, frame_dir = Path(sys.argv[1]), Path(sys.argv[2])
ref_a = (out_dir / "ref-320x240.nv12").read_bytes()
ref_b = (out_dir / "ref-640x480.nv12").read_bytes()
size_a, size_b = 320 * 240 * 3 // 2, 640 * 480 * 3 // 2
count_a, count_b = len(ref_a) // size_a, len(ref_b) // size_b

frames = sorted(frame_dir.glob("frame-*.nv12"))
errors = []
if len(frames) != count_a + count_b:
    errors.append(f"frame count {len(frames)} != {count_a + count_b}")

for index, path in enumerate(frames):
    data = path.read_bytes()
    if index < count_a:
        expected = ref_a[index * size_a:(index + 1) * size_a]
        label = f"320x240 frame {index}"
    else:
        j = index - count_a
        expected = ref_b[j * size_b:(j + 1) * size_b]
        label = f"640x480 frame {j}"
    if data != expected:
        errors.append(f"{label} differs ({len(data)} vs {len(expected)} bytes)")

if errors:
    for error in errors[:10]:
        print(f"FAIL: {error}")
    sys.exit(1)
print(f"PASS: {len(frames)} frames match software ({count_a} at 320x240, {count_b} at 640x480)")
PY
}

for backend in $BACKENDS; do
    case "$backend" in
        vaapi|vk) ;;
        *) die "unknown backend '$backend'" ;;
    esac
    frame_dir="$OUT_DIR/frames-$backend"
    rm -rf "$frame_dir"
    mkdir -p "$frame_dir"
    log="$OUT_DIR/decode-$backend.log"
    if ! timeout -s KILL 120 env CUVID_SHIM_BACKEND=$backend "$HOST" \
            "$OUT_DIR/reschange.h264" "$frame_dir" >"$log" 2>&1; then
        echo "FAIL: $backend host exited non-zero (see $log)" >&2
        echo "FAIL $backend host-exit" >>"$OUT_DIR/summary.txt"
        FAILURES=$((FAILURES + 1))
        continue
    fi
    if ! grep -q "SEQUENCE CHANGE" "$log"; then
        echo "FAIL: $backend did not report a sequence change (see $log)" >&2
        echo "FAIL $backend no-sequence-change" >>"$OUT_DIR/summary.txt"
        FAILURES=$((FAILURES + 1))
        continue
    fi
    if result=$(check_frames "$backend" "$frame_dir"); then
        echo "PASS: $backend $result"
        echo "PASS $backend $result" >>"$OUT_DIR/summary.txt"
        PASSES=$((PASSES + 1))
    else
        echo "$result" | sed 's/^/  /' >&2
        echo "FAIL $backend frame-mismatch" >>"$OUT_DIR/summary.txt"
        FAILURES=$((FAILURES + 1))
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
echo "PASS: dynamic resolution handled on all requested backends"
