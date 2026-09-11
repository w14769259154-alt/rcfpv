#!/bin/bash
# ============================================================
# 5gipc-rc: 交叉编译摄像头音频桥(audio_bridge) — OpenIPC ssc338q
# 用法: bash build_audio_bridge.sh
# 产物: dist/audio_bridge (静态, 链接 alsa-lib + opus fixed-point)
# 依赖: 内核需 USB Audio 驱动(自编固件); 板上 /dev/snd/* 由 devtmpfs 自动建
# ============================================================
set -euo pipefail

DIST="$(pwd)/audio/dist"   # 与 workflow upload path: audio/dist/audio_bridge 一致
ROOT="$(pwd)"              # 仓库根(脚本会 cd, 引用仓库内文件用绝对路径)
WORK="$(pwd)/work"
TC_DIR="$WORK/toolchain"
STAGE="$WORK/stage"

mkdir -p "$DIST" "$WORK" "$TC_DIR" "$STAGE"

TOOLCHAIN_URL="https://github.com/openipc/firmware/releases/download/toolchain/toolchain.sigmastar-infinity6e.tgz"
ALSA_VER="1.2.11"
# 用 ALSA 官方发布包(自带 configure), 避免 GitHub 源码需 autoreconf 的坑
ALSA_LIB_URL="https://www.alsa-project.org/files/pub/lib/alsa-lib-${ALSA_VER}.tar.bz2"
OPUS_VER="1.5.2"
# opus 官方发布包(自带 configure)
OPUS_URL="https://downloads.xiph.org/releases/opus/opus-${OPUS_VER}.tar.gz"

echo "==> 构建产物目录: $DIST"

# ---------------- 1. 工具链 ----------------
if [ ! -x "$TC_DIR/bin/arm-openipc-linux-gnueabihf-gcc" ]; then
  echo "==> 下载 OpenIPC 工具链 ..."
  wget -q "$TOOLCHAIN_URL" -O "$WORK/tc.tgz"
  tar -xzf "$WORK/tc.tgz" -C "$TC_DIR" --strip-components=1
fi
# 系统 bin 放最前: 工具链自带残缺 autoreconf(缺 Perl 模块)会让 autoreconf/aclocal 崩溃;
# 交叉编译器用全名调用(arm-openipc-*-gcc), 工具链 bin 保持在 PATH 中即可
export PATH="/usr/bin:/bin:$TC_DIR/bin:$PATH"
CROSS="arm-openipc-linux-gnueabihf-"
"$CROSS"gcc --version | head -1

# ---------------- 2. alsa-lib(动态, 供 audio_bridge 运行时 dlopen) ----------------
# 注意: 完全静态链接的 ALSA 程序 dlopen 机制不可用(snd_dlobj 找不到内建符号),
#       必须动态链接 libasound.so.2(设备 SD 卡 audio/lib/ 已有), 与 build_snd.sh 一致
if [ ! -f "$STAGE/usr/lib/libasound.so" ]; then
  echo "==> 编译 alsa-lib $ALSA_VER (共享库) ..."
  cd "$WORK"
  wget -q "$ALSA_LIB_URL" -O alsa-lib.tar.bz2
  tar -xjf alsa-lib.tar.bz2
  cd alsa-lib-${ALSA_VER}
  ./configure --host="${CROSS%-}" --prefix="$STAGE/usr" \
    --enable-shared --disable-static >/dev/null
  make -j"$(nproc)" >/dev/null && make install >/dev/null
fi

# ---------------- 3. opus(静态, fixed-point) ----------------
if [ ! -f "$STAGE/usr/lib/libopus.a" ]; then
  echo "==> 编译 opus $OPUS_VER (fixed-point) ..."
  cd "$WORK"
  wget -q "$OPUS_URL" -O opus.tar.gz
  tar -xzf opus.tar.gz
  cd opus-${OPUS_VER}
  ./configure --host="${CROSS%-}" --prefix="$STAGE/usr" \
    --enable-static --disable-shared --enable-fixed-point \
    --disable-doc --disable-extra-programs --disable-custom-modes >/dev/null
  make -j"$(nproc)" >/dev/null && make install >/dev/null
fi

# ---------------- 4. 编译 audio_bridge ----------------
echo "==> 编译 audio_bridge(动态链 libasound, opus 静态) ..."
cd "$ROOT/audio"   # 前几步已 cd 到 work/, 必须回到仓库内 audio/ 再编
"$CROSS"gcc -O2 \
  -I"$STAGE/usr/include" -I"$STAGE/usr/include/opus" \
  audio_bridge.c \
  -L"$STAGE/usr/lib" -lasound "$STAGE/usr/lib/libopus.a" -lpthread -lm \
  -o "$DIST/audio_bridge"

# 打包 libasound 共享库(设备 SD 卡 audio/lib/, 与 build_snd.sh 产物同源)
mkdir -p "$DIST/lib"
cp -L "$STAGE/usr/lib"/libasound.so* "$DIST/lib/" 2>/dev/null || true
echo "==> 打包 libasound -> $DIST/lib/"

echo "==> 产物:"
file "$DIST/audio_bridge"
ls -la "$DIST/audio_bridge"
echo "完成。传到设备 /usr/bin/audio_bridge(或 SD 卡), 用法见 audio_bridge.c 头注释"
