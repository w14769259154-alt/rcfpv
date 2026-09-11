#!/bin/bash
# ============================================================
# 5gipc-rc: 自编 OpenIPC 固件(ssc338q) — 砍包瘦身 + USB声卡驱动 + 内置服务
# 用法: bash build_firmware.sh
# 产物: dist/openipc.ssc338q-nor-rc.tgz (uImage + rootfs, sysupgrade 刷)
# 流程: 交叉编译 audio_bridge → 应用 defconfig/fragment/overlay → make BOARD=ssc338q_rc
# 耗时: 1-2h (buildroot 全量)
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
DIST="$ROOT/dist"
WORK="$ROOT/work"
FW="$WORK/firmware"

mkdir -p "$DIST" "$WORK"

echo "==> [1/7] 交叉编译 audio_bridge ..."
cd "$ROOT/audio"
bash build_audio_bridge.sh
cd "$ROOT"

echo "==> [2/7] 组装 overlay(内置 audio_bridge/mavfwd/5gipc 服务) ..."
mkdir -p "$ROOT/firmware/overlay/usr/bin"
cp "$ROOT/audio/dist/audio_bridge" "$ROOT/firmware/overlay/usr/bin/audio_bridge"
chmod +x "$ROOT/firmware/overlay/usr/bin/"* "$ROOT/firmware/overlay/etc/init.d/"* 2>/dev/null || true

echo "==> [3/7] clone OpenIPC firmware ..."
if [ ! -d "$FW" ]; then
  git clone --depth 1 https://github.com/OpenIPC/firmware.git "$FW"
fi

echo "==> [4/7] 应用定制 defconfig ..."
cp "$ROOT/firmware/ssc338q_rc_defconfig" \
   "$FW/br-ext-chip-sigmastar/configs/ssc338q_rc_defconfig"

echo "==> [5/7] 应用内核片段(USB Audio 模块) ..."
KCONF="$FW/br-ext-chip-sigmastar/board/infinity6e/infinity6e-ssc012b.config"
grep -vE '^CONFIG_(SOUND|SND|SND_|SND_USB)' "$KCONF" > "$KCONF.tmp"
mv "$KCONF.tmp" "$KCONF"
cat "$ROOT/firmware/kernel-snd.fragment" >> "$KCONF"

echo "==> [6/7] 应用 overlay(内置服务) ..."
cp -r "$ROOT/firmware/overlay/." "$FW/general/overlay/"

echo "==> [7/7] 构建固件(make BOARD=ssc338q_rc, 1-2h) ..."
cd "$FW"
make BOARD=ssc338q_rc

echo "==> 收集产物 ..."
cp output/images/openipc.ssc338q-nor-rc.tgz "$DIST/"
echo "完成: $DIST/openipc.ssc338q-nor-rc.tgz"
echo "刷机: sysupgrade --force_all -n --kernel/--rootfs (参考 5gIPC README)"
