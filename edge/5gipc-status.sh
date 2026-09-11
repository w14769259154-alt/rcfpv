#!/bin/sh
# 5gipc-rc: OpenIPC 摄像头板载状态采集上报(VPS edge TCP:7215,3s 周期)
# 帧: STATUS load=<1min负载> mem=<内存使用率%> up=<运行秒> disk=<SD使用率%> rx=<下行kbps> tx=<上行kbps>
# 依赖: busybox nc(TCP)/awk/df;由 /etc/init.d/S99_5gipc_status 开机拉起
# VPS edge StatusListener 解析后注入 OSD "板载状态"(替代显示 VPS 自身状态)

VPS=10.55.0.1
PORT=7215
INTERVAL=3

last_rx=""
last_tx=""
last_t=""

while true; do
  load=$(awk '{print $1}' /proc/loadavg 2>/dev/null)
  mem=$(awk '/MemTotal:/{t=$2} /MemAvailable:/{a=$2} END{if(t>0) printf "%d", (t-a)*100/t}' /proc/meminfo 2>/dev/null)
  up=$(awk '{printf "%.0f", $1}' /proc/uptime 2>/dev/null)
  disk=$(df /mnt/mmcblk0p1 2>/dev/null | awk 'NR==2{gsub("%","",$5); print $5}')
  # eth0 收发累计字节:busybox awk 默认按空白切分,$1=接口名
  rx=$(awk '$1=="eth0:"{print $2}' /proc/net/dev 2>/dev/null)
  tx=$(awk '$1=="eth0:"{print $10}' /proc/net/dev 2>/dev/null)
  now=$(awk '{printf "%.0f", $1}' /proc/uptime 2>/dev/null)
  rxbps=0
  txbps=0
  if [ -n "$last_rx" ] && [ -n "$last_t" ] && [ -n "$now" ] && [ "$now" != "$last_t" ]; then
    dt=$((now - last_t))
    [ "$dt" -lt 1 ] && dt=1
    if [ -n "$rx" ] && [ "$rx" -ge "$last_rx" ] 2>/dev/null; then
      rxbps=$(( (rx - last_rx) * 8 / dt / 1000 ))
    fi
    if [ -n "$tx" ] && [ "$tx" -ge "$last_tx" ] 2>/dev/null; then
      txbps=$(( (tx - last_tx) * 8 / dt / 1000 ))
    fi
  fi
  last_rx=$rx
  last_tx=$tx
  last_t=$now
  echo "STATUS load=$load mem=$mem up=$up disk=$disk rx=$rxbps tx=$txbps" | nc -w2 "$VPS" "$PORT" 2>/dev/null
  sleep "$INTERVAL"
done
