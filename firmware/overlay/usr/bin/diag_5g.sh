#!/bin/sh
# 5gIPC 设备端一键诊断(OpenIPC ultimate 版) — busybox ash
# 架构: WG(固件env机制) → mavfwd(SD) → VPS:7202; 视频 majestic RTSP ← VPS经隧道拉
CONF="/etc/5gipc.conf"
[ -f "$CONF" ] || { echo "缺 $CONF"; exit 1; }
. "$CONF"

ok()   { echo "  [OK]   $*"; }
bad()  { echo "  [FAIL] $*"; }
warn() { echo "  [WARN] $*"; }

echo "===== 5gIPC 诊断 $(date '+%F %T') ====="

echo "[1] 基础网络(eth0, 网线来自 5G CPE)"
if ip -4 addr show dev eth0 2>/dev/null | grep -q 'inet '; then
    ok "IP: $(ip -4 addr show dev eth0 | grep 'inet ' | awk '{print $2}')"
else
    bad "eth0 无 IP — 检查网线/CPE(路由模式 NAT+DHCP)"
fi
GW=$(ip route | awk '/^default/{print $3; exit}')
[ -n "$GW" ] && ok "默认网关: $GW" || warn "无默认路由(CPE 未联网或未接)"

echo "[2] 出网连通性"
if ping -c 2 -W 2 223.5.5.5 >/dev/null 2>&1; then
    ok "蜂窝出网正常"
else
    warn "蜂窝出网不通 — CPE 侧: SIM 流量/APN/信号(外部设备人工核对)"
fi

echo "[3] WireGuard 隧道(固件 env 机制: wg_privkey/wg_endpoint/...)"
wg show wg0 2>/dev/null | sed 's/^/    /'
hs=$(wg show wg0 latest-handshakes 2>/dev/null | awk '{print $2}')
if [ -n "$hs" ] && [ "$hs" != "0" ]; then
    age=$(( $(date +%s) - hs ))
    [ "$age" -lt 180 ] && ok "最近握手 ${age}s 前" \
        || bad "握手 ${age}s 前 — 隧道僵死: 重启设备或重设 wg_endpoint"
else
    bad "从未握手 — 检查: wg_endpoint 设了吗? VPS peer 注册了吗? CPE 放行出站 UDP 51821?"
fi
ping -c 2 -W 2 10.55.0.1 >/dev/null 2>&1 && ok "隧道内 ping 10.55.0.1 通" \
    || bad "隧道内 ping 10.55.0.1 不通 — VPS peer AllowedIPs/防火墙"

echo "[4] 视频(majestic RTSP, VPS 经隧道拉取)"
pgrep -x majestic >/dev/null 2>&1 && ok "majestic 运行中" || bad "majestic 未运行"
netstat -ltn 2>/dev/null | grep -q ":554 " && ok "RTSP 554 监听中" \
    || bad "RTSP 554 未监听 — 检查 /etc/majestic.yaml"
grep -q "codec: h265" /etc/majestic.yaml && echo "    (当前 H265 1080p60 CBR 2048k GOP 1s)"

echo "[5] MAVLink 数传"
if [ -f /var/run/5gipc/mavfwd.pid ] && [ -d "/proc/$(cat /var/run/5gipc/mavfwd.pid 2>/dev/null)" ]; then
    ok "mavfwd 运行中(pid=$(cat /var/run/5gipc/mavfwd.pid))"
else
    bad "mavfwd 未运行 — 查 /tmp/log/5gipc/mavfwd.log"
fi
[ -e "$MAVFWD_UART" ] 2>/dev/null && ok "串口 $MAVFWD_UART 存在" \
    || warn "串口 $MAVFWD_UART 不存在 — 核对飞控接线/设备名"
[ -x "$MAVFWD_BIN" ] && ok "mavfwd 二进制在位: $MAVFWD_BIN" \
    || bad "mavfwd 二进制缺失: $MAVFWD_BIN"
tail -n 5 /tmp/log/5gipc/mavfwd.log 2>/dev/null | sed 's/^/    /'

echo "[6] 5gipc monitor"
if [ -f /var/run/5gipc/monitor.pid ] && [ -d "/proc/$(cat /var/run/5gipc/monitor.pid 2>/dev/null)" ]; then
    ok "monitor 运行中(pid=$(cat /var/run/5gipc/monitor.pid))"
else
    bad "monitor 未运行 — /etc/init.d/S99_5gipc start"
fi
tail -n 8 /tmp/log/5gipc/5gipc.log 2>/dev/null | sed 's/^/    /'

echo "[7] CPE 侧(外部设备, 人工核对)"
echo "    - 5G CPE 路由模式 NAT+DHCP, SIM 流量正常"
echo "    - CPE 防火墙放行出站 UDP 51821(WG)"
echo "    - 上行带宽 >= 3Mbps 建议"
echo "===== 诊断结束 ====="
