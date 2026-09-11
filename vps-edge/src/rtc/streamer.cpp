#include "rtc/streamer.h"

#include "rtc/signaling_client.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>

namespace rcfpv {

using nlohmann::json;

WebRtcStreamer::WebRtcStreamer(const WebRtcConfig &cfg, const std::string &codec)
    : cfg_(cfg), codec_(codec == "H264" ? "H264" : "H265") {}

WebRtcStreamer::~WebRtcStreamer() {
    // 不能持锁 close:libdatachannel 状态回调可能同步触发,
    // onStateChange → removeViewer 会再次 lock 同一 mutex_ → 同线程二次加锁死锁。
    // 先在锁内移出全部 viewer 并 clear,然后在锁外逐个 close。
    std::vector<ViewerPtr> toClose;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto &[id, v] : viewers_)
            toClose.push_back(v);
        viewers_.clear();
    }
    // 锁外 close:触发的 onStateChange → removeViewer 会发现 viewer 已不存在,安全返回
    for (auto &v : toClose)
        if (v->pc) v->pc->close();
}

void WebRtcStreamer::setSignalingClient(SignalingClient *signaling) { signaling_ = signaling; }

void WebRtcStreamer::setDataChannelCallback(std::function<void(const std::string &)> cb) {
    dcCallback_ = std::move(cb);
}

void WebRtcStreamer::setTalkSink(std::function<void(const uint8_t *, size_t)> sink) {
    talkSink_ = std::move(sink);
}

void WebRtcStreamer::sendData(const std::string &msg) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto &[id, v] : viewers_) {
        if (v->dc && v->dc->isOpen())
            v->dc->send(msg);
    }
}

bool WebRtcStreamer::hasActiveViewers() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto &[id, v] : viewers_)
        if (v->dc && v->dc->isOpen())
            return true;
    return false;
}

WebRtcStreamer::ViewerPtr WebRtcStreamer::getViewer(const std::string &viewerId) {
    auto it = viewers_.find(viewerId);
    return it != viewers_.end() ? it->second : nullptr;
}

