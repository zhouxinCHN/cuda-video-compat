#!/usr/bin/env bash
# Complex H.264 decode scenarios through the CUVID shim.
#
# Compares the shim against FFmpeg's software decoder for scenarios that the
# basic four-stream matrix does not cover:
#   slices : multi-slice access units (4 slices per picture, High profile)
#   crop   : 1920x1080 stream whose coded height is 1088 (cropped chroma)
#   seek   : MP4 with B frames, decoded after an input seek (flush/reinit)
#
# Both host paths are exercised:
#   cuvid : -c:v h264_cuvid (the shim's own CUVID parser is used)
#   nvdec : -hwaccel nvdec (FFmpeg assembles Annex-B; parser cache differs)
#
# Both candidate backends (VAAPI / Vulkan Video) run by default; neither is a
# main line. Output is compared frame-by-frame with framemd5 in yuv420p.
#
# Usage:
#   FFMPEG_BUILD_DIR=/path/to/ffmpeg ./run_decode_scenarios.sh
#
# Environment:
#   FFMPEG / FFMPEG_BUILD_DIR   ffmpeg binary and build tree (libs on the path)
#   BACKENDS                   default "vaapi vk"
#   MODES                      default "cuvid nvdec"
#   SCENARIOS                  default "slices crop seek"
#   OUT_DIR                    default ./decode-scenarios
#   EVIDENCE_DIR               optional; must not already contain a summary
#   RADV_PERFTEST              default video_decode
#   TIMEOUT_SECONDS            per decode (default 60)
#
# Exit code: 0 = every scenario matches the software decoder; 1 = mismatch or
# decode failure; 2 = environment/argument failure.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
SHIM_DIR=${SHIM_DIR:-"$REPO_ROOT/cuda_shim"}
STREAM_DIR=${STREAM_DIR:-"$REPO_ROOT/testdata/h264"}
FFMPEG_BUILD_DIR=${FFMPEG_BUILD_DIR:-}
FFMPEG=${FFMPEG:-${FFMPEG_BUILD_DIR:+$FFMPEG_BUILD_DIR/ffmpeg}}
FFMPEG=${FFMPEG:-ffmpeg}
BACKENDS=${BACKENDS:-"vaapi vk"}
MODES=${MODES:-"cuvid nvdec"}
SCENARIOS=${SCENARIOS:-"slices crop seek"}
OUT_DIR=${OUT_DIR:-"$PWD/decode-scenarios"}
EVIDENCE_DIR=${EVIDENCE_DIR:-}
RADV_PERFTEST=${RADV_PERFTEST:-video_decode}
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
             libswresample libavdevice libpostproc; do
        [ -d "$FFMPEG_BUILD_DIR/$d" ] && BUILD_LIBS="$BUILD_LIBS:$FFMPEG_BUILD_DIR/$d"
    done
    LD_LIBRARY_PATH="${BUILD_LIBS#:}:$SHIM_DIR:$ROCM_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
fi
export RADV_PERFTEST

for backend in $BACKENDS; do
    case "$backend" in
        vaapi|vk) ;;
        *) die "unknown backend '$backend' (expected vaapi or vk)" ;;
    esac
done

mkdir -p "$OUT_DIR/logs" || die "cannot create $OUT_DIR"
[ -e "$OUT_DIR/summary.txt" ] && die "$OUT_DIR already contains a summary"

run_timeout() {
    if command -v timeout >/dev/null 2>&1; then
        timeout -s KILL "$TIMEOUT_SECONDS" "$@"
    else
        "$@"
    fi
}

scenario_input() {
    case "$1" in
        slices) echo "$STREAM_DIR/test_slices_320x240.h264" ;;
        crop)   echo "$STREAM_DIR/test_crop_1080p.h264" ;;
        seek)   echo "$STREAM_DIR/test_pts_320x240.mp4" ;;
    esac
}

scenario_extra() {
    case "$1" in
        seek) echo "-ss 0.5" ;;
        *)    echo "" ;;
    esac
}

scenario_frames() {
    case "$1" in
        seek) echo 5 ;;
        *)    echo 0 ;;
    esac
}

{
    echo "date=$(date -Is)"
    echo "ffmpeg=$FFMPEG"
    "$FFMPEG" -hide_banner -version 2>&1 | head -3
    ldd "$FFMPEG" 2>/dev/null | grep -E "libnpp|libcuda|amdhip" || true
} >"$OUT_DIR/environment.log" 2>&1

extract_hashes() {
    # framemd5 rows: stream,dts,pts,duration,size,hash
    awk -F',' '!/^#/ && NF >= 6 { gsub(/ /, "", $5); gsub(/ /, "", $6); print $5, $6 }' "$1"
}

