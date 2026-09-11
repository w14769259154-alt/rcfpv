#!/bin/bash
# ============================================================
# 5gipc-rc VPS edge 部署脚本(在 VPS 上执行)
#
# 用法:
#   bash deploy_vps.sh install [摄像头root密码] [TURN密码]   # 首次:依赖+clone+编译+systemd
#   bash deploy_vps.sh rebuild                               # 重编译(改代码后)
#   bash deploy_vps.sh restart|status|logs                   # 服务管理
#
# 环境变量:
#   LIBDC_SRC   libdatachannel 源码路径(默认 clone v0.24.5 到 /opt/rcfpv-edge/libdatachannel)
#
# 依赖: Ubuntu/Debian, root 或 sudo
# ============================================================
set -e

INSTALL_DIR=/opt/rcfpv-edge
SVC=rcfpv-edge
CAM_PASS="${2:-}"      # 摄像头 root 密码(majestic RTSP 认证)
TURN_PASS="${3:-}"

SUDO=""
[ "$(id -u)" != "0" ] && SUDO=sudo

case "${1:-}" in
  install)
    echo "[1/5] 安装编译依赖..."
    $SUDO apt update
    $SUDO apt install -y build-essential cmake pkg-config git \
        libavformat-dev libavcodec-dev libavutil-dev \
        libssl-dev nlohmann-json3-dev
    echo "[2/5] libdatachannel v0.24.5..."
    if [ -n "$LIBDC_SRC" ]; then
      echo "  使用 LIBDC_SRC=$LIBDC_SRC"
      mkdir -p "$INSTALL_DIR"
      rm -rf "$INSTALL_DIR/libdatachannel"
      cp -r "$LIBDC_SRC" "$INSTALL_DIR/libdatachannel"
    else
      mkdir -p "$INSTALL_DIR"
      [ -d "$INSTALL_DIR/libdatachannel" ] || \
        git clone --depth 1 --branch v0.24.5 --recurse-submodules \
          https://github.com/paullouisageneau/libdatachannel.git "$INSTALL_DIR/libdatachannel"
    fi
    echo "[3/5] 部署源码..."
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    $SUDO mkdir -p "$INSTALL_DIR/src"
    $SUDO cp -r "$SCRIPT_DIR/src/." "$INSTALL_DIR/src/"
    $SUDO cp "$SCRIPT_DIR/CMakeLists.txt" "$INSTALL_DIR/CMakeLists.txt"
    if [ -n "$CAM_PASS" ]; then
      sed "s/CHANGE_ME/$CAM_PASS/g" "$SCRIPT_DIR/config.json" > /tmp/edge_config.json
      # 仅第一次安装落配置;已存在则不覆盖(保护手改)
      [ -f "$INSTALL_DIR/config.json" ] || $SUDO cp /tmp/edge_config.json "$INSTALL_DIR/config.json"
    fi
    echo "[4/5] 编译..."
    cd "$INSTALL_DIR"
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j"$(nproc)"
    echo "[5/5] systemd 服务..."
    $SUDO tee /etc/systemd/system/${SVC}.service > /dev/null <<EOF
[Unit]
Description=5gipc-rc VPS Edge (WebRTC broadcaster for RC FPV ground station)
After=network.target wg-quick@wg0.service

[Service]
Type=simple
WorkingDirectory=$INSTALL_DIR
ExecStart=$INSTALL_DIR/build/rcfpv-edge config/edge_config.json
Restart=always
RestartSec=3
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
    $SUDO systemctl daemon-reload
    $SUDO systemctl enable ${SVC}
    $SUDO systemctl restart ${SVC}
    echo "============================================================"
    echo " 部署完成。服务: systemctl status $SVC"
    echo " 配置: $INSTALL_DIR/config.json (需填 TURN 密码/摄像头密码/room_id)"
    echo " 日志: journalctl -u $SVC -f"
    echo "============================================================"
    ;;
  rebuild)
    cd "$INSTALL_DIR"
    $SUDO cmake --build build -j"$(nproc)"
    $SUDO systemctl restart ${SVC}
    ;;
  restart) $SUDO systemctl restart ${SVC} ;;
  stop)    $SUDO systemctl stop ${SVC} ;;
  status)  $SUDO systemctl status ${SVC} --no-pager ;;
  logs)    $SUDO journalctl -u ${SVC} -n 50 --no-pager ;;
  *)
    echo "用法: $0 {install|rebuild|restart|stop|status|logs} [摄像头密码] [TURN密码]"
    exit 1
    ;;
esac
