# 5gipc-rc — OpenIPC(ssc338q) 5G 遥控车 FPV 地面站

功能对标 `已完成项目/RCfpv5.01公网`（RTSP/WebRTC 视频 + MAVLink2 遥测控制 + USB 声卡双向对讲），但**去掉 Radxa**，由 **OpenIPC 摄像头(ssc338q+IMX335) 直接承担 edge 全部功能**（方案 2）：edge(WebRTC) 跑在摄像头，视频/对讲/数控 **P2P 直达地面端**。地面端(PC Electron)与 RCFpv5.01 通用，仅房间号/信令地址不同。

## 当前进度(2026-09-11)

| 里程碑 | 状态 |
|---|---|
| 摄像头基础验证(视频/数传/隧道) | ✅ 见 [5gIPC README](../5gIPC/README.md) |
| USB 声卡可行性 | ✅ 实机：card0 mic / card1 speaker 全通（见下） |
| **内存基线** | ✅ **MemAvailable=60MB / majestic RSS 8.4MB** → 方案 2 内存关通过 |
| 固件定制(砍包瘦身+声卡驱动+内置服务) | ✅ 代码就绪 `firmware/` |
| edge 骨架(ARMv7 libdatachannel 内存实测) | 🔄 GH Actions 编译中 `edge-skel/` |
| edge 全量移植到摄像头 | ⏳ 骨架验证通过后开始 |
| 端到端 P2P 联调 | ⏳ |

## 实测结论(2026-09-11 实机)

**摄像头可驱动 USB 声卡**，两张卡分工明确：

| 声卡 | 设备 | 角色 | 实测能力 |
|---|---|---|---|
| card0 | Generalplus USB Audio (1b3f:2008) | **麦克风**(capture) | mono, 44.1k/48k, S16_LE |
| card1 | USB2.0 Device (Generic) | **扬声器**(playback) | stereo, **仅 48k**, S16_LE |

- 录音(card0)：`arecord -D hw:0,0 -c 1 -r 48000 -f S16_LE` ✅
- 播放(card1)：`aplay -D hw:1,0` 48k/stereo ✅（card0 无播放能力）
- **对讲约束**：上行 card0 mono 48k → Opus；下行解码 → 转 **stereo 48k** → card1

**内存基线**：`free -m` → 总 91MB，**可用 60MB**；`ps` → majestic 8.4MB。→ 方案 2（edge 预算 ≤35MB）可行。

## 架构（方案 2：P2P）

```
主摄(ssc338q): majestic(编码) + edge(libdatachannel) + mavfwd + S60sound
   ├ 视频: 本地取流 → WebRTC P2P ──5G蜂窝──→ PC 地面站(零改动)
   ├ 对讲: 本地 ALSA(card0 mic / card1 spk) + Opus → WebRTC 音频轨
   └ 数控: ttyS2@115200 → MavlinkBridge(串口) → 数据通道
VPS: signaling(WSS 8443) + coturn TURN(3478) —— 仅信令 + 打洞兜底, 不中转媒体
```

## 构建与部署（GitHub Actions 云编译）

| Workflow | 产物 | 说明 |
|---|---|---|
| `build-edge-skel` | `edge_skel` | ARMv7 libdatachannel 内存基线实测 |
| `build-firmware` | `openipc.ssc338q-nor-rc.tgz` | **自编固件**：砍包瘦身 + USB Audio 驱动内置 + edge/mavfwd/监护自启 |
| `build-audio-modules` | `snd-modules` | USB 音频内核模块（驱动验证/回退） |

刷机：`sysupgrade --force_all -n --kernel/--rootfs`（参考 [5gIPC README](../5gIPC/README.md)）。

## 设备端装机（固件内置，刷机即用）

- `/etc/5gipc.conf`：mavfwd(ttyS2→WG)、edge、声卡参数
- `/etc/init.d/S60sound`：声卡模块自启
- `/etc/init.d/S99_5gipc`：监护 mavfwd/edge
- `/usr/bin/mavfwd`、`/usr/bin/rcfpv-edge`(edge 移植后)

## 参考

- 摄像头基线/固件/隧道: [5gIPC](../5gIPC/README.md)
- 参考项目: `已完成项目/RCfpv5.01公网`(edge 车端 C++ / signaling / 桌面地面站)
- 详细需求: [需求.md](需求.md)（方案 2 定稿）