compare_hashes() {
    local ref=$1 cand=$2
    local ref_count cand_count
    ref_count=$(wc -l <"$ref")
    cand_count=$(wc -l <"$cand")
    if [ "$ref_count" -eq 0 ] || [ "$cand_count" -eq 0 ]; then
        echo "frame count mismatch: reference=$ref_count candidate=$cand_count"
        return 1
    fi
    if ! diff -q "$ref" "$cand" >/dev/null; then
        local line
        line=$(diff "$ref" "$cand" | head -1)
        echo "checksum mismatch: $line"
        return 1
    fi
    return 0
}

TOTAL=0
FAILURES=0
: >"$OUT_DIR/summary.txt"

for scenario in $SCENARIOS; do
    input=$(scenario_input "$scenario")
    [ -n "$input" ] || die "unknown scenario '$scenario'"
    [ -f "$input" ] || die "missing stream $input"
    extra=$(scenario_extra "$scenario")
    max_frames=$(scenario_frames "$scenario")
    frames_opt=()
    [ "$max_frames" -gt 0 ] && frames_opt=(-frames:v "$max_frames")

    ref_md5="$OUT_DIR/ref-$scenario.md5"
    # shellcheck disable=SC2086
    run_timeout "$FFMPEG" -hide_banner -nostdin -v error $extra -i "$input" \
        "${frames_opt[@]}" -fps_mode passthrough -pix_fmt yuv420p \
        -f framemd5 -y "$ref_md5" >"$OUT_DIR/logs/ref-$scenario.log" 2>&1 || {
        echo "FAIL: reference decode failed for $scenario" >&2
        FAILURES=$((FAILURES + 1))
        continue
    }
    extract_hashes "$ref_md5" >"$OUT_DIR/ref-$scenario.hashes"

    for mode in $MODES; do
        for backend in $BACKENDS; do
            case_name="$scenario-$mode-$backend"
            shim_md5="$OUT_DIR/$case_name.md5"
            log="$OUT_DIR/logs/$case_name.log"
            case "$mode" in
                cuvid)
                    # shellcheck disable=SC2086
                    run_timeout env CUVID_SHIM_BACKEND=$backend "$FFMPEG" \
                        -hide_banner -nostdin -v error -c:v h264_cuvid \
                        $extra -i "$input" "${frames_opt[@]}" \
                        -fps_mode passthrough -pix_fmt yuv420p \
                        -f framemd5 -y "$shim_md5" >"$log" 2>&1
                    ;;
                nvdec)
                    # shellcheck disable=SC2086
                    run_timeout env CUVID_SHIM_BACKEND=$backend "$FFMPEG" \
                        -hide_banner -nostdin -v error \
                        -hwaccel nvdec -hwaccel_output_format cuda \
                        $extra -i "$input" "${frames_opt[@]}" \
                        -vf hwdownload,format=nv12 \
                        -fps_mode passthrough -pix_fmt yuv420p \
                        -f framemd5 -y "$shim_md5" >"$log" 2>&1
                    ;;
                *) die "unknown mode '$mode'" ;;
            esac
            rc=$?
            if [ $rc -ne 0 ]; then
                echo "FAIL: $case_name exited $rc (see $log)" >&2
                echo "FAIL $case_name exit=$rc" >>"$OUT_DIR/summary.txt"
                FAILURES=$((FAILURES + 1))
                continue
            fi
            extract_hashes "$shim_md5" >"$OUT_DIR/$case_name.hashes"
            if result=$(compare_hashes "$OUT_DIR/ref-$scenario.hashes" "$OUT_DIR/$case_name.hashes"); then
                frames=$(wc -l <"$OUT_DIR/$case_name.hashes")
                echo "PASS: $case_name $frames frames match software"
                echo "PASS $case_name frames=$frames" >>"$OUT_DIR/summary.txt"
                TOTAL=$((TOTAL + 1))
            else
                echo "FAIL: $case_name $result" >&2
                echo "FAIL $case_name $result" >>"$OUT_DIR/summary.txt"
                FAILURES=$((FAILURES + 1))
            fi
        done
    done
done

echo "RESULT: $TOTAL passed, $FAILURES failed" | tee -a "$OUT_DIR/summary.txt"

if [ -n "$EVIDENCE_DIR" ]; then
    mkdir -p "$EVIDENCE_DIR" || die "cannot create $EVIDENCE_DIR"
    [ -e "$EVIDENCE_DIR/summary.txt" ] && die "$EVIDENCE_DIR already has evidence"
    cp "$OUT_DIR/summary.txt" "$OUT_DIR/environment.log" "$EVIDENCE_DIR/"
    cp -r "$OUT_DIR/logs" "$EVIDENCE_DIR/"
    echo "Evidence archived: $EVIDENCE_DIR"
fi

[ "$FAILURES" -eq 0 ] || exit 1
[ "$TOTAL" -gt 0 ] || die "no scenario ran"
echo "PASS: all decode scenarios match the software decoder"