void WebRtcStreamer::createOfferForViewer(const std::string &viewerId) {
    // viewer 快速重连时旧会话可能仍残留在 viewers_(状态回调尚未清理完),
    // 不能忽略请求——否则新 viewer 永远收不到 offer;先替换旧会话再重建
    {
        ViewerPtr oldViewer;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = viewers_.find(viewerId);
            if (it != viewers_.end()) {
                oldViewer = it->second;
                viewers_.erase(it);
                std::cout << "[webrtc] viewer 重复接入,移除旧会话: " << viewerId << std::endl;
            }
        }
        // 锁外 close 旧 pc(close 可能同步触发 onStateChange → removeViewer;
        // 旧条目已 erase 且新会话尚未创建,removeViewer 安全返回)
        if (oldViewer && oldViewer->pc) oldViewer->pc->close();
    }

    std::lock_guard<std::mutex> lk(mutex_);
    std::cout << "[webrtc] 为 viewer 创建 offer: " << viewerId << std::endl;

    rtc::Configuration rtcCfg;
    for (const auto &s : cfg_.stunServers)
        rtcCfg.iceServers.emplace_back(s);
    // TURN 中继:公网 NAT 后场景,ICE 打洞失败时经云服务器转发视频/音频/数据通道
    for (const auto &t : cfg_.turnServers) {
        if (t.hostname.empty()) continue;
        rtc::IceServer::RelayType relay =
            t.transport == "tcp" ? rtc::IceServer::RelayType::TurnTcp :
            t.transport == "tls" ? rtc::IceServer::RelayType::TurnTls :
                                  rtc::IceServer::RelayType::TurnUdp;
        rtcCfg.iceServers.emplace_back(t.hostname, static_cast<uint16_t>(t.port),
                                       t.username, t.credential, relay);
    }

    auto viewer = std::make_shared<Viewer>();
    viewer->pc = std::make_shared<rtc::PeerConnection>(rtcCfg);
    auto pc = viewer->pc;

    // ---- SDP / ICE 回调必须最先注册! ----
    // libdatachannel 在首个 createDataChannel/addTrack 时会自动发起协商,
    // 回调晚注册会错过自动生成的 offer(竞态导致对端永远收不到 offer)
    pc->onLocalDescription([this, viewerId](rtc::Description desc) {
        std::string sdp = std::string(desc);
        std::cout << "[webrtc] offer 已生成(" << sdp.size() << " 字节) → " << viewerId
                  << std::endl;
        json j = {{"type", "offer"},
                  {"to", viewerId},
                  {"sdp", sdp}};
        if (signaling_) signaling_->sendJson(j.dump());
    });
    pc->onLocalCandidate([this, viewerId](rtc::Candidate cand) {
        std::string c = std::string(cand);
        std::cout << "[webrtc] 本地候选(" << c << ") → " << viewerId << std::endl;
        json j = {{"type", "candidate"},
                  {"to", viewerId},
                  {"candidate", c},
                  {"mid", cand.mid()}};
        if (signaling_) signaling_->sendJson(j.dump());
    });
    // weak_ptr 防循环引用(viewer 持有 pc,若 pc 回调再持有 viewer 的 shared_ptr 则泄漏);
    // 状态回调做身份校验:被替换的旧 pc 迟到的回调不会误删新会话
    std::weak_ptr<Viewer> weakViewer = viewer;
    pc->onStateChange([this, viewerId, weakViewer](rtc::PeerConnection::State state) {
        std::cout << "[webrtc] viewer " << viewerId << " 状态: " << int(state) << std::endl;
        // Disconnected 也立即清理:viewer 快速重连时旧会话尽快释放,
        // 不必等 ICE 超时转 Failed/Closed(往往要 15~30s)
        if (state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Disconnected ||
            state == rtc::PeerConnection::State::Closed) {
            auto v = weakViewer.lock();
            if (v) removeViewer(viewerId, v);
        }
    });

    // ---- 视频轨 ----
    const bool isH265 = (codec_ == "H265");
    rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
    if (isH265)
        media.addH265Codec(cfg_.videoPayloadType);
    else
        media.addH264Codec(cfg_.videoPayloadType);
    media.addSSRC(cfg_.videoSsrc, "video-send");

    viewer->track = pc->addTrack(media);

    viewer->rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        cfg_.videoSsrc, "video-send", cfg_.videoPayloadType,
        isH265 ? rtc::H265RtpPacketizer::ClockRate : rtc::H264RtpPacketizer::ClockRate);

    std::shared_ptr<rtc::RtpPacketizer> packetizer;
    // StartSequence 模式同时识别 3/4 字节起始码;Long 模式只认 00 00 00 01,
    // 而多数 RTSP 源 H265 流用 3 字节起始码,会导致 NAL 无法切分(解码必败)
    if (isH265)
        packetizer = std::make_shared<rtc::H265RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, viewer->rtpConfig);
    else
        packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, viewer->rtpConfig);

    // SR 报告 + NACK 重传 + PLI 关键帧请求
    packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(viewer->rtpConfig));
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    packetizer->addToChain(std::make_shared<rtc::PliHandler>(
        [flag = viewer->needKeyframe] { *flag = true; }));
    viewer->track->setMediaHandler(packetizer);

    // ---- 副视频轨(mid="video2",双摄 PIP) ----
    // 始终创建(url2 为空时无帧,浏览器按轨 mute 状态隐藏小窗),
    // 避免 SET_AUX_URL 运行时改地址触发重协商
    {
        rtc::Description::Video auxMedia("video2", rtc::Description::Direction::SendOnly);
        if (isH265)
            auxMedia.addH265Codec(cfg_.videoPayloadType);
        else
            auxMedia.addH264Codec(cfg_.videoPayloadType);
        auxMedia.addSSRC(cfg_.auxVideoSsrc, "video2-send");

        viewer->auxTrack = pc->addTrack(auxMedia);
        viewer->auxRtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
            cfg_.auxVideoSsrc, "video2-send", cfg_.videoPayloadType,
            isH265 ? rtc::H265RtpPacketizer::ClockRate : rtc::H264RtpPacketizer::ClockRate);

        std::shared_ptr<rtc::RtpPacketizer> auxPacketizer;
        if (isH265)
            auxPacketizer = std::make_shared<rtc::H265RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, viewer->auxRtpConfig);
        else
            auxPacketizer = std::make_shared<rtc::H264RtpPacketizer>(
                rtc::NalUnit::Separator::StartSequence, viewer->auxRtpConfig);

        auxPacketizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(viewer->auxRtpConfig));
        auxPacketizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        auxPacketizer->addToChain(std::make_shared<rtc::PliHandler>(
            [flag = viewer->needAuxKeyframe] { *flag = true; }));
        viewer->auxTrack->setMediaHandler(auxPacketizer);
    }

    // ---- 音频轨(Opus 48kHz,AudioCapture 编码帧直发) ----
    rtc::Description::Audio audioMedia("audio", rtc::Description::Direction::SendOnly);
    audioMedia.addOpusCodec(cfg_.audioPayloadType);
    audioMedia.addSSRC(cfg_.audioSsrc, "audio-send");

    viewer->audioTrack = pc->addTrack(audioMedia);
    viewer->audioRtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        cfg_.audioSsrc, "audio-send", cfg_.audioPayloadType, rtc::OpusRtpPacketizer::DefaultClockRate);

    auto audioPacketizer = std::make_shared<rtc::OpusRtpPacketizer>(viewer->audioRtpConfig);
    audioPacketizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(viewer->audioRtpConfig));
    viewer->audioTrack->setMediaHandler(audioPacketizer);

    // ---- 对讲轨(RecvOnly:收地面站 mic) ----
    // 始终添加对讲轨:即使 AudioPlayer 不可用(talkSink_ 为空),也要让浏览器
    // 请求麦克风权限并建立上行 RTP 通路。talkSink_ 为空时收到的包直接丢弃。
    //
    // 关键:libdatachannel 对于本地 addTrack 创建的 RecvOnly 轨,不会在
    // setRemoteDescription(answer) 时触发 onTrack 回调(onTrack 主要用于
    // 收到 offer 时创建的远端轨)。因此必须保存 addTrack 返回的轨对象,
    // 直接在其上注册 onMessage,否则浏览器发送的 RTP 永远无人接收。
    //
    // ⚠️ 必须添加 RtcpReceivingSession:libdatachannel 0.24.5 已知 BUG
    // 如果 RecvOnly 轨没有任何 mediaHandler,edge 不会响应浏览器的 RTCP SR,
    // 也不发 RR,浏览器约 1 分钟后进入"保守模式"(降低音量/码率),
    // 表现为"运行一段时间就没声"(但日志看似一切正常,因为 RTP 仍在持续收到)
    // 加 RtcpReceivingSession 后:libdatachannel 收到浏览器 SR 时会响应 RR,
    // 浏览器认为对端正常接收,持续发送正常音频
    {
        rtc::Description::Audio talkMedia("talk", rtc::Description::Direction::RecvOnly);
        talkMedia.addOpusCodec(cfg_.audioPayloadType);
        viewer->talkTrack = pc->addTrack(talkMedia);

        // 添加 RTCP 接收会话:响应浏览器 SR + 发送 RR,避免浏览器进入保守模式
        viewer->talkTrack->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

        std::cout << "[webrtc] 对讲轨已创建: " << viewerId
                  << " (sink=" << (talkSink_ ? "已连接" : "未连接(丢弃)") << ")"
                  << std::endl;

        // 直接在对讲轨上注册 onMessage(不依赖 onTrack 回调)
        static std::atomic<int> talkPktCount{0};
        viewer->talkTrack->onMessage([this](rtc::message_variant data) {
            if (!std::holds_alternative<rtc::binary>(data)) return;
            auto &pkt = std::get<rtc::binary>(data);
            int n = ++talkPktCount;
            if (n <= 5 || n % 100 == 0)
                std::cout << "[webrtc] 对讲 RTP 收到 #" << n
                          << " (" << pkt.size() << " 字节, sink="
                          << (talkSink_ ? "已连接" : "未连接") << ")" << std::endl;
            if (talkSink_)
                talkSink_(reinterpret_cast<const uint8_t *>(pkt.data()), pkt.size());
            // else: AudioPlayer 不可用,丢弃
        });
    }

    // ---- 数据通道(offer 侧创建,浏览器通过 ondatachannel 接收) ----
    viewer->dc = pc->createDataChannel("mavlink");
    viewer->dc->onOpen([viewerId] {
        std::cout << "[webrtc] 数据通道已打开: " << viewerId << std::endl;
    });
    viewer->dc->onMessage([this, viewerId](rtc::message_variant data) {
        if (!std::holds_alternative<std::string>(data)) return;
        std::string msg = std::get<std::string>(data);
        if (dcCallback_) {
            dcCallback_(msg); // M2: 命令分发器
        } else {
            std::cout << "[webrtc] 数据通道消息(暂未处理): " << viewerId << " "
                      << msg.substr(0, 120) << std::endl;
        }
    });

    // 自动协商:createDataChannel 已触发 offer 生成,回调已就位

    viewers_[viewerId] = viewer;
}

