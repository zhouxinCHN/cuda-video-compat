#!/usr/bin/env bash
# FFmpeg 宿主双后端验收入口。
#
# 覆盖两个同级候选后端（VAAPI / Vulkan Video）、四路 P0 码流和四种输出模式：
#   nvdec-safe   : h264_nvdec 默认安全路径（FFmpeg 可写帧拷贝）
#   nvdec-unsafe : AV_HWACCEL_FLAG_UNSAFE_OUTPUT，NVDEC 零 D2D 目标路径
#   cuvid-cpu    : h264_cuvid，CPU(NV12) 输出
#   cuvid-cuda   : h264_cuvid，CUDA 输出后 hwdownload
#
# 每个组合都检查：命令退出码、帧数、输出大小、逐字节像素、显示顺序(PTS)、
# shim D2D 计数，并采样进程 fd/RSS。任何必需模式缺失、全部跳过或证据缺失都失败。
#
# 关键背景：
#   - h264_nvdec 在 FFmpeg 7.1 中是 hwaccel，不是 decoder；
#     正确入口是 `-hwaccel nvdec -hwaccel_output_format cuda`。
#   - h264_cuvid 是 decoder；CUDA 输出需要
#     `-c:v h264_cuvid -hwaccel cuda -hwaccel_output_format cuda`。
#   - h264_nvdec 的默认路径会被 FFmpeg 的 av_frame_make_writable() 拷贝；
#     不能用它证明零拷贝。零拷贝检查只针对 nvdec-unsafe 模式。
#
# 用法：
#   ./run_ffmpeg_compare.sh
#   BACKENDS="vaapi" MODES="nvdec-safe cuvid-cpu" ./run_ffmpeg_compare.sh
#   FFMPEG=$HOME/ffmpeg-7.1/ffmpeg \
#     EVIDENCE_DIR=docs/evidence/ffmpeg-host-2026-11-13 \
#     ./run_ffmpeg_compare.sh
#
# 关键环境变量：
#   BACKENDS                     默认 "vaapi vk"，两个后端同级，不设默认主线
#   MODES                        默认四种模式全跑
#   STREAMS                      P0 四路码流（默认全部）
#   FFMPEG / FFPROBE             可执行文件；默认必须能解析到 FFMPEG_BUILD_DIR
#   FFMPEG_BUILD_DIR             FFmpeg 7.1 源码构建目录；默认自动探测
#   EVIDENCE_DIR                 可选；非空目录拒绝覆盖，只归档日志/摘要，不归档 YUV
#   REQUIRE_NVDEC_ZEROCOPY=1     默认要求 nvdec-unsafe 的 D2D 必须为 0
#   EXTRA_HW_FRAMES=16           nvdec-unsafe 预留给下游/线程持有的额外 surface
#   TIMEOUT_SECONDS=60           单个解码超时
#
# 退出码：0=所有已选组合通过；1=至少一项失败；2=环境/参数/码流准备失败。

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
SHIM_DIR=${SHIM_DIR:-"$REPO_ROOT/cuda_shim"}
STREAM_DIR=${STREAM_DIR:-"$REPO_ROOT/testdata/h264"}
STREAMS=${STREAMS:-"test_p_b_5.h264 test_p_b_7.h264 test_p_main.h264 test_p_66.h264"}
BACKENDS=${BACKENDS:-"vaapi vk"}
MODES=${MODES:-"nvdec-safe nvdec-unsafe cuvid-cpu cuvid-cuda"}
FFMPEG=${FFMPEG:-ffmpeg}
FFPROBE=${FFPROBE:-}
FFMPEG_BUILD_DIR=${FFMPEG_BUILD_DIR:-}
EVIDENCE_DIR=${EVIDENCE_DIR:-}
KEEP=${KEEP:-0}
TIMEOUT_SECONDS=${TIMEOUT_SECONDS:-60}
RESOURCE_SAMPLE_INTERVAL=${RESOURCE_SAMPLE_INTERVAL:-0.02}
REQUIRE_NVDEC_ZEROCOPY=${REQUIRE_NVDEC_ZEROCOPY:-1}
EXTRA_HW_FRAMES=${EXTRA_HW_FRAMES:-16}
RADV_PERFTEST=${RADV_PERFTEST:-video_decode}

