/**
 * RCfpv Edge — MAVLink 桥接(官方 c_library_v2,ArduPilot 方言)
 *
 * - UDP 监听 fc_listen_port 收飞控数据,命令发往 fc_target
 * - 源地址学习:仅当包解析出有效飞控 HEARTBEAT(autopilot 非 INVALID)才学习/更新,
 *   防 QGC/端口扫描包抢先毒化地址;遥测 10s 超时自动重置重新学习
 * - 以 system_id 发送 heartbeatRateHz 心跳(MAV_TYPE_GCS)触发飞控上行
 * - 遥测转 JSON(t:"tel")回调给 WebRtcStreamer → 数据通道下行
 * - handleCommand(JSON) 数据通道上行命令 → MAVLink 帧:
 *   SET_MODE / ARM / DISARM / SET_SPEED / SET_ALT / SET_RADIUS / VTOL / RC_OVERRIDE
 *
 * 说明:官方库 CRC_EXTRA 表完整,正常走 CRC 校验
 * (旧项目跳过 CRC 是因为自实现表不完整导致误丢,该问题已根治)。
 */
#pragma once

#include "core/config.h"
#include <ardupilotmega/mavlink.h> // mavlink_message_t(handleTelemetry 签名)

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>

namespace rcfpv {

class MavlinkBridge {
public:
    // 完整遥测 JSON 文本(含 "t":"tel")
    using TelemetryCallback = std::function<void(const std::string &json)>;

    explicit MavlinkBridge(const MavlinkConfig &cfg);
    ~MavlinkBridge();

    MavlinkBridge(const MavlinkBridge &) = delete;
    MavlinkBridge &operator=(const MavlinkBridge &) = delete;

    void setTelemetryCallback(TelemetryCallback cb);
    bool start();
    void stop();

    // 数据通道上行命令(JSON 文本,来自 WebRtcStreamer)
    void handleCommand(const std::string &jsonCmd);

    // 发送原始 MAVLink 帧(转发用,M6)
    void sendRawFrame(const uint8_t *data, size_t size);

    // 多目标遥测转发:设置目标列表("ip:port"),立即生效并持久化;
    // 飞控数据副本转发给所有目标,目标发来的命令透传给飞控
    void setForwardTargets(const std::vector<std::string> &targets, bool persist = true);
    std::vector<std::string> forwardTargets() const;

    // viewer 活动通知(链路丢失看门狗):streamer 有 viewer 时周期调用;
    // 从未调用过则视为始终有活动(兼容未接入的调用方,行为与旧版一致)
    void noteViewerActivity();

private:
    // transport 抽象:Udp(socket+recvfrom)或 Serial(串口 read/write)
    // gcsFd_(14550 GCS 直连)与 transport 无关,始终保留作为飞控↔GCS 桥接 UDP 出口
    enum class Transport { Udp, Serial };

    void run(); // 收发线程(poll fd_ + gcsFd_;fd_ 可能是 UDP socket 或串口)
    void sendHeartbeat();
    void sendBuffer(const uint8_t *buf, size_t len); // 线程安全发送到飞控(UDP sendto / Serial write)
    void sendLocked(const uint8_t *buf, size_t len); // 已持 sendMutex_ 时发送(sendPacked 内部复用)
    // 持锁完成 pack+发送:pack 会推进通道 TX seq,pack 与发送须在同一临界区,
    // 否则多线程并发 pack/send 导致 seq 乱序与数据竞态
    void sendPacked(const std::function<void(mavlink_message_t *)> &packer);
    void handleTelemetry(const mavlink_message_t *msg); // 用官方 decode 函数,规避 packed/对齐/v2 扩展问题
    bool isForwardSource(const sockaddr_in &addr) const; // 来源是否为转发目标(GCS)
    void handleGcsPacket();     // 14550 收包:登记动态 GCS + 透传给飞控
    void loadForwardTargets();   // 启动时读 config/forward_targets.json(优先于配置文件)
    void persistForwardTargets() const;

    // 飞控数据统一处理(UDP src 非空做源地址学习;Serial src=nullptr 直接解析+转发)
    void handleFcData(const uint8_t *buf, size_t len, const sockaddr_in *src);

    // 探测 UDP 包是否含有效飞控 HEARTBEAT(autopilot 非 INVALID):地址学习判据,
    // 用独立解析通道试探,不污染飞控方向(COMM_0)的解析状态
    bool probeFcHeartbeat(const uint8_t *buf, size_t len) const;

    // handleCommand 的实现体(外壳做整体异常防护,catch 后回错误回复)
    void handleCommandImpl(const std::string &jsonCmd);

    // transport 生命周期:openFcTransport 按 transport_ 打开 fd_(UDP socket 或 serial fd)
    bool openFcTransport();
    void closeFcTransport(); // 仅关 fd_,不碰 gcsFd_

