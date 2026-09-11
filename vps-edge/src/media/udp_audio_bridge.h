/**
 * 5gipc-rc VPS edge — UdpAudioBridge(与摄像头音频桥的 UDP 对接)
 *
 * 摄像头音频桥(OpenIPC 侧)负责:USB 声卡采集→Opus 编码 / Opus 解码→声卡播放。
 * VPS 侧只需透传:
 *   - 上行: 摄像头 → VPS:listenPort 的 Opus 编码帧 → WebRtcStreamer::onAudioFrame
 *   - 下行: talkSink 收到的 WebRTC RTP 包 → 转发摄像头:cameraPort → 音频桥解码播放
 *   - 命令: 麦克风增益 / 采集音量 / 扬声器音量 的文本命令双向透传
 *
 * 帧/命令封装(与摄像头音频桥一致):
 *   - 音频帧:  [0x41][2B big-endian len][payload]     (上行=裸 Opus 帧;下行=WebRTC RTP 包)
 *   - 命令/回执: 纯文本 UTF-8,如 "SET_GAIN 1.5" / "GAIN 1.500" / "HW_VOL 60" / "SPK_VOL 80"
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <thread>

namespace rcfpv {

class UdpAudioBridge {
public:
    // 上行 Opus 编码帧回调:(数据, 字节数, 采样数);采样数用于 WebRTC RTP 时间戳推进
    using EncodedFrameCallback = std::function<void(const uint8_t *, size_t, int)>;
    // 摄像头侧命令回执回调:一行文本("GAIN 1.500" / "HW_VOL 60" / "SPK_VOL 80")
    using CommandReplyCallback = std::function<void(const std::string &line)>;

    UdpAudioBridge(int listenPort, std::string cameraIp, int cameraPort);
    ~UdpAudioBridge();

    UdpAudioBridge(const UdpAudioBridge &) = delete;
    UdpAudioBridge &operator=(const UdpAudioBridge &) = delete;

    void setEncodedFrameCallback(EncodedFrameCallback cb) { frameCb_ = std::move(cb); }
    void setCommandReplyCallback(CommandReplyCallback cb) { replyCb_ = std::move(cb); }

    // 对讲下行:WebRTC 收到的 RTP 包(含头) → 转发摄像头音频桥解码播放
    void sendDownstream(const uint8_t *data, size_t size);
    // 发送文本命令到摄像头音频桥("SET_GAIN 150" 等)
    void sendCommand(const std::string &cmd);

    bool start();
    void stop();

private:
    void run(); // recvfrom 循环:0x41 前缀→音频帧;纯文本→命令回执

    int listenPort_;
    std::string cameraIp_;
    int cameraPort_;

    EncodedFrameCallback frameCb_;
    CommandReplyCallback replyCb_;

    int fd_ = -1;
    sockaddr_in camAddr_{};
    std::mutex sendMutex_; // 保护 fd_ 上的并发 sendto(下行帧线程 + 命令线程)
    std::thread thread_;
    std::atomic<bool> stopFlag_{false};
};

} // namespace rcfpv
