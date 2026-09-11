#!/bin/bash
# ============================================================
# 5gipc-rc: x86_64 构建 VPS edge(验证 + 产出二进制,VPS 可直接用)
# 用法: bash build_vps_edge.sh
# 产物: dist/rcfpv-edge (x86_64 静态链 libdatachannel, 动态链 FFmpeg)
# ============================================================
set -euo pipefail

DIST="$(pwd)/dist"
WORK="$(pwd)/work"
SRC="$(cd "$(dirname "$0")" && pwd)"

mkdir -p "$DIST" "$WORK"

echo "==> libdatachannel v0.24.5 ..."
if [ ! -d "$WORK/libdatachannel" ]; then
  git clone --depth 1 --branch v0.24.5 --recurse-submodules \
    https://github.com/paullouisageneau/libdatachannel.git "$WORK/libdatachannel"
fi

echo "==> 组装 edge 源码 ..."
rm -rf "$WORK/edge-src"
mkdir -p "$WORK/edge-src/src"
cp -r "$SRC/src/." "$WORK/edge-src/src/"
cp "$SRC/CMakeLists.txt" "$WORK/edge-src/"

echo "==> 编译 ..."
cd "$WORK/edge-src"
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

cp build/rcfpv-edge "$DIST/"
echo "==> 产物: $DIST/rcfpv-edge"
file "$DIST/rcfpv-edge"
ls -la "$DIST"
echo "完成。传到 VPS /opt/rcfpv-edge/build/rcfpv-edge 或直接用 deploy_vps.sh"
