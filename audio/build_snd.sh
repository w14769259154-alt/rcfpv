#!/bin/bash
# ============================================================
# 5gipc-rc: 交叉编译 OpenIPC(ssc338q) USB 声卡内核模块 + 静态 arecord
# 用法: bash build_snd.sh
# 产物: dist/  (内核模块 .ko + aplay/arecord + 版本信息)
# 环境: Ubuntu 22.04+ (GitHub Actions / WSL2 均可)
# ============================================================
set -euo pipefail

DIST="$(pwd)/audio/dist"   # 与 workflow upload-artifact path: audio/dist/ 保持一致
ROOT="$(pwd)"              # 仓库根(后续脚本会 cd, 引用仓库内文件用绝对路径)
WORK="$(pwd)/work"
TC_DIR="$WORK/toolchain"
KSRC="$WORK/linux"
STAGE="$WORK/stage"

mkdir -p "$DIST" "$WORK" "$TC_DIR" "$KSRC" "$STAGE"

# ---------------- 0. 关键变量 ----------------
KERNEL_TAG="sigmastar-infinity6e"                       # openipc/linux 分支(与固件 4.9.84 对应)
TOOLCHAIN_URL="https://github.com/openipc/firmware/releases/download/toolchain/toolchain.sigmastar-infinity6e.tgz"
# 注意: sigmastar-infinity6e 是 分支(branch) 不是 tag, 必须用 refs/heads/ 下载
KERNEL_URL="https://github.com/openipc/linux/archive/refs/heads/${KERNEL_TAG}.tar.gz"
CONFIG_URL="https://raw.githubusercontent.com/OpenIPC/firmware/master/br-ext-chip-sigmastar/board/infinity6e/infinity6e-ssc012b.config"
ALSA_VER="1.2.11"
# 用 ALSA 官方发布包(自带 configure + AM_PATH_ALSA 宏已展开), 避免 GitHub 源码需 autoreconf 的一堆坑
ALSA_LIB_URL="https://www.alsa-project.org/files/pub/lib/alsa-lib-${ALSA_VER}.tar.bz2"
ALSA_UTILS_URL="https://www.alsa-project.org/files/pub/utils/alsa-utils-${ALSA_VER}.tar.bz2"

echo "==> 构建产物目录: $DIST"

# ---------------- 1. 工具链 ----------------
if [ ! -x "$TC_DIR/bin/arm-openipc-linux-gnueabihf-gcc" ]; then
  echo "==> 下载 OpenIPC 工具链 ..."
  wget -q "$TOOLCHAIN_URL" -O "$WORK/tc.tgz"
  tar -xzf "$WORK/tc.tgz" -C "$TC_DIR" --strip-components=1
fi
# 系统 bin 放最前: 工具链自带残缺 autotools 包装脚本(缺 Perl 模块), 会让 autoreconf/aclocal 崩溃;
# 交叉编译器用全名调用(arm-openipc-*-gcc), 工具链 bin 保持在 PATH 中即可
export PATH="/usr/bin:/bin:$TC_DIR/bin:$PATH"
CROSS="arm-openipc-linux-gnueabihf-"
"$CROSS"gcc --version | head -1

# ---------------- 2. 内核源码 ----------------
if [ ! -f "$KSRC/Makefile" ]; then
  echo "==> 下载内核源码 $KERNEL_TAG ..."
  wget -q "$KERNEL_URL" -O "$WORK/linux.tar.gz"
  tar -xzf "$WORK/linux.tar.gz" -C "$KSRC" --strip-components=1
fi

# ---------------- 3. 内核配置: 追加 USB Audio ----------------
echo "==> 生成内核配置 ..."
wget -q "$CONFIG_URL" -O "$KSRC/.config"
cd "$KSRC"
# 用 scripts/config 追加(正确处理重复行/依赖)
./scripts/config \
  --module SOUND \
  --module SND \
  --module SND_TIMER \
  --module SND_PCM \
  --module SND_HWDEP \
  --module SND_RAWMIDI \
  --module SND_USB_AUDIO
# 确认
grep -E "CONFIG_SOUND|CONFIG_SND=|CONFIG_SND_USB_AUDIO" .config || true

echo "==> olddefconfig ..."
make ARCH=arm olddefconfig >/dev/null 2>&1 || make ARCH=arm olddefconfig