die() {
    echo "FAIL: $*" >&2
    exit 2
}

if [ -n "${STREAM:-}" ]; then
    STREAMS=$STREAM
fi

for backend in $BACKENDS; do
    case "$backend" in
        vaapi|vk) ;;
        *) die "unknown backend '$backend' (expected vaapi or vk)" ;;
    esac
done
[ -n "$BACKENDS" ] || die "BACKENDS is empty"

for mode in $MODES; do
    case "$mode" in
        nvdec-safe|nvdec-unsafe|cuvid-cpu|cuvid-cuda) ;;
        *) die "unknown mode '$mode'" ;;
    esac
done
[ -n "$MODES" ] || die "MODES is empty"

[ -f "$SHIM_DIR/libnvcuvid.so.1" ] ||
    die "$SHIM_DIR/libnvcuvid.so.1 not found; build the shim first"
[ -f "$SHIM_DIR/libcuda.so.1" ] ||
    die "$SHIM_DIR/libcuda.so.1 not found; build the shim first"

FFMPEG=$(command -v "$FFMPEG" 2>/dev/null || true)
[ -n "$FFMPEG" ] || die "ffmpeg executable not found"

# 固定 FFmpeg 构建树，避免可执行文件悄悄加载 /usr/local 或系统另一套库。
if [ -z "$FFMPEG_BUILD_DIR" ]; then
    ffmpeg_dir=$(cd "$(dirname "$FFMPEG")" && pwd -P)
    if [ -d "$ffmpeg_dir/libavcodec" ]; then
        FFMPEG_BUILD_DIR=$ffmpeg_dir
    elif [ -d "$HOME/ffmpeg-7.1/libavcodec" ]; then
        FFMPEG_BUILD_DIR=$HOME/ffmpeg-7.1
    else
        die "cannot locate FFmpeg build tree; set FFMPEG_BUILD_DIR=/path/to/ffmpeg-7.1"
    fi
fi
[ -d "$FFMPEG_BUILD_DIR/libavcodec" ] ||
    die "FFMPEG_BUILD_DIR=$FFMPEG_BUILD_DIR has no libavcodec/"

FFMPEG_BUILD_DIR=$(cd "$FFMPEG_BUILD_DIR" && pwd -P)

if [ -z "$FFPROBE" ]; then
    ffprobe_candidate=$(dirname "$FFMPEG")/ffprobe
    if [ -x "$ffprobe_candidate" ]; then
        FFPROBE=$ffprobe_candidate
    else
        FFPROBE=$(command -v ffprobe 2>/dev/null || true)
    fi
fi
[ -n "$FFPROBE" ] || die "ffprobe executable not found"
FFPROBE=$(command -v "$FFPROBE" 2>/dev/null || true)
[ -n "$FFPROBE" ] || die "ffprobe executable not found"

ffmpeg_lib_path=""
for libdir in \
    "$FFMPEG_BUILD_DIR/libavdevice" \
    "$FFMPEG_BUILD_DIR/libavfilter" \
    "$FFMPEG_BUILD_DIR/libavformat" \
    "$FFMPEG_BUILD_DIR/libavcodec" \
    "$FFMPEG_BUILD_DIR/libswresample" \
    "$FFMPEG_BUILD_DIR/libswscale" \
    "$FFMPEG_BUILD_DIR/libavutil"; do
    [ -d "$libdir" ] || continue
    if [ -n "$ffmpeg_lib_path" ]; then
        ffmpeg_lib_path="$ffmpeg_lib_path:$libdir"
    else
        ffmpeg_lib_path=$libdir
    fi
done
[ -n "$ffmpeg_lib_path" ] || die "no FFmpeg shared-library directories found"