void WebRtcStreamer::handleAnswer(const std::string &viewerId, const std::string &sdp) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto v = getViewer(viewerId);
    if (!v) return;
    try {
        v->pc->setRemoteDescription(rtc::Description(sdp, "answer"));
        std::cout << "[webrtc] answer 已设置: " << viewerId << std::endl;
    } catch (const std::exception &e) {
        std::cerr << "[webrtc] 设置 answer 失败: " << e.what() << std::endl;
    }
}

void WebRtcStreamer::handleCandidate(const std::string &viewerId, const std::string &candidate,
                                     const std::string &mid) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto v = getViewer(viewerId);
    if (!v || candidate.empty()) return;
    try {
        v->pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
    } catch (const std::exception &e) {
        std::cerr << "[webrtc] 添加 candidate 失败: " << e.what() << std::endl;
    }
}

void WebRtcStreamer::removeViewer(const std::string &viewerId) {
    removeViewer(viewerId, nullptr);
}

// 带身份校验:仅当 viewers_ 中该 id 对应的条目与 expect 为同一对象时才移除,
// 防止被替换的旧 PeerConnection 迟到的状态回调误删新会话
void WebRtcStreamer::removeViewer(const std::string &viewerId, const ViewerPtr &expect) {
    ViewerPtr v;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = viewers_.find(viewerId);
        if (it == viewers_.end()) return;   // 已不存在(可能刚被替换/清理),安全返回
        if (expect && it->second != expect) return; // 已是新会话,不是本 viewer
        v = it->second;
        viewers_.erase(it);
    }
    // 锁外 close(既有模式):close 可能同步触发 onStateChange,
    // 回调再进 removeViewer 时条目已不存在,安全返回
    if (v->pc) v->pc->close();
    std::cout << "[webrtc] viewer 已移除: " << viewerId << std::endl;
}

