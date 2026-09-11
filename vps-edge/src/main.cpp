/**
 * 5gipc-rc VPS edge — 入口(VPS 版,替代摄像头侧 Radxa/edge)
 *
 * 与参考项目 RCfpv5.0 edge 同协议(地面端零改动),但承担者从车端主机换到 VPS:
 *   - 视频: RtspCapture 经 WG 拉主/副两路摄像头 RTSP → WebRTC 推流
 *   - 数传: MavlinkBridge(UDP 模式)监听 7212,mavfwd 从摄像头经 WG 直推
 *   - 对讲: UdpAudioBridge ↔ 摄像头音频桥(USB 声卡 Opus 编解码在摄像头侧)
 *   - 看门狗: 数据通道断链 >5s → 中立 RC_OVERRIDE + 停心跳(触发飞控 failsafe)
 *
 * 启动顺序: 加载配置 → 创建组件 → 互连回调 → RTSP 采集 → 音频桥 → MAVLink 桥 → 信令 → 主循环
 */
#include "core/config.h"
#include "mavlink/bridge.h"
#include "media/rtsp_capture.h"
#include "media/status_listener.h"
#include "media/udp_audio_bridge.h"
#include "rtc/signaling_client.h"
#include "rtc/streamer.h"

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

std::atomic<bool> gStop{false};

