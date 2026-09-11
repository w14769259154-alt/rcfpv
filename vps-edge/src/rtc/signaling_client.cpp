#include "rtc/signaling_client.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>

namespace rcfpv {

using nlohmann::json;

SignalingClient::SignalingClient(const SignalingConfig &cfg) : cfg_(cfg) {}

SignalingClient::~SignalingClient() { disconnect(); }

void SignalingClient::setOnViewerJoin(ViewerJoinCallback cb) { onViewerJoin_ = std::move(cb); }
void SignalingClient::setOnViewerLeave(ViewerLeaveCallback cb) { onViewerLeave_ = std::move(cb); }
void SignalingClient::setOnAnswer(AnswerCallback cb) { onAnswer_ = std::move(cb); }
void SignalingClient::setOnCandidate(CandidateCallback cb) { onCandidate_ = std::move(cb); }

bool SignalingClient::connect() {
    {
        std::lock_guard<std::mutex> lk(wsMutex_);
        if (connected_ && ws_ && ws_->isOpen()) return true;
    }

    std::cout << "[signaling] 连接: " << cfg_.serverUrl << std::endl;
    // 连云服务器 wss 自签证书时禁用 TLS 校验(disable_tls_verify=true)
    rtc::WebSocketConfiguration wsCfg;
    wsCfg.disableTlsVerification = cfg_.disableTlsVerify;
    auto ws = std::make_shared<rtc::WebSocket>(wsCfg);

    ws->onOpen([this] {
        connected_ = true;
        std::cout << "[signaling] 已连接,加入房间: " << cfg_.roomId << std::endl;
        json join = {{"type", "join"},
                     {"room", cfg_.roomId},
                     {"role", "broadcaster"},
                     {"id", cfg_.clientId}};
        // 持锁读 ws_(与 connect 的写入并发)
        std::lock_guard<std::mutex> lk(wsMutex_);
        if (ws_) ws_->send(join.dump());
    });

    ws->onMessage([this](rtc::message_variant data) {
        if (!std::holds_alternative<std::string>(data)) return;
        // 畸形消息(类型不匹配等)会抛 nlohmann 异常,必须整体捕获:
        // 异常逃逸进 libdatachannel 回调线程会导致 std::terminate
        try {
            json msg = json::parse(std::get<std::string>(data), nullptr, false);
            if (msg.is_discarded() || !msg.contains("type")) return;
            if (!msg.at("type").is_string()) return;

            const std::string &type = msg.at("type").get_ref<const std::string &>();
            // 字段先 is_string() 校验再取值,非字符串一律按缺失处理
            if (type == "viewer_ready" && msg.contains("viewerId") &&
                msg.at("viewerId").is_string()) {
                if (onViewerJoin_) onViewerJoin_(msg.at("viewerId").get<std::string>());
            } else if (type == "peer_left" && msg.contains("id") &&
                       msg.at("id").is_string()) {
                if (onViewerLeave_) onViewerLeave_(msg.at("id").get<std::string>());
            } else if (type == "answer" && msg.contains("from") && msg.contains("sdp") &&
                       msg.at("from").is_string() && msg.at("sdp").is_string()) {
                if (onAnswer_)
                    onAnswer_(msg.at("from").get<std::string>(),
                              msg.at("sdp").get<std::string>());
            } else if (type == "candidate" && msg.contains("from") &&
                       msg.at("from").is_string()) {
                // candidate/mid 可缺失或非字符串,按空串处理
                auto optStr = [&msg](const char *key) -> std::string {
                    return (msg.contains(key) && msg.at(key).is_string())
                               ? msg.at(key).get<std::string>()
                               : std::string{};
                };
                if (onCandidate_)
                    onCandidate_(msg.at("from").get<std::string>(),
                                 optStr("candidate"), optStr("mid"));
            }
        } catch (const std::exception &e) {
            std::cerr << "[signaling] 消息处理异常,已忽略: " << e.what() << std::endl;
        }
    });

    ws->onClosed([this] {
        connected_ = false;
        std::cout << "[signaling] 连接关闭,等待自动重连" << std::endl;
    });
    ws->onError([this](std::string err) {
        connected_ = false;
        std::cerr << "[signaling] 错误: " << err << std::endl;
    });

    // 锁内原子替换 ws_:旧连接先取出;close 必须在锁外执行
    // (close 可能同步触发回调,且旧连接不 close 会每秒泄漏一条 WebSocket)
    std::shared_ptr<rtc::WebSocket> oldWs;
    {
        std::lock_guard<std::mutex> lk(wsMutex_);
        oldWs = ws_;
        ws_ = ws;
    }
    // 局部引用保活,锁外安全关闭旧连接(随后局部析构释放)
    if (oldWs) oldWs->close();

    ws->open(cfg_.serverUrl);

    // 启动常驻重连监测线程
    if (!reconnectThread_.joinable())
        reconnectThread_ = std::thread(&SignalingClient::runReconnectLoop, this);
    return true;
}

void SignalingClient::disconnect() {
    stopping_ = true;
    if (reconnectThread_.joinable()) reconnectThread_.join();
    std::shared_ptr<rtc::WebSocket> ws;
    {
        std::lock_guard<std::mutex> lk(wsMutex_);
        ws = ws_;
        ws_.reset();
    }
    // 锁外 close(close 可能同步触发回调)
    if (ws) ws->close();
}

bool SignalingClient::sendJson(const std::string &jsonStr) {
    // 锁内拷贝 shared_ptr,锁外使用:与 connect 替换 ws_ 并发安全
    std::shared_ptr<rtc::WebSocket> ws;
    {
        std::lock_guard<std::mutex> lk(wsMutex_);
        ws = ws_;
    }
    if (ws && ws->isOpen()) {
        ws->send(jsonStr);
        std::cout << "[signaling] 已发送(" << jsonStr.size() << " 字节): "
                  << jsonStr.substr(0, 80) << std::endl;
        return true;
    }
    std::cerr << "[signaling] 发送失败(未连接),丢弃 " << jsonStr.substr(0, 64) << "..."
              << std::endl;
    return false;
}

void SignalingClient::runReconnectLoop() {
    while (!stopping_) {
        // 3s 间隔:1s 会造成重连风暴(每秒新建连接覆盖旧的)
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if (stopping_) break;

        // 以 connected_ 为准(而非 ws_->isOpen):连接被服务端 terminate 时
        // isOpen 可能仍返回 true,导致重连循环永远跳过
        if (!connected_) {
            std::cout << "[signaling] 自动重连..." << std::endl;
            connect();
        }
    }
}

} // namespace rcfpv