void WebRtcStreamer::onVideoFrame(const uint8_t *data, size_t size, bool isKeyframe,
                                  uint32_t rtpTimestamp) {
    std::lock_guard<std::mutex> lk(mutex_);
    uint64_t sentFrames = 0, sentBytes = 0;
    // 5s 统计窗口累计(单 RTSP 线程调用 + mutex_,静态安全)
    static uint64_t accKeys = 0, accKeyWait = 0, accClosed = 0, accCacheSent = 0;

    // 缓存最近关键帧(已含参数集,可独立解码):新 viewer/PLI 立即补发
    if (isKeyframe)
        lastKeyframe_.assign(reinterpret_cast<const std::byte *>(data),
                             reinterpret_cast<const std::byte *>(data) + size);

    for (auto &[id, v] : viewers_) {
        if (!v->track || !v->track->isOpen()) {
            ++accClosed;
            continue;
        }

        // 关键帧对齐:PLI/新建 viewer 后立即补发缓存关键帧(不等下一个 GOP),
        // 后续 P 帧与缓存关键帧同属一个 GOP,可连续解码。
        // 补发帧时间戳必须严格大于该 viewer 已发的最后一帧:
        // 固定 -3600 在 25fps 时与上一帧同戳、30fps 时回退,都会花屏
        if (v->needKeyframe->load()) {
            if (isKeyframe) {
                *v->needKeyframe = false; // 当前帧就是关键帧,正常发送
            } else if (!lastKeyframe_.empty()) {
                const uint32_t ts = std::max(rtpTimestamp, v->lastVideoTs + 1);
                v->rtpConfig->timestamp = ts;
                v->track->send(lastKeyframe_.data(), lastKeyframe_.size());
                v->lastVideoTs = ts;
                ++accCacheSent;
                *v->needKeyframe = false;
            } else {
                ++accKeyWait; // 无缓存(刚启动)才等真关键帧
                continue;
            }
        }

        // 正常发帧:同样保持严格单调递增(补发帧可能已占用当前帧的时间戳)
        const uint32_t sendTs = std::max(rtpTimestamp, v->lastVideoTs + 1);
        v->rtpConfig->timestamp = sendTs;
        v->lastVideoTs = sendTs;
        v->track->send(reinterpret_cast<const std::byte *>(data), size);
        ++sentFrames;
        sentBytes += size;
    }
    if (isKeyframe) ++accKeys;

    // 发送统计(每 5s 一条):帧数/关键帧数/等关键帧跳过/通道未开跳过
    auto now = std::chrono::steady_clock::now();
    if (now - lastVideoStat_ >= std::chrono::seconds(5)) {
        lastVideoStat_ = now;
        auto secs = 5.0;
        std::cout << "[webrtc] 视频发送: " << videoStatFrames_ << " 帧 / "
                  << (videoStatBytes_ / 1024) << " KB (" << (videoStatFrames_ / secs)
                  << " fps, viewers=" << viewers_.size() << ", 关键帧=" << accKeys
                  << ", 缓存补发=" << accCacheSent
                  << ", 等关键帧跳过=" << accKeyWait << ", 通道未开跳过=" << accClosed
                  << ")" << std::endl;
        videoStatFrames_ = 0;
        videoStatBytes_ = 0;
        accKeys = accKeyWait = accClosed = accCacheSent = 0;
    }
    videoStatFrames_ += sentFrames;
    videoStatBytes_ += sentBytes;
}

