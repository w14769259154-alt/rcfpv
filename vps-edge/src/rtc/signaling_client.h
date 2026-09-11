/**
 * RCfpv Edge — WebSocket 信令客户端(broadcaster 角色)
 *
 * - 连接信令服务器并加入房间
 * - viewer 加入时回调(由 WebRtcStreamer 发起 offer)
 * - 转发 answer / candidate 给 WebRtcStreamer
 * - 常驻监测线程:连接断开后自动重连(周期 3s 轮询,幂等)
 */
#pragma once

#include "core/config.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rcfpv {

class SignalingClient {
public:
    // viewerId
    using ViewerJoinCallback = std::function<void(const std::string &)>;
    using ViewerLeaveCallback = std::function<void(const std::string &)>;
    // viewerId, sdp
    using AnswerCallback = std::function<void(const std::string &, const std::string &)>;
    // viewerId, candidate, mid
    using CandidateCallback =
        std::function<void(const std::string &, const std::string &, const std::string &)>;

    explicit SignalingClient(const SignalingConfig &cfg);
    ~SignalingClient();

    SignalingClient(const SignalingClient &) = delete;
    SignalingClient &operator=(const SignalingClient &) = delete;

    void setOnViewerJoin(ViewerJoinCallback cb);
    void setOnViewerLeave(ViewerLeaveCallback cb);
    void setOnAnswer(AnswerCallback cb);
    void setOnCandidate(CandidateCallback cb);

    bool connect();
    void disconnect();

    // 发送 JSON 文本到信令服务器;未连接时返回 false
    bool sendJson(const std::string &jsonStr);

private:
    void runReconnectLoop(); // 常驻:断线自动重连

    SignalingConfig cfg_;
    // ws_ 并发访问:重连线程写(connect 赋值)/ libdatachannel 回调线程读
    // (onOpen 里 send、sendJson),必须用 wsMutex_ 保护;
    // close 一律在锁外执行(close 可能同步触发回调,持锁会死锁)
    std::shared_ptr<rtc::WebSocket> ws_;
    std::mutex wsMutex_;

    ViewerJoinCallback onViewerJoin_;
    ViewerLeaveCallback onViewerLeave_;
    AnswerCallback onAnswer_;
    CandidateCallback onCandidate_;

    std::atomic<bool> stopping_{false};
    std::thread reconnectThread_;
};

} // namespace rcfpv
