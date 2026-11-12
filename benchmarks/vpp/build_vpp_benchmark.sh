#!/bin/bash
# 编译 VAAPI VPP 直测程序
cd "$(dirname "$0")"
gcc -o vpp_benchmark vpp_benchmark.c $(pkg-config --cflags --libs libva libva-drm) -lm
echo "编译完成: $(pwd)/vpp_benchmark"
echo ""
echo "注意: 需在 AMD GPU 机器运行，且用户有 /dev/dri/renderD128 权限"
echo "权限设置: sudo usermod -aG video \$USER 后重新登录"
echo ""
echo "运行示例:"
echo "  ./vpp_benchmark --size 1920x1080 --iter 50 --json vpp_results.json"
