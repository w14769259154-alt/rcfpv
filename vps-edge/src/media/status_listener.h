/**
 * 5gipc-rc VPS edge — 摄像头板载状态监听(设备 5gipc-status 脚本 TCP 上报)
 *
 * 设备端(OpenIPC)每 3s 用 busybox nc 发一行:
 *   STATUS load=1.23 mem=27 up=2091 disk=18 rx=123 tx=45
 * 本类 TCP 监听 statusListenPort,解析 k=v 缓存到 DeviceStatus,
 * MavlinkBridge 组装 OSD 板载状态时优先用设备值(新鲜≤6s),回退 VPS 自身。
 * 安全:只接受 WG 网段(10.55.0.0/24)来源,防公网伪造。
 */
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace rcfpv {

struct DeviceStatus {
    std::chrono::steady_clock::time_point updated{}; // 最近接收时刻
    float temp = 0;      // CPU 温度(°C,无节点时为 0)
    float load = 0;      // 1 分钟平均负载
    int memPct = -1;     // 内存使用率(%)
    int diskPct = -1;    // SD 卡使用率(%)
    uint32_t uptimeS = 0; // 运行时间(秒)
    float rxKbps = 0;    // 下行网速(eth0,KB/s)
    float txKbps = 0;    // 上行网速(eth0,KB/s)
    bool fresh = false;  // 是否有过接收(区别于"刚启动"与"从未上报")
};

class StatusListener {
public:
    explicit StatusListener(int listenPort);
    ~StatusListener();

    StatusListener(const StatusListener &) = delete;
    StatusListener &operator=(const StatusListener &) = delete;

    bool start();
    void stop();

    // 最近设备状态副本(线程安全)
    DeviceStatus latest() const;

private:
    void run();

    int listenPort_ = 7215;
    int fd_ = -1;
    std::atomic<bool> stopFlag_{false};
    std::thread thread_;
    mutable std::mutex mtx_;
    DeviceStatus status_;
};

} // namespace rcfpv
