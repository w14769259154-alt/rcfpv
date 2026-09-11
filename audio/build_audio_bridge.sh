#!/bin/bash
# ============================================================
# 5gipc-rc: 交叉编译摄像头音频桥(audio_bridge) — OpenIPC ssc338q
# 用法: bash build_audio_bridge.sh
# 产物: dist/audio_bridge (静态, 链接 alsa-lib + opus fixed-point)
# 依赖: 内核需 USB Audio 驱动(自编固件); 板上 /dev/snd/* 由 devtmpfs 自动建
# ============================================================
set -euo pipefail

DIST="$(pwd)/dist"
WORK="$(pwd)/work"
TC_DIR="$WORK/toolchain"
STAGE="$WORK/stage"

mkdir -p "$DIST" "$WORK" "$TC_DIR" "$STAGE"

TOOLCHAIN_URL="https://github.com/openipc/firmware/releases/download/toolchain/toolchain.sigmastar-infinity6e.tgz"
ALSA_VER="1.2.11"
ALSA_LIB_URL="https://github.com/alsa-project/alsa-lib/archive/refs/tags/v${ALSA_VER}.tar.gz"
OPUS_VER="1.5.2"
OPUS_URL="https://github.com/xiph/opus/archive/refs/tags/v${OPUS_VER}.tar.gz"

echo "==> 构建产物目录: $DIST"

# ---------------- 1. 工具链 ----------------
if [ ! -x "$TC_DIR/bin/arm-openipc-linux-gnueabihf-gcc" ]; then
  echo "==> 下载 OpenIPC 工具链 ..."
  wget -q "$TOOLCHAIN_URL" -O "$WORK/tc.tgz"
  tar -xzf "$WORK/tc.tgz" -C "$TC_DIR" --strip-components=1
fi
export PATH="$TC_DIR/bin:$PATH"
CROSS="arm-openipc-linux-gnueabihf-"
"$CROSS"gcc --version | head -1

# ---------------- 2. alsa-lib(静态) ----------------
if [ ! -f "$STAGE/usr/lib/libasound.a" ]; then
  echo "==> 编译 alsa-lib $ALSA_VER (静态) ..."
  cd "$WORK"
  wget -q "$ALSA_LIB_URL" -O alsa-lib.tgz
  tar -xzf alsa-lib.tgz
  cd alsa-lib-${ALSA_VER}
  autoreconf -fi
  ./configure --host="$CROSS" --prefix="$STAGE/usr" \
    --enable-static --disable-shared --with-pic >/dev/null
  make -j"$(nproc)" >/dev/null && make install >/dev/null
fi

# ---------------- 3. opus(静态, fixed-point) ----------------
if [ ! -f "$STAGE/usr/lib/libopus.a" ]; then
  echo "==> 编译 opus $OPUS_VER (fixed-point) ..."
  cd "$WORK"
  wget -q "$OPUS_URL" -O opus.tgz
  tar -xzf opus.tgz
  cd opus-${OPUS_VER}
  ./configure --host="$CROSS" --prefix="$STAGE/usr" \
    --enable-static --disable-shared --enable-fixed-point \
    --disable-doc --disable-extra-programs --disable-custom-modes >/dev/null
  make -j"$(nproc)" >/dev/null && make install >/dev/null
fi

# ---------------- 4. 编译 audio_bridge ----------------
echo "==> 编译 audio_bridge(静态) ..."
cd "$(dirname "$0")"
"$CROSS"gcc -O2 -static \
  -I"$STAGE/usr/include" -I"$STAGE/usr/include/opus" \
  audio_bridge.c \
  -L"$STAGE/usr/lib" -lasound -lopus -lpthread -lm \
  -o "$DIST/audio_bridge"

echo "==> 产物:"
file "$DIST/audio_bridge"
ls -la "$DIST/audio_bridge"
echo "完成。传到设备 /usr/bin/audio_bridge(或 SD 卡), 用法见 audio_bridge.c 头注释"