export LD_LIBRARY_PATH="$SHIM_DIR:$ffmpeg_lib_path:${LD_LIBRARY_PATH:-}"
export ROCM_PATH=${ROCM_PATH:-/opt/rocm}
export RADV_PERFTEST

# FFmpeg 必须实际加载 FFMPEG_BUILD_DIR 里的 libavcodec，而不是系统副本。
check_libavcodec_pin() {
    local exe=$1
    local resolved
    resolved=$(ldd "$exe" 2>/dev/null |
        awk '/libavcodec\.so/ {print $3; exit}')
    [ -n "$resolved" ] || die "cannot resolve libavcodec for $exe"
    resolved=$(readlink -f "$resolved")
    case "$resolved" in
        "$FFMPEG_BUILD_DIR"/libavcodec/*) ;;
        *) die "$exe is loading unexpected libavcodec: $resolved" ;;
    esac
}
check_libavcodec_pin "$FFMPEG"
check_libavcodec_pin "$FFPROBE"

if ! "$FFMPEG" -version >/dev/null 2>&1; then
    die "ffmpeg cannot start with pinned FFmpeg libraries"
fi

if printf '%s\n' "$MODES" | grep -qwE 'cuvid-(cpu|cuda)'; then
    "$FFMPEG" -hide_banner -decoders 2>/dev/null | grep -qw 'h264_cuvid' ||
        die "h264_cuvid decoder missing from pinned FFmpeg build"
fi

for entry in $STREAMS; do
    if [ -f "$entry" ]; then
        :
    elif [ -f "$STREAM_DIR/$entry" ]; then
        :
    elif [ -f "$STREAM_DIR/$entry.h264" ]; then
        :
    else
        die "stream not found: $entry (also tried $STREAM_DIR/$entry and .h264)"
    fi
done

if [ -n "$EVIDENCE_DIR" ]; then
    mkdir -p "$EVIDENCE_DIR" || die "cannot create EVIDENCE_DIR=$EVIDENCE_DIR"
    if [ -n "$(find "$EVIDENCE_DIR" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]; then
        die "EVIDENCE_DIR=$EVIDENCE_DIR is not empty; refusing to overwrite evidence"
    fi
fi

if [ -n "${WORK:-}" ]; then
    mkdir -p "$WORK" || die "cannot create WORK=$WORK"
    WORK_TEMP=0
else
    WORK=$(mktemp -d /tmp/shim-ffmpeg-host-XXXXXX) || die "mktemp failed"
    WORK_TEMP=1
fi

SUMMARY_TSV="$WORK/summary.tsv"
SUMMARY_MD="$WORK/summary.md"
ENV_LOG="$WORK/environment.log"
COMMAND_LOG="$WORK/commands.log"
: >"$SUMMARY_TSV"
: >"$COMMAND_LOG"
printf 'backend\tmode\tstream\trc\tref_frames\tout_frames\tpixel\tpts\td2d\tfd_first\tfd_last\tfd_max\trss_first_kib\trss_last_kib\trss_max_kib\tresult\treason\n' \
    >"$SUMMARY_TSV"

cleanup() {
    if [ "$KEEP" = "1" ] || [ "$WORK_TEMP" = "0" ]; then
        echo "WORK kept: $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

quote_command() {
    local arg
    for arg in "$@"; do
        printf '%q ' "$arg"
    done
    printf '\n'
}

record_command() {
    local label=$1
    shift
    {
        printf '%s\t' "$label"
        quote_command "$@"
    } >>"$COMMAND_LOG"
}

extract_pts() {
    awk '
        /Parsed_showinfo_/ && / n:/ {
            n = ""; pts = ""; pts_time = "";
            for (i = 1; i <= NF; i++) {
                if ($i == "n:") n = $(i + 1);
                else if ($i == "pts:") pts = $(i + 1);
                else if ($i ~ /^pts_time:/) {
                    pts_time = $i;
                    sub(/^pts_time:/, "", pts_time);
                }
            }
            if (n != "") print n "\t" pts "\t" pts_time;
        }
    ' "$1"
}

metric_value() {
    local metrics=$1
    local key=$2
    local value
    value=$(printf '%s\n' "$metrics" | tr ' ' '\n' |
        awk -F= -v wanted="$key" '$1 == wanted {print $2}' | tail -1)
    printf '%s' "${value:-NA}"
}

# 启动实际 FFmpeg 进程并采样 /proc/<pid>/{fd,status}。
# 参数：stderr-log stdout-file command...
run_monitored() {
    local log=$1
    local stdout_file=$2
    shift 2

    local pid rc start_ns now_ns elapsed_ms
    local fd_first=0 fd_last=0 fd_max=0 fd_count
    local rss_first=0 rss_last=0 rss_max=0 rss
    local timed_out=0

    "$@" >"$stdout_file" 2>"$log" &
    pid=$!
    start_ns=$(date +%s%N)

    while kill -0 "$pid" 2>/dev/null; do
        fd_count=$(find "/proc/$pid/fd" -mindepth 1 -maxdepth 1 2>/dev/null |
            wc -l | tr -d ' ')
        if [ -n "$fd_count" ]; then
            [ "$fd_first" -eq 0 ] && fd_first=$fd_count
            fd_last=$fd_count
            [ "$fd_count" -gt "$fd_max" ] && fd_max=$fd_count
        fi
        rss=$(awk '/^VmRSS:/ {print $2; exit}' "/proc/$pid/status" 2>/dev/null || true)
        if [ -n "$rss" ]; then
            [ "$rss_first" -eq 0 ] && rss_first=$rss
            rss_last=$rss
            [ "$rss" -gt "$rss_max" ] && rss_max=$rss
        fi

        now_ns=$(date +%s%N)
        elapsed_ms=$(( (now_ns - start_ns) / 1000000 ))
        if [ "$TIMEOUT_SECONDS" -gt 0 ] &&
           [ "$elapsed_ms" -ge $((TIMEOUT_SECONDS * 1000)) ]; then
            timed_out=1
            kill -TERM "$pid" 2>/dev/null || true
            for _ in $(seq 1 100); do
                kill -0 "$pid" 2>/dev/null || break
                sleep 0.02
            done
            kill -KILL "$pid" 2>/dev/null || true
            break
        fi
        sleep "$RESOURCE_SAMPLE_INTERVAL"
    done

    wait "$pid"
    rc=$?
    now_ns=$(date +%s%N)
    elapsed_ms=$(( (now_ns - start_ns) / 1000000 ))
    if [ "$timed_out" -eq 1 ]; then
        rc=124
    fi

    printf 'HARNESS_RESOURCE rc=%s elapsed_ms=%s fd_first=%s fd_last=%s fd_max=%s rss_first_kib=%s rss_last_kib=%s rss_max_kib=%s\n' \
        "$rc" "$elapsed_ms" "$fd_first" "$fd_last" "$fd_max" \
        "$rss_first" "$rss_last" "$rss_max" >>"$log"
    return "$rc"
}

resolve_stream() {
    local entry=$1
    if [ -f "$entry" ]; then
        printf '%s' "$entry"
    elif [ -f "$STREAM_DIR/$entry" ]; then
        printf '%s' "$STREAM_DIR/$entry"
    else
        printf '%s' "$STREAM_DIR/$entry.h264"
    fi
}

write_environment() {
    {
        echo "=== date ==="
        date -Is
        echo
        echo "=== repository ==="
        git -C "$REPO_ROOT" rev-parse HEAD 2>&1 || true
        git -C "$REPO_ROOT" status --short 2>&1 || true
        echo
        echo "=== system ==="
        uname -a
        lspci -nnk 2>/dev/null | grep -A3 -Ei 'vga|3d|display' || true
        echo
        echo "=== ffmpeg ==="
        "$FFMPEG" -version
        "$FFMPEG" -hwaccels
        "$FFMPEG" -decoders 2>/dev/null | grep -E 'h264|cuvid' || true
        grep '^#define FFMPEG_CONFIGURATION' \
            "$FFMPEG_BUILD_DIR/config.h" 2>/dev/null || true
        echo
        echo "=== linked FFmpeg libraries ==="
        ldd "$FFMPEG"
        echo
        echo "=== ROCm / VAAPI / Vulkan ==="
        if command -v rocminfo >/dev/null 2>&1; then
            timeout 10s rocminfo 2>&1 |
                grep -E 'ROCm-SMI|Name:|gfx|Marketing Name' | head -40 || true
        fi
        if command -v vainfo >/dev/null 2>&1; then
            timeout 10s vainfo 2>&1 | head -40 || true
        fi
        if command -v vulkaninfo >/dev/null 2>&1; then
            timeout 10s vulkaninfo --summary 2>&1 | head -80 || true
        fi
        echo
        echo "=== artifacts / inputs ==="
        sha256sum "$FFMPEG" "$FFPROBE" \
            "$SHIM_DIR/libcuda.so.1" "$SHIM_DIR/libnvcuvid.so.1" \
            "$SHIM_DIR/src/"*.cpp "$SCRIPT_DIR/run_ffmpeg_compare.sh" \
            2>/dev/null || true
        local entry
        for entry in $STREAMS; do
            sha256sum "$(resolve_stream "$entry")" 2>/dev/null || true
        done
        echo
        echo "=== matrix ==="
        printf 'BACKENDS=%s\nMODES=%s\nSTREAMS=%s\n' \
            "$BACKENDS" "$MODES" "$STREAMS"
        printf 'TIMEOUT_SECONDS=%s\nREQUIRE_NVDEC_ZEROCOPY=%s\nEXTRA_HW_FRAMES=%s\n' \
            "$TIMEOUT_SECONDS" "$REQUIRE_NVDEC_ZEROCOPY" "$EXTRA_HW_FRAMES"
    } >"$ENV_LOG" 2>&1
}

write_environment

PASS=0
FAIL=0
RUNS=0

echo "=== FFmpeg host acceptance matrix ==="
echo "  shim        : $SHIM_DIR"
echo "  ffmpeg      : $FFMPEG"
echo "  ffmpeg build: $FFMPEG_BUILD_DIR"
echo "  backends    : $BACKENDS"
echo "  modes       : $MODES"
echo "  streams     : $STREAMS"
echo "  work        : $WORK"

for stream_entry in $STREAMS; do
    stream=$(resolve_stream "$stream_entry")
    stream_name=$(basename "$stream")
    stream_stem=${stream_name%.h264}

    width=$("$FFPROBE" -v error -select_streams v:0 \
        -show_entries stream=width -of csv=p=0 "$stream" 2>/dev/null | head -1)
    height=$("$FFPROBE" -v error -select_streams v:0 \
        -show_entries stream=height -of csv=p=0 "$stream" 2>/dev/null | head -1)
    if [ -z "$width" ] || [ -z "$height" ] ||
       [ "$width" -le 0 ] 2>/dev/null || [ "$height" -le 0 ] 2>/dev/null; then
        die "ffprobe cannot read dimensions from $stream"
    fi
    frame_bytes=$(( width * height * 3 / 2 ))

    input_has_pts=0
    if "$FFPROBE" -v error -select_streams v:0 \
        -show_entries packet=pts -of csv=p=0 "$stream" 2>/dev/null |
        awk 'NF && $0 != "N/A" && $0 ~ /^-?[0-9]+$/ {found = 1}
             END {exit !found}'; then
        input_has_pts=1
    fi

    ref_out="$WORK/reference__${stream_stem}.nv12"
    ref_log="$WORK/reference__${stream_stem}.log"
    ref_pts="$WORK/reference__${stream_stem}.pts"

    echo
    echo "--- reference: $stream (${width}x${height}) ---"
    ref_cmd=(
        "$FFMPEG" -nostdin -hide_banner -nostats -loglevel info -y
        -c:v h264 -i "$stream" -map 0:v:0 -vf showinfo
        -fps_mode passthrough -f rawvideo -pix_fmt nv12
        -an -sn -dn "$ref_out"
    )
    record_command "reference/$stream_name" "${ref_cmd[@]}"
    run_monitored "$ref_log" /dev/null "${ref_cmd[@]}"
    ref_rc=$?
    if [ "$ref_rc" -ne 0 ] || [ ! -f "$ref_out" ]; then
        die "software reference decode failed for $stream (rc=$ref_rc, log=$ref_log)"
    fi

    ref_size=$(stat -c%s "$ref_out")
    if [ "$ref_size" -eq 0 ] || [ $((ref_size % frame_bytes)) -ne 0 ]; then
        die "reference size $ref_size is not a whole number of ${width}x${height} frames"
    fi
    ref_frames=$(( ref_size / frame_bytes ))
    extract_pts "$ref_log" >"$ref_pts"
    ref_pts_frames=$(wc -l <"$ref_pts" | tr -d ' ')
    if [ "$input_has_pts" -eq 1 ] && [ "$ref_pts_frames" -ne "$ref_frames" ]; then
        die "reference PTS count $ref_pts_frames != frame count $ref_frames"
    fi
    ref_sha=$(sha256sum "$ref_out" | awk '{print $1}')
    printf '  reference: frames=%s bytes=%s sha256=%s input_pts=%s\n' \
        "$ref_frames" "$ref_size" "$ref_sha" \
        "$([ "$input_has_pts" -eq 1 ] && echo available || echo absent)"

    for backend in $BACKENDS; do
        for mode in $MODES; do
            pre_args=()
            filter="showinfo"
            require_zero_copy=0
            case "$mode" in
                nvdec-safe)
                    pre_args=(-hwaccel nvdec -hwaccel_output_format cuda)
                    filter="hwdownload,format=nv12,showinfo"
                    ;;
                nvdec-unsafe)
                    pre_args=(
                        -hwaccel nvdec
                        -hwaccel_output_format cuda
                        -hwaccel_flags unsafe_output
                        -extra_hw_frames "$EXTRA_HW_FRAMES"
                    )
                    filter="hwdownload,format=nv12,showinfo"
                    require_zero_copy=$REQUIRE_NVDEC_ZEROCOPY
                    ;;
                cuvid-cpu)
                    pre_args=(-c:v h264_cuvid)
                    ;;
                cuvid-cuda)
                    pre_args=(
                        -c:v h264_cuvid
                        -hwaccel cuda
                        -hwaccel_output_format cuda
                    )
                    filter="hwdownload,format=nv12,showinfo"
                    ;;
            esac

            label="${backend}/${mode}/${stream_name}"
            out="$WORK/${backend}__${mode}__${stream_stem}.nv12"
            log="$WORK/${backend}__${mode}__${stream_stem}.log"
            pts="$WORK/${backend}__${mode}__${stream_stem}.pts"
            rm -f "$out" "$log" "$pts"

            export CUVID_SHIM_BACKEND="$backend"
            export CUDA_SHIM_TRACE_COPY=1

            cmd=(
                "$FFMPEG" -nostdin -hide_banner -nostats -loglevel info -y
                "${pre_args[@]}"
                -i "$stream" -map 0:v:0 -vf "$filter"
                -fps_mode passthrough -f rawvideo -pix_fmt nv12
                -an -sn -dn "$out"
            )
            record_command \
                "decode/$label" \
                env "CUVID_SHIM_BACKEND=$backend" \
                "CUDA_SHIM_TRACE_COPY=1" \
                "RADV_PERFTEST=$RADV_PERFTEST" \
                "${cmd[@]}"

            echo
            echo "--- $label ---"
            run_monitored "$log" /dev/null "${cmd[@]}"
            rc=$?
            RUNS=$((RUNS + 1))

            reasons=()
            out_frames=0
            if [ -f "$out" ]; then
                out_size=$(stat -c%s "$out")
                out_frames=$(( out_size / frame_bytes ))
                if [ "$out_size" -ne "$ref_size" ]; then
                    reasons+=("size=${out_size}/${ref_size}")
                fi
            else
                reasons+=("output-missing")
            fi

            pixel_ok=FAIL
            if [ -f "$out" ] && [ "$out_frames" -eq "$ref_frames" ] &&
               cmp -s "$out" "$ref_out"; then
                pixel_ok=PASS
            else
                reasons+=("pixel-mismatch")
            fi

            extract_pts "$log" >"$pts"
            if [ "$input_has_pts" -eq 1 ]; then
                pts_ok=FAIL
                if [ -s "$pts" ] && cmp -s "$pts" "$ref_pts"; then
                    pts_ok=PASS
                else
                    reasons+=("pts-or-display-order-mismatch")
                fi
            else
                # 裸 Annex-B 没有输入 PTS；此时像素按输出顺序逐帧一致，
                # 已经证明显示顺序。软件解码器补出的 PTS 不能要求 CUVID 相同。
                pts_ok="NA(no-input-pts)"
            fi
            if [ "$out_frames" -ne "$ref_frames" ]; then
                reasons+=("frames=${out_frames}/${ref_frames}")
            fi

            d2d_2d=$(sed -n 's/.*copy-trace: cuMemcpy2D\* D2D=\([0-9][0-9]*\).*/\1/p' \
                "$log" 2>/dev/null | tail -1)
            d2d_dtod=$(sed -n 's/.*cuMemcpyDtoD\*=\([0-9][0-9]*\).*/\1/p' \
                "$log" 2>/dev/null | tail -1)
            d2d=NA
            copy_ok=1
            if [[ "$d2d_2d" =~ ^[0-9]+$ ]] && [[ "$d2d_dtod" =~ ^[0-9]+$ ]]; then
                d2d=$((d2d_2d + d2d_dtod))
            else
                reasons+=("d2d-count-missing")
                copy_ok=0
            fi
            if [ "$copy_ok" -eq 1 ] && [ "$require_zero_copy" = "1" ] &&
               [ "$d2d" -ne 0 ]; then
                reasons+=("d2d=${d2d}-expected-0(2d=${d2d_2d},dtod=${d2d_dtod})")
                copy_ok=0
            fi

            metrics=$(sed -n 's/^HARNESS_RESOURCE //p' "$log" 2>/dev/null | tail -1)
            fd_first=$(metric_value "$metrics" fd_first)
            fd_last=$(metric_value "$metrics" fd_last)
            fd_max=$(metric_value "$metrics" fd_max)
            rss_first=$(metric_value "$metrics" rss_first_kib)
            rss_last=$(metric_value "$metrics" rss_last_kib)
            rss_max=$(metric_value "$metrics" rss_max_kib)

            if [ "$rc" -ne 0 ]; then
                reasons+=("ffmpeg-rc=$rc")
            fi

            if [ "${#reasons[@]}" -eq 0 ] && [ "$copy_ok" -eq 1 ]; then
                result=PASS
                PASS=$((PASS + 1))
                reason="ok"
                printf '  [PASS] frames=%s d2d=%s fd=%s->%s max=%s rss=%s->%s max=%s KiB\n' \
                    "$out_frames" "$d2d" "$fd_first" "$fd_last" "$fd_max" \
                    "$rss_first" "$rss_last" "$rss_max"
            else
                result=FAIL
                FAIL=$((FAIL + 1))
                reason=$(IFS=';'; echo "${reasons[*]}")
                printf '  [FAIL] frames=%s/%s pixel=%s pts=%s d2d=%s fd=%s->%s max=%s rss_max=%s KiB reason=%s\n' \
                    "$out_frames" "$ref_frames" "$pixel_ok" "$pts_ok" "$d2d" \
                    "$fd_first" "$fd_last" "$fd_max" "$rss_max" "$reason"
                printf '         log: %s\n' "$log"
            fi

            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                "$backend" "$mode" "$stream_name" "$rc" "$ref_frames" \
                "$out_frames" "$pixel_ok" "$pts_ok" "$d2d" \
                "$fd_first" "$fd_last" "$fd_max" \
                "$rss_first" "$rss_last" "$rss_max" \
                "$result" "$reason" >>"$SUMMARY_TSV"
        done
    done
done

{
    echo "# FFmpeg 宿主双后端验收基线"
    echo
    echo "> 由 \`tests/quick-verify/run_ffmpeg_compare.sh\` 自动生成。"
    echo "> 生成时间：$(date -Is)"
    echo "> 代码提交：\`$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)\`"
    echo "> FFmpeg：\`$FFMPEG\`"
    echo "> FFmpeg 构建树：\`$FFMPEG_BUILD_DIR\`"
    echo "> 后端：\`$BACKENDS\`（同级候选，无默认主线）"
    echo "> 模式：\`$MODES\`"
    echo
    echo "## 结果"
    echo
    echo "| 后端 | 模式 | 码流 | 帧数 | 像素 | PTS/顺序 | D2D | fd first→last (max) | RSS max KiB | 结果 | 原因 |"
    echo "|---|---|---|---:|---|---|---:|---|---:|---|---|"
    awk -F '\t' 'NR > 1 {
        reason = $17; gsub(/\|/, "\\|", reason);
        printf "| %s | %s | %s | %s/%s | %s | %s | %s | %s→%s (%s) | %s | **%s** | %s |\n",
            $1, $2, $3, $6, $5, $7, $8, $9, $10, $11, $12, $15, $16, reason
    }' "$SUMMARY_TSV"
    echo
    echo "## 判据与边界"
    echo
    echo "- PASS 必须同时满足：FFmpeg 退出 0、帧数一致、输出大小一致、逐字节像素一致、D2D 计数可读；输入含 PTS 时还要求 PTS/显示顺序一致。"
    echo "- 裸 Annex-B P0 码流没有输入 PTS，PTS 一栏记 \`NA(no-input-pts)\`；输出与软件参考逐帧逐字节一致即为显示顺序证据。"
    echo "- \`nvdec-unsafe\` 在 \`REQUIRE_NVDEC_ZEROCOPY=1\` 时还要求 D2D=0。"
    echo "- \`nvdec-safe\` 的 D2D 是 FFmpeg 可写帧/回读路径的真实行为，只记录，不能作为零拷贝证据。"
    echo "- D2D 只统计 shim 的 \`cuMemcpy2D*\`/\`cuMemcpyDtoD*\` 调用，不含驱动内部拷贝或 CPU 回读。"
    echo "- fd/RSS 是短时采样的首值、末值和峰值，不是长稳泄漏结论。"
    echo "- 结论只适用于本机、当前提交、当前 FFmpeg 构建和所列码流，不代替目标平台及业务验收。"
} >"$SUMMARY_MD"

if [ -n "$EVIDENCE_DIR" ]; then
    find "$WORK" -maxdepth 1 -type f \
        \( -name '*.log' -o -name '*.pts' -o -name 'summary.tsv' \
           -o -name 'summary.md' -o -name 'commands.log' \
           -o -name 'environment.log' \) \
        -exec cp {} "$EVIDENCE_DIR/" \;
    echo "Evidence archived: $EVIDENCE_DIR"
fi

echo
echo "=== summary: PASS=$PASS FAIL=$FAIL RUN=$RUNS SKIP=0 ==="
if [ "$RUNS" -eq 0 ]; then
    echo "FAIL: no checks were executed" >&2
    exit 1
fi
if [ "$FAIL" -ne 0 ]; then
    echo "One or more required checks failed." >&2
    exit 1
fi
echo "PASS: FFmpeg host matrix passed for all selected backends/modes/streams"
