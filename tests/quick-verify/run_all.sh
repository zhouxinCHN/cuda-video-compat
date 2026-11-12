#!/usr/bin/env bash
# 一次跑完 quick_verify 的两组验证。
# 用法：BACKENDS="vaapi" ./run_all.sh

set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
FAIL=0

echo "############ 1. mapped dev_ptr kernel read ############"
"$SCRIPT_DIR/run_kernel_read.sh" || FAIL=1

echo
echo "############ 2. FFmpeg pixel / zero-copy ############"
"$SCRIPT_DIR/run_ffmpeg_compare.sh" || FAIL=1

echo
if [ "$FAIL" -eq 0 ]; then
    echo "ALL QUICK CHECKS PASSED"
else
    echo "SOME QUICK CHECKS FAILED" >&2
fi
exit "$FAIL"
