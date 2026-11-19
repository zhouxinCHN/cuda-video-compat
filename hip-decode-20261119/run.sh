#!/usr/bin/env bash
# run.sh — build the Vulkan Video H.264 linear-decode probe and run its matrix.
#
# What it checks
#   * the driver decodes a stream into a VK_IMAGE_TILING_LINEAR image created
#     with VK_KHR_video_maintenance1's profile-independent flag and exported as
#     an OPAQUE_FD dma-buf, and HIP reads it with a plain device pointer;
#   * every decoded picture matches FFmpeg's software decode byte for byte, in
#     display order, for the profiles/chroma formats/bit depths and protocol
#     features the local toolchain can produce.
#
# The stream generator uses libx264 directly (an 8-bit build can create 8-bit
# 4:2:0/4:2:2/4:4:4 streams; 10-bit needs a high-depth libx264 and is reported
# as SKIP). Cases the driver does not support come back as SKIP with the
# VkResult from vkGetPhysicalDeviceVideoCapabilitiesKHR.
#
# Environment
#   MESA_DIR       Vulkan headers (default $HOME/mesa)
#   FFMPEG_DIR     FFmpeg build tree for the parser objects (default $HOME/ffmpeg-7.1)
#   FFMPEG         ffmpeg binary for the software reference (default $FFMPEG_DIR/ffmpeg)
#   HIPCC, HIP_ARCH, CC
#   VALIDATION=1   enable VK_LAYER_KHRONOS_validation when installed (default 1)
#   KEEP=1         keep the temporary build directory
#   CASES          subset: "profiles resolutions chroma depth fixture" (default all)
#
# Exit code: 0 = no failures (SKIPs are allowed), 1 = at least one FAIL.

set -uo pipefail

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MESA_DIR=${MESA_DIR:-"$HOME/mesa"}
FFMPEG_DIR=${FFMPEG_DIR:-"$HOME/ffmpeg-7.1"}
FFMPEG=${FFMPEG:-"$FFMPEG_DIR/ffmpeg"}
HIPCC=${HIPCC:-/opt/rocm/bin/hipcc}
CC=${CC:-gcc}
HIP_ARCH=${HIP_ARCH:-gfx1100}
VALIDATION=${VALIDATION:-1}
KEEP=${KEEP:-0}
CASES=${CASES:-"profiles resolutions chroma depth fixture"}

build=$(mktemp -d "${TMPDIR:-/tmp}/vulkan-hip-decode.XXXXXXXX")
cleanup() { [[ $KEEP == 1 ]] && echo "build dir kept: $build" || rm -rf -- "$build"; }
trap cleanup EXIT

die() { echo "FAIL: $*" >&2; exit 2; }

command -v "$CC" >/dev/null || die "C compiler not found: $CC"
[ -x "$HIPCC" ] || die "hipcc not found: $HIPCC"
[ -x "$FFMPEG" ] || die "ffmpeg not found: $FFMPEG"
[ -d "$MESA_DIR/include/vulkan" ] || die "MESA_DIR has no Vulkan headers: $MESA_DIR"
[ -f "$FFMPEG_DIR/libavcodec/h264_ps.o" ] || die "FFMPEG_DIR is not a built FFmpeg tree: $FFMPEG_DIR"

FF_OBJS=(h2645_parse.o h264_ps.o h2645_vui.o h2645data.o h264data.o
         h264_parse.o golomb.o mathtables.o refstruct.o)
for obj in "${FF_OBJS[@]}"; do
    [ -f "$FFMPEG_DIR/libavcodec/$obj" ] || die "missing FFmpeg parser object $obj"
done

echo "== build =="
"$CC" -O2 -Wall "$here/gen_stream.c" -o "$build/gen_stream" -lx264 || die "gen_stream build failed"
"$HIPCC" -std=c++17 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers \
    -Wno-gcc-install-dir-libstdcxx -Wno-register -Wno-deprecated-declarations -Wno-unused-parameter -Wno-sign-compare \
    --offload-arch="$HIP_ARCH" -I"$MESA_DIR/include" -I"$FFMPEG_DIR" \
    -x hip "$here/decode.cpp" -x none "${FF_OBJS[@]/#/$FFMPEG_DIR/libavcodec/}" \
    -l:libvulkan.so.1 -L"$FFMPEG_DIR/libavutil" -lavutil \
    -Wl,-rpath,"$FFMPEG_DIR/libavutil" -o "$build/decode" || die "decode build failed"

export VK_DRIVER_FILES="${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/radeon_icd.json}"
if [[ $VALIDATION == 1 ]] && [ -d /usr/share/vulkan/explicit_layer.d ]; then
    export VK_INSTANCE_LAYERS="${VK_INSTANCE_LAYERS:-VK_LAYER_KHRONOS_validation}"
else
    unset VK_INSTANCE_LAYERS
fi

results="$build/results.txt"
: >"$results"
pass=0 fail=0 skip=0 failed=""

record() { printf '%s\n' "$*" | tee -a "$results"; }

# run_case <name> <ref-format> [gen_stream options...]
run_case() {
    local name=$1 pixfmt=$2
    shift 2
    local stream="$build/$name.h264" ref="$build/$name.ref" log="$build/$name.log"
    if ! "$build/gen_stream" "$stream" "$@" >"$log" 2>&1; then
        record "SKIP $name (encoder cannot produce this stream; 10-bit needs a high-depth libx264)"
        skip=$((skip + 1))
        return
    fi
    if ! "$FFMPEG" -v error -y -threads 1 -i "$stream" -pix_fmt "$pixfmt" -f rawvideo "$ref" \
            >>"$log" 2>&1; then
        record "FAIL $name (software reference decode failed, see $log)"
        failed="$failed $name"
        fail=$((fail + 1))
        return
    fi
    run_decode "$name" "$stream" "$ref" "$pixfmt" "$log"
}