void signalHandler(int) { gStop.store(true); }

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
float clampFloat(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

int main(int argc, char *argv[]) {
    std::cout << std::unitbuf; // systemd pipe 下全缓冲,立即刷新保证日志可见
    const std::string configPath = (argc > 1) ? argv[1] : "config/edge_config.json";

    std::cout << "=== 5gipc-rc VPS Edge ===" << std::endl;
    std::cout << "[main] 加载配置: " << configPath << std::endl;
    rcfpv::Config config = rcfpv::loadConfig(configPath);

    rtc::InitLogger(rtc::LogLevel::Warning);
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ---- 创建组件 ----
    auto streamer = std::make_unique<rcfpv::WebRtcStreamer>(config.webrtc, config.rtsp.codec);
    auto signaling = std::make_unique<rcfpv::SignalingClient>(config.signaling);
    auto rtsp = std::make_unique<rcfpv::RtspCapture>(config.rtsp.url, config.rtsp.transport,
                                                     config.rtsp.bufferSize, config.rtsp.codec);
    // 副路 RTSP 采集(双摄 PIP):url2 为空时对象存在但不启动,SET_AUX_URL 运行时切换
    auto rtspAux = std::make_unique<rcfpv::RtspCapture>(config.rtsp.url2, config.rtsp.transport,
                                                        config.rtsp.bufferSize, config.rtsp.codec);
    auto mavlink = std::make_unique<rcfpv::MavlinkBridge>(config.mavlink);

    // ---- 摄像头板载状态监听(设备 5gipc-status 脚本 TCP 上报 → OSD 板载状态) ----
    std::unique_ptr<rcfpv::StatusListener> statusListener;
    if (config.status.enabled) {
        statusListener = std::make_unique<rcfpv::StatusListener>(config.status.listenPort);
        mavlink->setDeviceStatusSource([&statusListener] { return statusListener->latest(); });
    }

    // ---- 音频桥(摄像头侧 USB 声卡编解码,本侧仅 UDP 透传) ----
    std::unique_ptr<rcfpv::UdpAudioBridge> audioBridge;
    if (config.audio.enabled) {
        audioBridge = std::make_unique<rcfpv::UdpAudioBridge>(
            config.audio.bridgeListenPort, config.audio.bridgeCameraIp,
            config.audio.bridgeCameraPort);
    }

    // ---- 互连回调 ----
    streamer->setSignalingClient(signaling.get());

    // 副路视频帧 → WebRTC 副视频轨(mid="video2")
    rtspAux->setFrameCallback([&streamer](const uint8_t *data, size_t size, bool isKeyframe,
                                          uint32_t rtpTimestamp) {
        streamer->onAuxVideoFrame(data, size, isKeyframe, rtpTimestamp);
    });

    // 主路视频帧 → WebRTC 视频轨
    rtsp->setFrameCallback([&streamer](const uint8_t *data, size_t size, bool isKeyframe,
                                       uint32_t rtpTimestamp) {
        streamer->onVideoFrame(data, size, isKeyframe, rtpTimestamp);
    });

    // 音频上行:摄像头音频桥 Opus 帧 → WebRTC 音频轨
    if (audioBridge) {
        audioBridge->setEncodedFrameCallback([&streamer](const uint8_t *data, size_t size,
                                                         int samples) {
            streamer->onAudioFrame(data, size, samples);
        });
    }

    // MAVLink 遥测 → 数据通道下行;数据通道命令 → MAVLink + 音频命令分流
    mavlink->setTelemetryCallback([&streamer](const std::string &json) {
        streamer->sendData(json);
    });

    // 音频桥命令回执 → 下行 JSON(覆盖本地缓存值)
    int micGainPct = static_cast<int>(config.audio.micGain * 100.0f);
    int hwVol = config.audio.micHwVolume;
    int spkVol = 50;
    if (audioBridge) {
        audioBridge->setCommandReplyCallback([&streamer, &micGainPct, &hwVol, &spkVol](
                                                 const std::string &line) {
            nlohmann::json reply;
            if (line.rfind("GAIN ", 0) == 0) {
                micGainPct = static_cast<int>(atof(line.c_str() + 5) * 100.0f + 0.5f);
                reply["t"] = "mic_gain";
                reply["gain"] = micGainPct;
            } else if (line.rfind("HW_VOL ", 0) == 0) {
                hwVol = atoi(line.c_str() + 7);
                reply["t"] = "mic_hw_volume";
                reply["value"] = hwVol;
            } else if (line.rfind("SPK_VOL ", 0) == 0) {
                spkVol = atoi(line.c_str() + 8);
                reply["t"] = "volume";
                reply["value"] = spkVol;
            } else {
                return; // 未知回执忽略
            }
            streamer->sendData(reply.dump());
        });
    }

    streamer->setDataChannelCallback([&mavlink, &streamer, &audioBridge, &config, &configPath,
                                      &micGainPct, &hwVol, &spkVol](const std::string &msg) {
        nlohmann::json j = nlohmann::json::parse(msg, nullptr, false);
        if (j.is_discarded()) {
            try {
                mavlink->handleCommand(msg);
            } catch (const std::exception &e) {
                std::cerr << "[main] 命令处理异常: " << e.what() << std::endl;
            }
            return;
        }
        std::string verb = j.value("cmd", j.value("type", ""));
        for (auto &c : verb) c = static_cast<char>(toupper(c));

        // ---- 音频命令(转发摄像头音频桥,回执经 CommandReplyCallback 转下行) ----
        if (verb == "SET_MIC_GAIN" || verb == "GET_MIC_GAIN") {
            if (verb == "SET_MIC_GAIN" && j.contains("gain") && j.at("gain").is_number()) {
                int pct = clampInt(static_cast<int>(j.at("gain").get<float>() + 0.5f), 0, 200);
                config.audio.micGain = pct / 100.0f;
                rcfpv::saveMicGain(configPath, config.audio.micGain);
                micGainPct = pct;
                if (audioBridge) audioBridge->sendCommand("SET_GAIN " + std::to_string(pct));
            }
            nlohmann::json reply;
            reply["t"] = "mic_gain";
            reply["gain"] = micGainPct;
            streamer->sendData(reply.dump());
            return;
        }
        if (verb == "SET_MIC_HW_VOLUME" || verb == "GET_MIC_HW_VOLUME") {
            if (verb == "SET_MIC_HW_VOLUME" && j.contains("value") && j.at("value").is_number()) {
                int pct = clampInt(j.at("value").get<int>(), 0, 100);
                config.audio.micHwVolume = pct;
                rcfpv::saveMicHwVolume(configPath, pct);
                hwVol = pct;
                if (audioBridge) audioBridge->sendCommand("SET_HW_VOL " + std::to_string(pct));
            }
            nlohmann::json reply;
            reply["t"] = "mic_hw_volume";
            reply["value"] = hwVol;
            streamer->sendData(reply.dump());
            return;
        }
        if (verb == "SET_VOLUME" || verb == "GET_VOLUME") {
            if (verb == "SET_VOLUME" && j.contains("value") && j.at("value").is_number()) {
                int v = clampInt(j.at("value").get<int>(), 0, 100);
                spkVol = v;
                if (audioBridge) audioBridge->sendCommand("SET_SPK_VOL " + std::to_string(v));
            } else if (verb == "GET_VOLUME") {
                if (audioBridge) audioBridge->sendCommand("GET_SPK_VOL");
            }
            nlohmann::json reply;
            reply["t"] = "volume";
            reply["value"] = spkVol;
            streamer->sendData(reply.dump());
            return;
        }

        // ---- 其余命令交 MAVLink 桥 ----
        try {
            mavlink->handleCommand(msg);
        } catch (const std::exception &e) {
            std::cerr << "[main] 命令处理异常: " << e.what() << std::endl;
        }
    });

    // 对讲下行:WebRTC RTP 包 → 摄像头音频桥解码播放
    if (audioBridge) {
        streamer->setTalkSink([&audioBridge](const uint8_t *data, size_t size) {
            audioBridge->sendDownstream(data, size);
        });
    } else {
        streamer->setTalkSink([](const uint8_t *, size_t) { /* 丢弃 */ });
    }

    // ---- 信令回调 → streamer ----
    signaling->setOnViewerJoin([&streamer](const std::string &viewerId) {
        streamer->createOfferForViewer(viewerId);
    });
    signaling->setOnAnswer([&streamer](const std::string &viewerId, const std::string &sdp) {
        streamer->handleAnswer(viewerId, sdp);
    });
    signaling->setOnCandidate([&streamer](const std::string &viewerId, const std::string &candidate,
                                          const std::string &mid) {
        streamer->handleCandidate(viewerId, candidate, mid);
    });
    signaling->setOnViewerLeave([&streamer](const std::string &viewerId) {
        streamer->removeViewer(viewerId);
    });

    // ---- 启动 ----
    std::cout << "[main] 启动主路 RTSP 采集: " << config.rtsp.url << std::endl;
    if (!rtsp->start())
        std::cerr << "[main] 主路 RTSP 采集启动失败(线程内会持续重连)" << std::endl;

    if (!config.rtsp.url2.empty()) {
        std::cout << "[main] 启动副路 RTSP 采集: " << config.rtsp.url2 << std::endl;
        rtspAux->start();
    }

    if (audioBridge) {
        if (!audioBridge->start())
            std::cerr << "[main] 音频桥启动失败" << std::endl;
    }

    if (statusListener) {
        if (!statusListener->start())
            std::cerr << "[main] 设备状态监听启动失败" << std::endl;
    }

    std::cout << "[main] 启动 MAVLink 桥(UDP :" << config.mavlink.fcListenPort << ")..." << std::endl;
    if (!mavlink->start())
        std::cerr << "[main] MAVLink 桥启动失败" << std::endl;

    std::cout << "[main] 连接信令服务器: " << config.signaling.serverUrl << std::endl;
    signaling->connect();

    std::cout << "[main] 系统就绪,等待 viewer 接入(Ctrl+C 退出)" << std::endl;

    // 链路看门狗喂狗:地面站数据通道在线时周期通知 MAVLink 桥;
    // 断链超过 5s 后桥会发一次中立 RC_OVERRIDE 并停发心跳(触发飞控 failsafe)
    auto lastFeed = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    while (!gStop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (streamer->hasActiveViewers()) {
            auto now = std::chrono::steady_clock::now();
            if (now - lastFeed >= std::chrono::seconds(1)) {
                mavlink->noteViewerActivity();
                lastFeed = now;
            }
        }
    }

    // ---- 优雅关闭 ----
    std::cout << "[main] 正在关闭..." << std::endl;
    signaling->disconnect();
    if (audioBridge) audioBridge->stop();
    if (statusListener) statusListener->stop();
    rtsp->stop();
    mavlink->stop();
    streamer.reset();
    signaling.reset();
    rtsp.reset();
    mavlink.reset();
    audioBridge.reset();
    rtc::Cleanup();
    std::cout << "[main] 已退出" << std::endl;
    return 0;
}
