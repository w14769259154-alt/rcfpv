#!/bin/bash
# ============================================================
# 5gipc-rc: x86_64 构建 VPS edge(验证 + 产出二进制,VPS 可直接用)
# 用法: bash build_vps_edge.sh
# 产物: dist/rcfpv-edge (x86_64 静态链 libdatachannel, 动态链 FFmpeg)
# ============================================================
set -euo pipefail

DIST="$(pwd)/vps-edge/dist"   # 与 workflow upload path: vps-edge/dist/rcfpv-edge 一致
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
# 日志重定向: 编译输出几十万行会被 GitHub 截断, 失败时只 tail 出错误尾部
if ! cmake -B build -DCMAKE_BUILD_TYPE=Release > "$WORK/cmake.log" 2>&1; then
  echo "!! cmake 配置失败, 尾部日志:"; tail -60 "$WORK/cmake.log"; exit 1
fi
if ! cmake --build build -j"$(nproc)" > "$WORK/build.log" 2>&1; then
  echo "!! 编译失败, 尾部日志:"; tail -80 "$WORK/build.log"; exit 1
fi

cp build/rcfpv-edge "$DIST/"
echo "==> 产物: $DIST/rcfpv-edge"
file "$DIST/rcfpv-edge"
ls -la "$DIST"
echo "完成。传到 VPS /opt/rcfpv-edge/build/rcfpv-edge 或直接用 deploy_vps.sh"
