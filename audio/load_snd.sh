#!/bin/sh
# ============================================================
# 5gipc-rc: 设备端 USB 声卡内核模块加载/卸载
# 用法: sh load_snd.sh start | stop | status
# 模块需先用 GitHub Actions 编译(build_snd.sh), dist/ 传到 SD 卡:
#   /mnt/mmcblk0p1/5gipc/audio/
# 前提: 内核 CONFIG_MODULES=y(已验证), 声卡插在 USB host 口(已枚举)
# ============================================================

AUD_DIR=/mnt/mmcblk0p1/5gipc/audio
MODULES="soundcore snd snd-timer snd-pcm snd-hwdep snd-rawmidi snd-usb-audio"

start() {
  # 幂等: 已加载则跳过
  for m in $MODULES; do
    if lsmod 2>/dev/null | grep -q "^$m "; then
      echo "skip(已加载): $m"
      continue
    fi
    echo "insmod $m.ko"
    insmod "$AUD_DIR/$m.ko" || { echo "!! insmod $m 失败(vermagic 不匹配或依赖缺失)"; return 1; }
  done
  sleep 1
  status
}

stop() {
  # 逆序卸载
  for m in snd-usb-audio snd-rawmidi snd-hwdep snd-pcm snd-timer snd soundcore; do
    if lsmod 2>/dev/null | grep -q "^$m "; then
      echo "rmmod $m"
      rmmod "$m" 2>/dev/null || echo "  (rmmod $m 被占用, 跳过)"
    fi
  done
}

status() {
  echo "---- /proc/asound ----"
  cat /proc/asound/cards 2>&1
  echo "---- /dev/snd ----"
  ls -l /dev/snd/ 2>&1
  echo "---- 已加载模块 ----"
  lsmod 2>/dev/null | grep -E "snd|soundcore" || true
}

case "$1" in
  start)   start ;;
  stop)    stop ;;
  status)  status ;;
  restart) stop; start ;;
  *) echo "用法: $0 {start|stop|status|restart}"; exit 1 ;;
esac
