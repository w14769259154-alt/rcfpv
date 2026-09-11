# 5gipc-rc — OpenIPC(ssc338q) 5G 遥控车 FPV 地面站

功能对标 `已完成项目/RCfpv5.01公网`(RTSP/WebRTC 视频 + MAVLink2 遥测控制 + USB 声卡双向对讲), 但机位端从 Radxa Zero3W 换成 **OpenIPC 摄像头(ssc338q+IMX335)**。地面端(PC Electron/Flutter)与 RCFpv5.01 通用, 仅房间号/信令地址不同。

## 当前进度(2026-09-11)

| 里程碑 | 状态 |
|---|---|
| 摄像头基础验证(视频/数传/隧道) | ✅ 见 [5gIPC README](../5gIPC/README.md) |
| **机位 USB 声卡可行性验证** | ✅ **完成**: 驱动/录音/播放全通, 见下方"实测结论" |
| edge(WebRTC 推流+MAVLink 桥)移植到 OpenIPC | ⏳ 待评估(内存 91MB 是约束) |
| 5gipc-rc 信令/房间名 | ⏳ 复用 RCFpv5.01 signaling, room_id 改为独立值 |

## 实测结论(2026-09-11 实机验证)

**摄像头可以驱动 USB 声卡**, 录音/播放全链路正常。两张 USB 声卡分工明确:

| 声卡 | 设备 | 角色 | 实测能力 |
|---|---|---|---|
| card0 | Generalplus USB Audio (1b3f:2008) | **麦克风**(capture) | mono, 44.1k/48k, S16_LE |
| card1 | USB2.0 Device (Generic) | **扬声器**(playback) | stereo, **仅 48k**, S16_LE |

验证结果:
- **录音**(card0): `arecord -D hw:0,0 -c 1 -r 44100 -f S16_LE` 录 5s → 200705 个非静音采样, 确认真实采到声音
- **播放**(card1): `aplay -D hw:1,0` 播放 48k/stereo 1kHz 测试音 → 正常出声无错误
- 注意 card0 麦克风**无音频输出**(播放会静音), 播放必须走 card1; 测试音参考 [test_tone_48k.wav](audio/test_tone_48k.wav)

**对 edge 对讲设计的约束**: 上行用 card0 mono(44.1k 或 48k)采集 → Opus 编码; 下行解码 → 转 **48k stereo** 送 card1 播放。

## 为什么需要编译音频模块

OpenIPC ultimate 固件为省资源把 ALSA 整栈裁剪(`# CONFIG_SOUND is not set`), 但:
- 内核 `CONFIG_MODULES=y` → **可以编译 .ko 放 SD 卡加载, 不重刷固件**
- 设备有 insmod/modprobe/mdev + devtmpfs(加载后 `/dev/snd/*` 自动创建)
- 内核源码 = openipc/linux 分支 `sigmastar-infinity6e`(4.9.84, 固件同源)
- 设备 glibc 为 buildroot-gcc-13.3.0(armhf), 与 openipc 工具链匹配 → **动态链接可行**

## 构建步骤(GitHub Actions 云编译)

1. 把本目录推到 GitHub 仓库 `w14769259154-alt/rcfpv`(`.github/workflows/build-audio.yml` + `audio/`)
2. GitHub → Actions → `build-audio-modules` → **Run workflow**(或 push audio/ 自动触发)
3. 构建完成(约 2 分钟) → 下载 Artifact `snd-modules`, 解压得到:
   - 8 个内核模块: `soundcore.ko` `snd.ko` `snd-timer.ko` `snd-pcm.ko` `snd-hwdep.ko` `snd-rawmidi.ko` `snd-usbmidi-lib.ko` `snd-usb-audio.ko`
   - `arecord`(动态链接, 需配合 lib/)
   - `lib/libasound.so.2*`(ALSA 共享库, 设备 glibc 可加载)
   - `alsa/alsa.conf`(精简配置, 仅 hw/plughw/default, 无 @hooks)
