#!/bin/bash
# 编译 NVDEC 解码直测程序
cd "$(dirname "$0")"
g++ -o nvdec_benchmark nvdec_benchmark.cpp \
    -I$HOME/nv-codec-headers/include -ldl -lcuda
echo "编译完成: $(pwd)/nvdec_benchmark"
echo ""
echo "使用步骤:"
echo "  1. demux 出裸码流:"
echo "     ffmpeg -i in.mp4 -c:v copy -bsf:v h264_mp4toannexb -f h264 out.h264"
echo "  2. 运行测试:"
echo "     ./nvdec_benchmark --input out.h264 --codec h264 --json nvdec_h264.json"
echo ""
echo "支持 codec: h264 / hevc / vp9 / av1 / mpeg2"
