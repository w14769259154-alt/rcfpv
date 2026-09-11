#!/bin/sh
# ============================================================
# 5gipc-rc: 设备端 USB 声卡采集测试(需先 load_snd.sh start)
# 录 5 秒 wav, 检查字节数与非零采样(判断是否真实采集到声音)
# 用法: sh test_snd.sh [秒数]
# ============================================================
SECS="${1:-5}"
AUD_DIR=/mnt/mmcblk0p1/5gipc/audio
OUT=/tmp/snd_test.wav

# 0. 前置检查
echo "== 声卡状态 =="
cat /proc/asound/cards 2>&1 || { echo "无 /proc/asound: 先执行 sh load_snd.sh start"; exit 1; }
ls /dev/snd/ 2>&1

# ALSA 配置: arecord 运行时需读 alsa.conf, 用 SD 卡上的配置包; libasound 用自带的共享库
export ALSA_CONFIG_PATH="$AUD_DIR/alsa/alsa.conf"
export ALSA_CONFIG_DIR="$AUD_DIR/alsa"
export LD_LIBRARY_PATH="$AUD_DIR/lib:$LD_LIBRARY_PATH"

# 找第一张卡(card0)
CARD=$(cat /proc/asound/cards 2>/dev/null | grep -oE "^ *[0-9]+" | head -1 | tr -d ' ')
[ -n "$CARD" ] || CARD=0
echo "== 使用 card $CARD =="

# 1. 采集(用内建 hw 设备: plug 插件是外部 .so, 静态 arecord 无法 dlopen;
#    plughw 需 libasound_module_pcm_plug.so, 会失败。hw 走硬件直通, 需声卡支持该格式)
echo "== 采集 ${SECS}s (hw 直通) ... =="
if ! "$AUD_DIR/arecord" -D "hw:$CARD,0" -c 1 -r 16000 -f S16_LE -d "$SECS" "$OUT" 2>&1; then
  echo "  16000 失败, 试 48000 ..."
  "$AUD_DIR/arecord" -D "hw:$CARD,0" -c 1 -r 48000 -f S16_LE -d "$SECS" "$OUT" 2>&1
fi
ls -l "$OUT"

# 2. 数据有效性: 统计非零采样(跳过 wav 44 字节头)
NONZERO=$(dd if="$OUT" bs=1 skip=44 2>/dev/null | od -An -v -t d2 2>/dev/null | \
  awk '{for(i=1;i<=NF;i++){v=$i; if(v<0) v=-v; if(v>80) c++}} END{print c+0}')
SIZE=$(wc -c < "$OUT")
echo "== 结果 =="
echo "  wav 大小: $SIZE 字节 (预期 > $((SECS*32000+44)), 静音则只有头部小体积)"
echo "  非静音采样数: $NONZERO (>=100 说明确实采到了声音信号)"

# 3. 提示
if [ "$SIZE" -lt $((SECS*30000)) ]; then
  echo "  [!!] 数据量不足, 声卡可能未真正工作"
elif [ "$NONZERO" -lt 100 ]; then
  echo "  [!!] 采到的基本是静音(麦克风没接/增益 0/采样端错误)"
else
  echo "  [OK] 声卡采集链路正常"
fi
echo "  wav 文件: $OUT (可 scp 回电脑播放验证)"