4. 传到设备 SD 卡:
   ```bash
   # scp -O -r 到设备, 目标:
   # /mnt/mmcblk0p1/5gipc/audio/{*.ko, arecord, lib/, alsa/, load_snd.sh, test_snd.sh}
   ```
5. 设备上(SSH root@<IP>):
   ```sh
   cd /mnt/mmcblk0p1/5gipc/audio
   sh load_snd.sh start     # 加载 8 个模块, 打印 /proc/asound/cards
   sh test_snd.sh 5         # 录 5 秒, 检查非静音采样
   # 播放验证(card1 扬声器):
   export LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH
   export ALSA_CONFIG_PATH=$PWD/alsa/alsa.conf ALSA_CONFIG_DIR=$PWD/alsa
   cp arecord aplay && ./aplay -D hw:1,0 /tmp/test_tone_48k.wav
   ```

## 关键坑(全部实踩, 已固化进 build_snd.sh)

1. **内核下载**: `sigmastar-infinity6e` 是**分支**不是 tag, 必须 `archive/refs/heads/` 否则 wget 404(exit 8)
2. **ALSA 源码**: GitHub 源码无 configure(autoconf 项目)且缺 AM_PATH_ALSA → 用官方发布包 tar.bz2(自带 configure); 且需 `autoreconf` 前把系统 `/usr/bin` 放 PATH 最前(openipc 工具链自带残缺 autotools 脚本缺 Perl 模块)
3. **configure --host** 不能带尾横杠: `--host="${CROSS%-}"` 否则 config.sub 报 more than four components
4. **必须动态链接**: 完全静态的 ALSA 程序 dlopen 插件机制不可用(无动态链接器) → alsa-lib `--enable-shared`, arecord 动态链接, 部署 `libasound.so.2` + `LD_LIBRARY_PATH`
5. **精简 alsa.conf**: 完整配置的 `@hooks` 需 dlopen 动态库会整树丢弃 → 用 [alsa.min.conf](audio/alsa.min.conf)(无 hooks)
6. **plughw 不可用**: plug 插件是外部 .so 未打包 → 用内建 `hw:0,0`; 后续如需重采样/格式转换需打包 `alsa-lib/` 插件目录或加 plug 插件
7. **lsmod 匹配**: 模块名是**下划线**(`snd_timer`)不是连字符(`snd-timer`), `grep "^$m "` 会误判 → File exists; load_snd.sh 已按模块名下划线匹配
8. **依赖模块**: `snd-usb-audio.ko` 依赖 `snd-usbmidi-lib.ko`(提供 `snd_usbmidi_create`), 两个都要收集/加载
9. **脚本内 cd 后引用仓库内文件**要用 `$ROOT` 绝对路径(否则 cp 找不到 audio/alsa.min.conf)
10. **设备重启后模块丢失**: insmod 不持久, 需重跑 `load_snd.sh start`(后续做开机自启)

## 构建输入(可调)

| 变量 | 值 | 说明 |
|---|---|---|
| KERNEL_TAG | `sigmastar-infinity6e` | openipc/linux 分支, 与固件 4.9.84 对应 |
| 内核 config | firmware `infinity6e-ssc012b.config` + 追加 SND | 见 build_snd.sh |
| 工具链 | openipc `toolchain.sigmastar-infinity6e.tgz` | 官方发布 |
| ALSA | 1.2.11(共享 libasound + 动态 arecord) | --enable-shared |

音频采集目标参数(供 edge 使用): **card0 mono 44.1k/48k S16_LE 采集**(实测硬件不支持 16k), 下行 **card1 stereo 48k** 播放。

## 参考

- 摄像头基线/固件/隧道: [5gIPC](../5gIPC/README.md)
- 参考项目: `已完成项目/RCfpv5.01公网`(edge 车端 C++ / signaling / 桌面地面站)
