#!/usr/bin/env bash
# Decode soak test: 1-way and 8-way looping decode with resource sampling.
#
# Runs FFmpeg in one process per configuration, loops a 1080p stream for the
# requested duration and samples RSS and fd count. Exit status, RSS growth and
# fd growth are checked; the full P0 soak uses DURATION_SECONDS=1800.
#
# Usage:
#   FFMPEG_BUILD_DIR=/path/to/ffmpeg ./run_stability.sh
#   DURATION_SECONDS=60 STREAMS="1 8" ./run_stability.sh
#
# Environment:
#   FFMPEG / FFMPEG_BUILD_DIR  ffmpeg with libx264 (stream generation)
#   BACKENDS                   default "vaapi vk"
#   STREAMS                    default "1 8"
#   DURATION_SECONDS           default 1800
#   RSS_GROWTH_LIMIT_MB        default 64
#   FD_GROWTH_LIMIT            default 8
#   OUT_DIR                    default ./stability
#   EVIDENCE_DIR               optional
#   RADV_PERFTEST              default video_decode
#
# Exit code: 0 = every configuration completed without resource growth; 1 =
# failure; 2 = setup.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
FFMPEG_BUILD_DIR=${FFMPEG_BUILD_DIR:-}
FFMPEG=${FFMPEG:-${FFMPEG_BUILD_DIR:+$FFMPEG_BUILD_DIR/ffmpeg}}
FFMPEG=${FFMPEG:-ffmpeg}
BACKENDS=${BACKENDS:-"vaapi vk"}
STREAMS=${STREAMS:-"1 8"}
DURATION_SECONDS=${DURATION_SECONDS:-1800}
RSS_GROWTH_LIMIT_MB=${RSS_GROWTH_LIMIT_MB:-64}
FD_GROWTH_LIMIT=${FD_GROWTH_LIMIT:-8}
OUT_DIR=${OUT_DIR:-"$PWD/stability"}
EVIDENCE_DIR=${EVIDENCE_DIR:-}
RADV_PERFTEST=${RADV_PERFTEST:-video_decode}
SAMPLE_INTERVAL=${SAMPLE_INTERVAL:-5}
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

# MP4 so -stream_loop can seek back to the start; the raw H.264 demuxer cannot.
STREAM="$OUT_DIR/soak-1920x1080.mp4"
if [ ! -f "$STREAM" ]; then
    "$FFMPEG" -hide_banner -nostdin -v error \
        -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=2" \
        -vf format=yuv420p -frames:v 60 -c:v libx264 -preset ultrafast \
        -x264-params "keyint=30:bframes=2:scenecut=0" -y "$STREAM" || \
        die "failed to generate soak stream"
fi

: >"$OUT_DIR/summary.txt"
PASSES=0
FAILURES=0

for backend in $BACKENDS; do
    case "$backend" in
        vaapi|vk) ;;
        *) die "unknown backend '$backend'" ;;
    esac
    for count in $STREAMS; do
        case_name="stability-${count}x-${backend}"
        log="$OUT_DIR/$case_name.log"
        samples="$OUT_DIR/$case_name.samples"
        args=()
        for _ in $(seq 1 "$count"); do
            args+=(-stream_loop -1 -t "$DURATION_SECONDS" -c:v h264_cuvid -i "$STREAM")
        done
        args+=(-map 0:v -f null /dev/null)
        for index in $(seq 1 $((count - 1))); do
            args+=(-map "$index:v" -f null /dev/null)
        done

        echo "RUN: $case_name for ${DURATION_SECONDS}s"
        env CUVID_SHIM_BACKEND=$backend "$FFMPEG" -hide_banner -nostdin -v error \
            "${args[@]}" >"$log" 2>&1 &
        pid=$!
        : >"$samples"
        while kill -0 "$pid" 2>/dev/null; do
            rss=$(awk '/^VmRSS:/ {print $2}' "/proc/$pid/status" 2>/dev/null)
            fds=$(ls "/proc/$pid/fd" 2>/dev/null | wc -l)
            [ -n "${rss:-}" ] && echo "$(date +%s) ${rss} ${fds}" >>"$samples"
            sleep "$SAMPLE_INTERVAL"
        done
        wait "$pid"
        rc=$?
        if [ $rc -ne 0 ]; then
            echo "FAIL: $case_name exited $rc (see $log)" >&2
            echo "FAIL $case_name exit=$rc" >>"$OUT_DIR/summary.txt"
            FAILURES=$((FAILURES + 1))
            continue
        fi

        # Baseline is the first sample after warm-up: FFmpeg allocates frame
        # pools and device handles during the first seconds, and the sampler can
        # catch the process before /proc reports a non-zero RSS.
        warmup=$(( DURATION_SECONDS / 10 ))
        [ "$warmup" -lt 10 ] && warmup=10
        read -r first_rss first_fd last_rss last_fd max_rss max_fd < <(
            awk -v warm="$warmup" '
                NR == 1 { start = $1 }
                $2 > 0 {
                    last_rss = $2; last_fd = $3
                    if (base_rss == 0 && $1 >= start + warm) { base_rss = $2; base_fd = $3 }
                    if ($2 > max_rss) max_rss = $2
                    if ($3 > max_fd) max_fd = $3
                }
                END {
                    if (base_rss == 0) { base_rss = last_rss; base_fd = last_fd }
                    print base_rss, base_fd, last_rss, last_fd, max_rss, max_fd
                }
            ' "$samples"
        )
        if [ -z "${first_rss:-}" ]; then
            echo "FAIL: $case_name produced no samples" >&2
            echo "FAIL $case_name no-samples" >>"$OUT_DIR/summary.txt"
            FAILURES=$((FAILURES + 1))
            continue
        fi

        rss_growth_mb=$(( (last_rss - first_rss) / 1024 ))
        fd_growth=$(( last_fd - first_fd ))
        echo "INFO: $case_name rss ${first_rss}->${last_rss} KiB (max ${max_rss}), fd ${first_fd}->${last_fd} (max ${max_fd})"
        if [ "$rss_growth_mb" -gt "$RSS_GROWTH_LIMIT_MB" ] || [ "$fd_growth" -gt "$FD_GROWTH_LIMIT" ]; then
            echo "FAIL: $case_name grew rss=${rss_growth_mb}MiB fd=${fd_growth}" >&2
            echo "FAIL $case_name rss_growth_mib=$rss_growth_mb fd_growth=$fd_growth" >>"$OUT_DIR/summary.txt"
            FAILURES=$((FAILURES + 1))
        else
            echo "PASS: $case_name ${DURATION_SECONDS}s rss_growth=${rss_growth_mb}MiB fd_growth=${fd_growth}"
            echo "PASS $case_name duration=${DURATION_SECONDS}s rss_growth_mib=$rss_growth_mb fd_growth=$fd_growth max_rss_kib=$max_rss max_fd=$max_fd" >>"$OUT_DIR/summary.txt"
            PASSES=$((PASSES + 1))
        fi
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
echo "PASS: soak completed without resource growth"
