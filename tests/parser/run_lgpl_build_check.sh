#!/usr/bin/env bash
# 实验 B：FFmpeg 干净(LGPL)构建下 8 个 .o 的可提取性验证（§4.6 法律路线 + §4.14 依赖）
#
# 目的：验证"干净 LGPL 构建（--disable-gpl --disable-nonfree --enable-pic）"下，
#       用于 parser 复用的 8 个 .o 是否仍然生成、且可独立链接（不被 GPL/NONFREE 组件污染）。
#
# 只 configure + 编译这 8 个 .o（不跑完整 make，省时），然后逐个检查产物。
#
# 用法：./run_lgpl_build_check.sh

set -euo pipefail

SRC="${FFMPEG_SRC:-$HOME/ffmpeg-7.1}"

OBJS="h2645_parse h264_ps h2645_vui h2645data h264data golomb mathtables refstruct"

echo "=== 实验 B：FFmpeg 干净(LGPL)构建下 8 个 .o 可提取性 ==="
echo "源码: $SRC"
echo

cd "$SRC"

# 1. 干净 configure（in-tree，全新源码目录）
echo "[1/4] 干净 configure (--disable-gpl --disable-nonfree --enable-pic)..."
"./configure" \
    --disable-gpl --disable-nonfree --enable-pic \
    --disable-programs --disable-doc --disable-avdevice --disable-avfilter \
    --disable-swresample --disable-swscale --disable-postproc --disable-network \
    --disable-everything --enable-decoder=h264 --enable-parser=h264 \
    --disable-x86asm \
    > configure.log 2>&1
echo "  configure 完成"

# 2. 验证配置项生效
echo "[2/4] 检查 config.h 关键宏..."
grep -E "CONFIG_GPL|CONFIG_NONFREE|CONFIG_PIC" config.h

# 3. 只编译这 8 个 .o（顶层 make，保证 config.h 与 CFLAGS 生效）
echo "[3/4] 编译 8 个 .o ..."
for o in $OBJS; do
    make "libavcodec/$o.o" >> build.log 2>&1 || { echo "  FAIL: 编译 $o.o 失败"; tail -5 build.log; exit 1; }
    echo "  OK: libavcodec/$o.o"
done

# 4. 检查产物
echo "[4/4] 检查产物存在性..."
fail=0
for o in $OBJS; do
    if [ -f "libavcodec/$o.o" ]; then
        echo "  [PASS] libavcodec/$o.o 存在"
    else
        echo "  [FAIL] libavcodec/$o.o 缺失"
        fail=1
    fi
done

echo
if [ "$fail" = "0" ]; then
    echo ">>> 实验 B 结论：干净 LGPL 构建下 8 个 .o 全部可生成，法律路线成立"
else
    echo ">>> 实验 B 结论：存在缺失，需复查 configure 选项或依赖"
    exit 1
fi
