#!/bin/bash
# 编译 NPP 直测程序
cd "$(dirname "$0")"
nvcc -o npp_benchmark npp_benchmark.cpp \
    -lnppc -lnppig -lnppif -lnppicc -lnppidei -lnppim -lnppitc \
    -lnppial -lnppist -lnpps -lcudart
echo "编译完成: $(pwd)/npp_benchmark"
echo "运行示例:"
echo "  ./npp_benchmark --size 1920x1080 --iter 50 --json npp_results.json"
echo "  ./npp_benchmark --size 3840x2160 --iter 20 --json npp_4k_results.json"