# ---------------- 4. 编译内核模块 ----------------
echo "==> modules_prepare ..."
make ARCH=arm CROSS_COMPILE="$CROSS" modules_prepare -j"$(nproc)" >/dev/null
echo "==> modules (只编 =m 的音频相关) ..."
make ARCH=arm CROSS_COMPILE="$CROSS" modules -j"$(nproc)"

echo "==> 收集 .ko ..."
KO_LIST="soundcore.ko snd.ko snd-timer.ko snd-pcm.ko snd-hwdep.ko snd-rawmidi.ko snd-usbmidi-lib.ko snd-usb-audio.ko"
for k in $KO_LIST; do
  f=$(find "$KSRC/sound" -name "$k" | head -1)
  if [ -n "$f" ]; then cp "$f" "$DIST/"; echo "  + $k"; else echo "  !! 缺少 $k"; fi
done

# ---------------- 5. 交叉编译动态 arecord (设备是 glibc armhf, 与工具链匹配) ----------------
# 说明: 完全静态链接时 ALSA 库的插件加载(dlopen)机制不可用, 必须动态链接 libasound
echo "==> 编译 alsa-lib (共享库) ..."
cd "$WORK"
wget -q "$ALSA_LIB_URL" -O alsa-lib.tbz2
tar -xjf alsa-lib.tbz2
cd alsa-lib-${ALSA_VER}
./configure --host="${CROSS%-}" --prefix="$STAGE/usr" \
  --enable-shared --disable-static >/dev/null
make -j"$(nproc)" >/dev/null && make install >/dev/null

echo "==> 编译 alsa-utils (仅 aplay/arecord, 动态) ..."
cd "$WORK"
wget -q "$ALSA_UTILS_URL" -O alsa-utils.tbz2
tar -xjf alsa-utils.tbz2
cd alsa-utils-${ALSA_VER}
export PKG_CONFIG_PATH="$STAGE/usr/lib/pkgconfig"
./configure --host="${CROSS%-}" --prefix="$STAGE/usr" \
  --disable-alsaconf --disable-alsactl --disable-alsaloop \
  --disable-alsamixer --disable-alsaucm --disable-amixer \
  --disable-speaker-test --disable-bat --disable-xmlto --disable-nls \
  --disable-alsatplg --disable-topology \
  --with-alsa-inc-prefix="$STAGE/usr/include" \
  --with-alsa-prefix="$STAGE/usr/lib" >/dev/null
make -j"$(nproc)" >/dev/null || make >/dev/null
cp aplay/aplay "$DIST/arecord" 2>/dev/null || cp aplay/aplay "$DIST/aplay"
file "$DIST"/* 2>/dev/null || true

# ---------------- 5.5 打包 ALSA 配置文件 + 共享库 ----------------
# 设备上设: export ALSA_CONFIG_PATH=$AUD_DIR/alsa/alsa.conf ALSA_CONFIG_DIR=$AUD_DIR/alsa
#           export LD_LIBRARY_PATH=$AUD_DIR/lib:$LD_LIBRARY_PATH
if [ -d "$STAGE/usr/share/alsa" ]; then
  mkdir -p "$DIST/alsa"
  cp -r "$STAGE/usr/share/alsa/." "$DIST/alsa/"
  # 完整 alsa.conf 的 @hooks 需要 dlopen 动态库; 精简版避免(仅保留 hw/plughw/default)
  cp "$ROOT/audio/alsa.min.conf" "$DIST/alsa/alsa.conf"
  echo "==> 打包 ALSA 配置(精简版) -> $DIST/alsa/"
fi
mkdir -p "$DIST/lib"
cp -L "$STAGE/usr/lib"/libasound.so* "$DIST/lib/" 2>/dev/null || true
echo "==> 打包 libasound -> $DIST/lib/"
ls -la "$DIST/lib" 2>/dev/null || true

# ---------------- 6. 版本信息/vermagic 校验 ----------------
echo "==> 模块 vermagic(需与设备 insmod 报错对比) ..."
if command -v modinfo >/dev/null; then
  modinfo "$DIST/snd-usb-audio.ko" 2>/dev/null | grep -E "vermagic|filename" || true
fi
echo "==> 产物: $DIST"
ls -la "$DIST"
echo "完成。把 dist/ 整个目录传到设备 SD 卡, 执行 load_snd.sh start"