    // 运行时热切换:停 run 线程 → 关旧 fd_ → 切状态 → 开新 fd_(失败回退)→ 重启线程
    bool reconfigure(const std::string &connection, const std::string &device, int baudrate);
    // 回执 t:"fc_connection"(withOk=true 时附带切换结果 ok 字段)
    void replyConnectionState(bool withOk, bool ok);

    // 16 通道全 1500 中立 RC_CHANNELS_OVERRIDE(viewer 丢失停车切换沿发送一次)
    void sendNeutralOverride();
    // 命令错误回复(t:"cmd_error"):字段缺失/类型非法/处理异常
    void sendCmdError(const std::string &error);

    MavlinkConfig cfg_;
    TelemetryCallback telemetryCb_;

    int fd_ = -1;
    Transport transport_ = Transport::Udp; // 当前 FC transport(openFcTransport 按此分支)
    // 串口 fd 错误(拔出等)后的重开时间点(run 循环每 2s 重试 openFcTransport 串口)
    std::chrono::steady_clock::time_point reopenAtMs_{};
    std::string serialDevice_;             // 实际生效串口路径(自动探测后回填)
    int serialBaudrate_ = 115200;
    bool serialAutoDetected_ = false;      // 串口路径是否为自动探测结果
    sockaddr_in targetAddr_{}; // 飞控地址(源地址学习后更新;Serial 模式不用)
    std::mutex sendMutex_;     // 保护 targetAddr_ + fd_ 生命周期 + 发送(TX seq)
    bool addrLearned_ = false;
    // stop()/reconfigure() 线程生命周期互斥:串行 join/重启,防并发 double-join UB
    std::mutex lifecycleMutex_;

    // GCS 直连监听(默认 14550,QGC 惯例):GCS→FC 透传;FC 遥测经此口回推
    // 动态 GCS:最近 15s 内发过包的直连地面站(地址学习,无需预配置)
    int gcsFd_ = -1;
    mutable std::mutex dynMutex_;
    std::map<std::string, std::pair<sockaddr_in, std::chrono::steady_clock::time_point>> dynGcs_;

    // 遥测转发目标(QGC / Mission Planner 等地面站)
    mutable std::mutex fwdMutex_;
    std::vector<sockaddr_in> fwdAddrs_;
    std::vector<std::string> fwdTargets_; // 原始字符串(回显/持久化)
    std::string fwdPersistPath_ = "config/forward_targets.json";

    // 飞控参数:首个 HEARTBEAT 后自动 PARAM_REQUEST_READ 读取 WP_LOITER_RAD /
    // AIRSPEED_CRUISE / ALT_HOLD_RTL(前端参数面板回填);PARAM_VALUE → JSON t:"param"
    // 未收齐的参数 5s 后自动重试(最多 3 轮),应对启动时飞控参数流控丢包
    bool paramRequested_ = false;
    void requestStartupParams();
    void requestParams(const std::vector<std::string> &ids); // 发送指定参数的读取请求
    std::set<std::string> paramRecv_;                        // 已收到的参数 id
    int paramRetry_ = 0;                                     // 重试轮次
    std::chrono::steady_clock::time_point paramReqAt_;       // 上次请求时间
    bool paramAllRecv_ = false;                              // 启动参数已收齐(停止重试)

    // 飞控地址跟踪:HEARTBEAT(autopilot 有效)源地址变化即更新 targetAddr_
    // (飞控重启后 ephemeral 端口变化,命令仍可送达);遥测超时 10s 自动重置学习
    std::chrono::steady_clock::time_point lastFcRx_{};
    void resetFcAddress();

    // viewer 链路丢失看门狗:超过 5s 无 viewer 活动则停车(停发心跳/RC_OVERRIDE,
    // 切换沿发一次 16 通道中立 override);viewerTracked_=false(从未通知)视为始终有活动
    std::atomic<bool> viewerTracked_{false};
    std::atomic<int64_t> lastViewerActiveMs_{0}; // steady_clock 毫秒(0=未记录)
    std::atomic<bool> viewerTimeoutActive_{false};

    // 消息 ID 计数(随 5s 统计输出,诊断 OSD 缺数据用)
    std::map<uint32_t, uint64_t> msgCounts_;

    // MAVLink 解析状态(跨包持久;reconfigure 时清零以应对分片跨链路错位)
    mavlink_message_t fcMsg_{};
    mavlink_status_t fcStatus_{};
    uint64_t statPkts_ = 0, statMsgs_ = 0; // 5s 统计计数(run 与 handleFcData 共用)

    std::thread thread_;
    std::atomic<bool> stopFlag_{false};
    std::mutex reconfigureMutex_; // 串行化 reconfigure(数据通道线程调,避免并发热切换)
};

} // namespace rcfpv
