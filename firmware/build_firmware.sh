#!/bin/bash
# ============================================================
# 5gipc-rc: 自编 OpenIPC 固件(ssc338q) — 方案2
# 砍包瘦身 + USB声卡驱动 + 内置 edge/mavfwd/监护服务
# 用法: bash build_firmware.sh
# 产物: dist/openipc.ssc338q-nor-rc.tgz (uImage + rootfs, sysupgrade 刷)
# 流程: 应用 defconfig/fragment/overlay → make BOARD=ssc338q_rc
# 耗时: 1-2h (buildroot 全量)
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
DIST="$ROOT/dist"
WORK="$ROOT/work"
FW="$WORK/firmware"

mkdir -p "$DIST" "$WORK"

echo "==> [1/5] clone OpenIPC firmware ..."
if [ ! -d "$FW" ]; then
  git clone --depth 1 https://github.com/OpenIPC/firmware.git "$FW"
fi

echo "==> [2/5] 应用定制 defconfig ..."
cp "$ROOT/firmware/ssc338q_rc_defconfig" \
   "$FW/br-ext-chip-sigmastar/configs/ssc338q_rc_defconfig"

echo "==> [3/5] 应用内核片段(USB Audio 模块) ..."
KCONF="$FW/br-ext-chip-sigmastar/board/infinity6e/infinity6e-ssc012b.config"
grep -vE '^CONFIG_(SOUND|SND|SND_|SND_USB)' "$KCONF" > "$KCONF.tmp"
mv "$KCONF.tmp" "$KCONF"
cat "$ROOT/firmware/kernel-snd.fragment" >> "$KCONF"

echo "==> [4/5] 应用 overlay(内置 mavfwd/监护/edge 预留) ..."
cp -r "$ROOT/firmware/overlay/." "$FW/general/overlay/"

echo "==> [5/5] 构建固件(make BOARD=ssc338q_rc, 1-2h) ..."
cd "$FW"
make BOARD=ssc338q_rc

echo "==> 收集产物 ..."
cp output/images/openipc.ssc338q-nor-rc.tgz "$DIST/"
echo "完成: $DIST/openipc.ssc338q-nor-rc.tgz"
echo "刷机: sysupgrade --force_all -n --kernel/--rootfs (参考 5gIPC README)"