# run_decode <name> <stream> <reference> <ref-format> <log>
run_decode() {
    local name=$1 stream=$2 ref=$3 pixfmt=$4 log=$5
    "$build/decode" "$stream" "$ref" "$pixfmt" >>"$log" 2>&1
    local rc=$?
    local validation=0
    grep -qE 'Validation Error|VUID-.*\(ERROR' "$log" && validation=1
    if [ $validation -ne 0 ]; then
        record "FAIL $name (validation layer reported an error, see $log)"
        failed="$failed $name"
        fail=$((fail + 1))
    elif [ $rc -eq 0 ]; then
        record "PASS $name $(grep -E '^PASS' "$log" | tail -1 | sed 's/^PASS //')"
        pass=$((pass + 1))
    elif [ $rc -eq 77 ]; then
        record "SKIP $name $(grep -E '^SKIP' "$log" | tail -1 | sed 's/^SKIP //')"
        skip=$((skip + 1))
    else
        record "FAIL $name (decode rc=$rc: $(grep -E 'FAIL|SKIP' "$log" | tail -1))"
        failed="$failed $name"
        fail=$((fail + 1))
    fi
}

echo "== driver capability probe =="
"$build/decode" --probe 2>&1 | tee "$build/probe.log" | sed -n '/^probe:/p'
if ! grep -q '^probe: high      4:2:0  8-bit OK' "$build/probe.log"; then
    die "the driver does not offer H.264 High 4:2:0 8-bit decode"
fi

case " $CASES " in *' profiles '*)
echo "== profiles and protocol features (320x240, 8-bit 4:2:0) =="
run_case baseline nv12 --width 320 --height 240 --frames 24 --profile baseline \
    --bframes 0 --cabac 0 --refs 1
run_case main nv12 --width 320 --height 240 --frames 24 --profile main --bframes 2 --refs 2
run_case high nv12 --width 320 --height 240 --frames 24 --profile high --bframes 3 --refs 4
run_case high-bpyramid nv12 --width 320 --height 240 --frames 24 --profile high \
    --bframes 3 --bpyramid 2 --refs 8 --weightp 2 --weightb 1 --8x8dct 1
run_case high-ref16 nv12 --width 320 --height 240 --frames 24 --profile high --bframes 3 --refs 16
run_case high-slices4 nv12 --width 320 --height 240 --frames 24 --profile high --slices 4
run_case high-intra-refresh nv12 --width 320 --height 240 --frames 24 --profile high \
    --intra-refresh 1 --keyint 300 --bframes 3
run_case high-open-gop nv12 --width 320 --height 240 --frames 24 --profile high \
    --open-gop 1 --bframes 3 --keyint 12
run_case high-nodeblock nv12 --width 320 --height 240 --frames 24 --profile high --deblock 0
run_case high-aud nv12 --width 320 --height 240 --frames 24 --profile high --aud 1
run_case high-weighted nv12 --width 320 --height 240 --frames 24 --profile high \
    --weightp 2 --weightb 1
esac

case " $CASES " in *' resolutions '*)
echo "== resolutions (High profile) =="
run_case res-96x96 nv12 --width 96 --height 96 --frames 24 --profile high --bframes 2
run_case res-176x144 nv12 --width 176 --height 144 --frames 24 --profile high --bframes 2
run_case res-320x240 nv12 --width 320 --height 240 --frames 24 --profile high --bframes 2
run_case res-640x368 nv12 --width 640 --height 368 --frames 24 --profile high --bframes 2
run_case res-854x480-crop nv12 --width 854 --height 480 --frames 16 --profile high \
    --bframes 2 --preset ultrafast
run_case res-1280x720 nv12 --width 1280 --height 720 --frames 12 --profile high \
    --bframes 2 --preset ultrafast
run_case res-1920x1080-crop nv12 --width 1920 --height 1080 --frames 6 --profile high \
    --bframes 2 --preset ultrafast
run_case res-3840x2160 nv12 --width 3840 --height 2160 --frames 3 --profile high \
    --bframes 2 --preset ultrafast
esac

case " $CASES " in *' chroma '*)
echo "== chroma formats (8-bit) =="
run_case chroma-422-yuv422p yuv422p --width 320 --height 240 --frames 8 --profile high422 \
    --chroma 422 --bframes 2
run_case chroma-444-yuv444p yuv444p --width 320 --height 240 --frames 8 --profile high444 \
    --chroma 444 --bframes 2
esac

case " $CASES " in *' depth '*)
echo "== bit depths =="
run_case depth-10-p010le p010le --width 320 --height 240 --frames 8 --profile high10 \
    --bitdepth 10 --bframes 2
esac

case " $CASES " in *' fixture '*)
echo "== original IDR-only fixture =="
if [ -f "$here/fixture.h264" ]; then
    if "$FFMPEG" -v error -y -threads 1 -i "$here/fixture.h264" -pix_fmt nv12 -f rawvideo \
            "$build/fixture.ref" >"$build/fixture.log" 2>&1; then
        run_decode fixture "$here/fixture.h264" "$build/fixture.ref" nv12 "$build/fixture.log"
    else
        record "FAIL fixture (software reference decode failed)"
        failed="$failed fixture"
        fail=$((fail + 1))
    fi
fi
esac

echo
echo "== summary: PASS=$pass SKIP=$skip FAIL=$fail =="
if [ $fail -ne 0 ]; then
    echo "failed cases:$failed"
    exit 1
fi
echo "PASS: Vulkan linear decode matches the software decoder for every runnable case"
