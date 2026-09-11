# 5gipc-rc — OpenIPC(ssc338q) 5G 遥控车 FPV 地面站

功能对标 `已完成项目/RCfpv5.01公网`(RTSP/WebRTC 视频 + MAVLink2 遥测控制 + USB 声卡双向对讲), 但机位端从 Radxa Zero3W 换成 **OpenIPC 摄像头(ssc338q+IMX335)**。地面端(PC Electron/Flutter)与 RCFpv5.01 通用, 仅房间号/信令地址不同。

## 当前进度(2026-09-11)

| 里程碑 | 状态 |
|---|---|
| 摄像头基础验证(视频/数传/隧道) | ✅ 见 [5gIPC README](../5gIPC/README.md) |
| **机位 USB 声卡可行性验证** | 🔬 进行中: 内核无 USB Audio 驱动 → 编译模块方案已设计 |
| edge(WebRTC 推流+MAVLink 桥)移植到 OpenIPC | ⏳ 待声卡验证后评估(内存 91MB 是约束) |
| 5gipc-rc 信令/房间名 | ⏳ 复用 RCFpv5.01 signaling, room_id 改为独立值 |

## 为什么需要编译音频模块

OpenIPC ultimate 固件为省资源把 ALSA 整栈裁剪(`# CONFIG_SOUND is not set`), 但:
- 内核 `CONFIG_MODULES=y`、`CONFIG_MODVERSIONS` 未开 → **可以编译 .ko 放 SD 卡加载, 不重刷固件**
- 设备有 insmod/modprobe/mdev + devtmpfs(加载后 `/dev/snd/*` 自动创建)
- 内核源码 = openipc/linux 分支 `sigmastar-infinity6e`(4.9.84, 固件同源)
- 声卡硬件已被枚举: Generalplus USB Audio Device(1b3f:2008)挂在 EHCI host 的板载 Hub 下

## 构建步骤(GitHub Actions 云编译)

1. 把本目录推到你的 GitHub 仓库(私有即可):
   ```bash
   git init && git add . && git commit -m "audio modules" && git push
   ```
2. GitHub → Actions → `build-audio-modules` → **Run workflow**(手动触发)
3. 等待 10-25 分钟, 构建完成 → 下载 Artifact `snd-modules`, 解压得到:
   - `soundcore.ko` `snd.ko` `snd-timer.ko` `snd-pcm.ko` `snd-hwdep.ko` `snd-rawmidi.ko` `snd-usb-audio.ko`
   - `arecord`(静态, 独立可执行)
4. 传到设备 SD 卡:
   ```bash
   # 本机起 HTTP(fw-server)或 scp 到设备, 目标:
   # /mnt/mmcblk0p1/5gipc/audio/
   ```
5. 设备上(SSH root@<IP>):
   ```sh
   cd /mnt/mmcblk0p1/5gipc/audio
   sh load_snd.sh start     # 加载模块, 打印 /proc/asound/cards
   sh test_snd.sh 5         # 录 5 秒, 检查非静音采样
   ```

### 失败排查(vermagic 不匹配)

`insmod` 报 `invalid module format` 时, 先 `insmod` 打印的错误与模块自带 vermagic 对比:

```bash
# 设备上
modinfo /mnt/mmcblk0p1/5gipc/audio/snd-usb-audio.ko 2>/dev/null || strings snd-usb-audio.ko | grep -E "vermagic|4\.9\."
# 期望 vermagic 形如: 4.9.84 SMP preempt mod_unload ARMv7 p2v8 ...
# 若固件升级过内核(4.9.85+), 需用 openipc/linux 对应新 tag 重新编译
```

设备固件对应内核: `uname -r` = 4.9.84, 构建时间 2026-09-10。

## 构建输入(可调)

| 变量 | 值 | 说明 |
|---|---|---|
| KERNEL_TAG | `sigmastar-infinity6e` | openipc/linux 分支, 与固件 4.9.84 对应 |
| 内核 config | firmware `infinity6e-ssc012b.config` + 追加 SND | 见 build_snd.sh |
| 工具链 | openipc `toolchain.sigmastar-infinity6e.tgz` | 官方发布 |
| ALSA | 1.2.11(静态 libasound + arecord) | --disable-shared |

音频采集目标参数(供后续 edge 使用): **16kHz / 单声道 / S16_LE**(参考项目为 48k/mono/960, 按需调)。

## 参考

- 摄像头基线/固件/隧道: [5gIPC](../5gIPC/README.md)
- 参考项目: `已完成项目/RCfpv5.01公网`(edge 车端 C++ / signaling / 桌面地面站)