void WebRtcStreamer::onAuxVideoFrame(const uint8_t *data, size_t size, bool isKeyframe,
                                     uint32_t rtpTimestamp) {
    // 与 onVideoFrame 同构:关键帧缓存 + PLI/新 viewer 补发 + 严格单调时间戳
    std::lock_guard<std::mutex> lk(mutex_);

    if (isKeyframe)
        lastAuxKeyframe_.assign(reinterpret_cast<const std::byte *>(data),
                                reinterpret_cast<const std::byte *>(data) + size);

    for (auto &[id, v] : viewers_) {
        if (!v->auxTrack || !v->auxTrack->isOpen()) continue;

        if (v->needAuxKeyframe->load()) {
            if (isKeyframe) {
                *v->needAuxKeyframe = false;
            } else if (!lastAuxKeyframe_.empty()) {
                const uint32_t ts = std::max(rtpTimestamp, v->lastAuxTs + 1);
                v->auxRtpConfig->timestamp = ts;
                v->auxTrack->send(lastAuxKeyframe_.data(), lastAuxKeyframe_.size());
                v->lastAuxTs = ts;
                *v->needAuxKeyframe = false;
            } else {
                continue; // 无缓存,等真关键帧
            }
        }

        const uint32_t sendTs = std::max(rtpTimestamp, v->lastAuxTs + 1);
        v->auxRtpConfig->timestamp = sendTs;
        v->lastAuxTs = sendTs;
        v->auxTrack->send(reinterpret_cast<const std::byte *>(data), size);
    }
}

void WebRtcStreamer::onAudioFrame(const uint8_t *data, size_t size, int samples) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (viewers_.empty()) {
        audioTimestamp_ += static_cast<uint32_t>(samples); // 无 viewer 也保持时间推进
        return;
    }
    for (auto &[id, v] : viewers_) {
        if (!v->audioTrack || !v->audioTrack->isOpen()) continue;
        v->audioRtpConfig->timestamp = audioTimestamp_;
        v->audioTrack->send(reinterpret_cast<const std::byte *>(data), size);
    }
    audioTimestamp_ += static_cast<uint32_t>(samples);
}

} // namespace rcfpv
