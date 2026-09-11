/**
 * RCfpv Edge — WebRTC 推流器(broadcaster,多 viewer)
 *
 * - 每个 viewer 一条 PeerConnection:H265/H264 视频轨 + Opus 音频轨 + "mavlink" 数据通道
 * - 视频轨链:H26xRtpPacketizer + RtcpSrReporter + RtcpNackResponder + PliHandler
 * - 音频轨链:OpusRtpPacketizer + RtcpSrReporter(48kHz 单声道)
 * - 关键帧对齐:新 viewer / 收到 PLI 后,等待下一个关键帧再发送
 * - M2 接入:MavlinkBridge(遥测下行 sendData / 命令上行 dataChannelCallback)
 */
#pragma once

#include "core/config.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rcfpv {

class SignalingClient;

class WebRtcStreamer {
public:
    // codec: "H265" 或 "H264"(来自 RtspConfig::codec)
    WebRtcStreamer(const WebRtcConfig &cfg, const std::string &codec);
    ~WebRtcStreamer();

    WebRtcStreamer(const WebRtcStreamer &) = delete;
    WebRtcStreamer &operator=(const WebRtcStreamer &) = delete;

    void setSignalingClient(SignalingClient *signaling);

    // 数据通道(上行命令)回调;由 main 安装(M2: dispatcher)
    void setDataChannelCallback(std::function<void(const std::string &)> cb);

    // 语音对讲下行:地面站 mic 的 RTP 包回调(AudioPlayer::onRtpPacket);
    // 未设置(为空)时对讲轨仍加入 offer,但收到的包被丢弃
    void setTalkSink(std::function<void(const uint8_t *, size_t)> sink);

    // 数据通道下行(M2: 遥测 JSON → 所有 viewer)
    void sendData(const std::string &msg);

    // 是否存在数据通道已打开的 viewer(供 MAVLink 桥链路看门狗周期查询)
    bool hasActiveViewers();

    // viewer 生命周期(由 SignalingClient 回调驱动)
    void createOfferForViewer(const std::string &viewerId);
    void handleAnswer(const std::string &viewerId, const std::string &sdp);
    void handleCandidate(const std::string &viewerId, const std::string &candidate,
                         const std::string &mid);
    void removeViewer(const std::string &viewerId);

    // 视频帧入口(RtspCapture 回调,Annex B)
    void onVideoFrame(const uint8_t *data, size_t size, bool isKeyframe, uint32_t rtpTimestamp);

    // 副视频帧入口(副路 RtspCapture 回调,双摄 PIP;Annex B)
    void onAuxVideoFrame(const uint8_t *data, size_t size, bool isKeyframe, uint32_t rtpTimestamp);

    // 音频帧入口(AudioCapture 回调,Opus 编码帧;samples 用于 RTP 时间戳推进)
    void onAudioFrame(const uint8_t *data, size_t size, int samples);

private:
    struct Viewer {
        std::shared_ptr<rtc::PeerConnection> pc;
        std::shared_ptr<rtc::Track> track;
        std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig;
        // 副视频轨(mid="video2",双摄 PIP;url2 为空时轨存在但无帧)
        std::shared_ptr<rtc::Track> auxTrack;
        std::shared_ptr<rtc::RtpPacketizationConfig> auxRtpConfig;
        std::shared_ptr<std::atomic<bool>> needAuxKeyframe =
            std::make_shared<std::atomic<bool>>(true);
        uint32_t lastAuxTs = 0; // 副路已发最后一帧 RTP 时间戳(严格单调)
        std::shared_ptr<rtc::Track> audioTrack;
        std::shared_ptr<rtc::RtpPacketizationConfig> audioRtpConfig;
        std::shared_ptr<rtc::Track> talkTrack; // 对讲轨(RecvOnly,收地面站 mic)
        std::shared_ptr<rtc::DataChannel> dc;
        // 初始/PLI 后等待关键帧;shared_ptr 保证 PliHandler 回调安全
        std::shared_ptr<std::atomic<bool>> needKeyframe =
            std::make_shared<std::atomic<bool>>(true);
        // 该 viewer 已发送的最后一帧视频 RTP 时间戳:
        // 补发缓存关键帧时保证严格单调递增,避免与已发帧撞戳(花屏)
        uint32_t lastVideoTs = 0;
    };
    using ViewerPtr = std::shared_ptr<Viewer>;

    ViewerPtr getViewer(const std::string &viewerId); // 调用方持有 mutex_

    // 带身份校验的移除:仅当 viewers_ 中该 id 对应 expect 同一对象时才移除,
    // 防止被替换的旧 PeerConnection 迟到的状态回调误删新会话
    void removeViewer(const std::string &viewerId, const ViewerPtr &expect);

    WebRtcConfig cfg_;
    std::string codec_; // "H265" / "H264"
    SignalingClient *signaling_ = nullptr;
    std::function<void(const std::string &)> dcCallback_;
    std::function<void(const uint8_t *, size_t)> talkSink_;

    uint32_t audioTimestamp_ = 0; // 音频 RTP 时间戳(采样计数,48kHz)

    // 最近一个关键帧缓存(含注入的参数集,Annex B 完整帧)。
    // 新 viewer 接入 / PLI 时立即补发,不再等下一个 GOP(摄像头 GOP=5s)。
    // 仅 onVideoFrame(单线程)访问,受 mutex_ 保护。
    std::vector<std::byte> lastKeyframe_;

    // 副路最近关键帧缓存(同 lastKeyframe_,供新 viewer/PLI 立即补发)
    std::vector<std::byte> lastAuxKeyframe_;

    // 视频发送统计(诊断用)
    std::chrono::steady_clock::time_point lastVideoStat_{};
    std::chrono::steady_clock::time_point videoStatStart_{};
    uint64_t videoStatFrames_ = 0;
    uint64_t videoStatBytes_ = 0;

    std::mutex mutex_; // 保护 viewers_
    std::map<std::string, ViewerPtr> viewers_;
};

} // namespace rcfpv
