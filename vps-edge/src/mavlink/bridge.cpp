#include "mavlink/bridge.h"

#include <ardupilotmega/mavlink.h> // vendor 官方库(src/mavlink/vendor)
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace rcfpv {

using nlohmann::json;

namespace {
constexpr int kRecvTimeoutMs = 500;
constexpr size_t kStatustextLen = 50;
// 启动自动读取的飞控参数(requestStartupParams 与 5s 重试逻辑共用)
const char *const kStartupParams[] = {"WP_LOITER_RAD", "AIRSPEED_CRUISE", "ALT_HOLD_RTL"};

// ---- 串口辅助(termios 8N1 raw) ----

// 配置串口:8N1、无流控、raw 输入输出、VMIN=0/VTIME=1(非阻塞,poll 驱动就绪)
bool configureSerial(int fd, int baudrate) {
    termios tty{};
    if (tcgetattr(fd, &tty) != 0) return false;

    speed_t sp = B115200;
    switch (baudrate) {
        case 9600: sp = B9600; break;
        case 19200: sp = B19200; break;
        case 38400: sp = B38400; break;
        case 57600: sp = B57600; break;
        case 115200: sp = B115200; break;
        case 230400: sp = B230400; break;
        case 460800: sp = B460800; break;
        case 921600: sp = B921600; break;
        default: sp = B115200; break;
    }
    cfsetispeed(&tty, sp);
    cfsetospeed(&tty, sp);

    // c_cflag:8N1 | CREAD | CLOCAL | 关 CRTSCTS
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    tty.c_cflag |= CS8 | CREAD | CLOCAL;

    // c_iflag:raw 输入(关软件流控/特殊转换)
    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);

    // c_oflag:raw 输出
    tty.c_oflag &= ~(OPOST | ONLCR);

    // c_lflag:raw(关 canonical/echo/signal)
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHONL | ISIG);

    // VMIN=0/VTIME=1:read 立即返回已有数据(poll 仍负责就绪通知)
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) return false;
    tcflush(fd, TCIOFLUSH); // 丢弃陈旧数据
    return true;
}

// 自动探测串口设备:扫描 /dev/ttyACM* 后 /dev/ttyUSB*,返回第一个可 open 的路径
// (ArduPilot/PX4 原生 USB CDC 通常枚举为 ttyACM0;FTDI/CP210x 走 ttyUSB0)
std::string detectSerialDevice() {
    for (const char *pat : {"/dev/ttyACM*", "/dev/ttyUSB*"}) {
        glob_t g;
        if (glob(pat, 0, nullptr, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; ++i) {
                int fd = open(g.gl_pathv[i], O_RDWR | O_NOCTTY | O_NONBLOCK);
                if (fd >= 0) {
                    close(fd);
                    std::string path = g.gl_pathv[i];
                    globfree(&g);
                    return path;
                }
            }
            globfree(&g);
        }
    }
    return "";
}

// 打开串口:device 传引用,空时自动探测并回填实际路径(避免二次探测失真);
// 配置 termios;返回 fd(失败 -1)
int openSerial(std::string &device, int baudrate, bool &autoDetected) {
    autoDetected = false;
    if (device.empty()) {
        device = detectSerialDevice();
        if (device.empty()) return -1;
        autoDetected = true;
    }
    int fd = open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    // 清 O_NONBLOCK:write 可短写循环补齐(read 由 poll 保证就绪)
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
    if (!configureSerial(fd, baudrate)) {
        close(fd);
        return -1;
    }
    return fd;
}

// 串口 write 短写循环(EINTR 重试);返回实际写入字节数
ssize_t writeAll(int fd, const uint8_t *buf, size_t len) {
    size_t w = 0;
    while (w < len) {
        ssize_t n = write(fd, buf + w, len - w);
        if (n <= 0) {
            if (errno == EINTR) continue;
            break;
        }
        w += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(w);
}
} // namespace

MavlinkBridge::MavlinkBridge(const MavlinkConfig &cfg) : cfg_(cfg) {}

MavlinkBridge::~MavlinkBridge() { stop(); }

void MavlinkBridge::setTelemetryCallback(TelemetryCallback cb) { telemetryCb_ = std::move(cb); }

bool MavlinkBridge::start() {
    if (thread_.joinable()) return true;

    // 初始化 transport 运行态(从配置;connection="usb" 内部走 Serial)
    transport_ = (cfg_.connection == "serial" || cfg_.connection == "usb")
                     ? Transport::Serial : Transport::Udp;
    serialDevice_ = cfg_.serialDevice;
    serialBaudrate_ = cfg_.serialBaudrate;

    // GCS 直连监听(14550,QGC 惯例):与 transport 无关,始终 UDP;失败仅告警不影响主链路
    if (cfg_.gcsListenPort > 0) {
        gcsFd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        sockaddr_in gcsLocal{};
        gcsLocal.sin_family = AF_INET;
        gcsLocal.sin_addr.s_addr = htonl(INADDR_ANY);
        gcsLocal.sin_port = htons(static_cast<uint16_t>(cfg_.gcsListenPort));
        int reuse = 1;
        setsockopt(gcsFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (bind(gcsFd_, reinterpret_cast<sockaddr *>(&gcsLocal), sizeof(gcsLocal)) < 0) {
            std::cerr << "[mavlink] GCS 直连端口 " << cfg_.gcsListenPort
                      << " 绑定失败: " << strerror(errno) << std::endl;
            close(gcsFd_);
            gcsFd_ = -1;
        } else {
            std::cout << "[mavlink] GCS 直连监听: UDP:" << cfg_.gcsListenPort
                      << "(QGC 直连此端口即可)" << std::endl;
        }
    }

    // 打开 FC transport(失败不致命:GCS 直连仍可用,run 线程跑空 FC)
    openFcTransport();

    stopFlag_ = false;
    thread_ = std::thread(&MavlinkBridge::run, this);

    // 转发目标:配置文件初始值,forward_targets.json(Web 端修改)优先
    setForwardTargets(cfg_.forwardTargets, false);
    loadForwardTargets();

    std::cout << "[mavlink] 已启动: " << (transport_ == Transport::Serial ? "USB" : "UDP")
              << ",转发目标 " << forwardTargets().size() << " 个" << std::endl;
    return true;
}

// 按 transport_ 打开 FC fd(UDP socket 或 serial fd);失败返回 false,fd_ 置 -1
// 持 sendMutex_:串口错误重开(run 线程)期间,数据通道线程可能并发 send 读 fd_,
// fd_/serialDevice_/targetAddr_ 写入须与发送临界区互斥
bool MavlinkBridge::openFcTransport() {
    std::lock_guard<std::mutex> lk(sendMutex_);
    if (transport_ == Transport::Serial) {
        bool ad = false;
        // openSerial 自动探测成功时会把实际路径回填进 serialDevice_(无需二次探测)
        fd_ = openSerial(serialDevice_, serialBaudrate_, ad);
        serialAutoDetected_ = ad;
        if (fd_ < 0) {
            std::cerr << "[mavlink] 串口打开失败: "
                      << (serialDevice_.empty() ? "<自动探测无设备>" : serialDevice_)
                      << " @ " << serialBaudrate_ << " (" << strerror(errno) << ")" << std::endl;
            return false;
        }
        std::cout << "[mavlink] 串口已打开: " << serialDevice_
                  << (ad ? " (自动探测)" : "") << " @ " << serialBaudrate_ << std::endl;
        return true;
    }

    // UDP 分支
    // SOCK_CLOEXEC:fork+exec gst-launch 等子进程时不继承此 fd(否则子进程持有的 fd 副本
    // 会使 closeFcTransport 后端口仍被占用,reconfigure 重新 bind 失败 "Address already in use")
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        std::cerr << "[mavlink] socket 创建失败: " << strerror(errno) << std::endl;
        return false;
    }
    // SO_REUSEADDR:reconfigure 关旧 fd 后立即重新 bind,避免内核短暂持有端口导致
    // "Address already in use"(与上方 gcsFd_ 一致;UDP 无 TIME_WAIT 但 close 后仍可能残留)
    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(static_cast<uint16_t>(cfg_.fcListenPort));
    if (bind(fd_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) {
        std::cerr << "[mavlink] UDP 绑定端口 " << cfg_.fcListenPort
                  << " 失败: " << strerror(errno) << std::endl;
        close(fd_);
        fd_ = -1;
        return false;
    }
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = kRecvTimeoutMs * 1000;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 初始目标(源地址学习前)
    targetAddr_.sin_family = AF_INET;
    targetAddr_.sin_port = htons(static_cast<uint16_t>(cfg_.fcTargetPort));
    inet_pton(AF_INET, cfg_.fcTargetIp.c_str(), &targetAddr_.sin_addr);
    addrLearned_ = false;

    std::cout << "[mavlink] UDP 监听: UDP:" << cfg_.fcListenPort << ",初始目标 "
              << cfg_.fcTargetIp << ":" << cfg_.fcTargetPort << std::endl;
    return true;
}

// 仅关 FC fd_;gcsFd_ 生命周期与 bridge 绑定(由 stop/reconfigure 管理_fc 关闭)
// 持 sendMutex_:close+置-1 与并发 send 不交错(防止拿到已关 fd 发送)
void MavlinkBridge::closeFcTransport() {
    std::lock_guard<std::mutex> lk(sendMutex_);
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void MavlinkBridge::stop() {
    // lifecycleMutex_:与 reconfigure 串行化线程 join/重启,防并发 double-join UB
    std::lock_guard<std::mutex> lk(lifecycleMutex_);
    stopFlag_ = true;
    if (thread_.joinable()) thread_.join();
    closeFcTransport();
    if (gcsFd_ >= 0) {
        close(gcsFd_);
        gcsFd_ = -1;
    }
}

void MavlinkBridge::run() {
    auto lastHeartbeat = std::chrono::steady_clock::now() - std::chrono::hours(1); // 立即发一次
    const auto hbInterval = std::chrono::duration<double>(1.0 / cfg_.heartbeatRateHz);
    auto lastStat = std::chrono::steady_clock::now();

    while (!stopFlag_) {
        auto now = std::chrono::steady_clock::now();

        // ---- viewer 活动看门狗:5s 无活动 → 停车(停发心跳/RC_OVERRIDE,切换沿发一次中立值) ----
        // viewerTracked_=false(调用方从未通知)视为始终有活动,行为与旧版一致
        bool viewerActive = true;
        if (viewerTracked_) {
            int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now.time_since_epoch()).count();
            int64_t lastMs = lastViewerActiveMs_.load();
            viewerActive = (lastMs > 0) && (nowMs - lastMs <= 5000);
        }
        if (!viewerActive && !viewerTimeoutActive_) {
            viewerTimeoutActive_ = true; // 状态切换沿:只发一次中立 override,不重复发
            std::cerr << "[mavlink] viewer 超时(>5s 无活动),停车:停发心跳/RC_OVERRIDE,"
                         "发送一次 16 通道中立摇杆值" << std::endl;
            sendNeutralOverride();
        } else if (viewerActive && viewerTimeoutActive_) {
            viewerTimeoutActive_ = false;
            std::cout << "[mavlink] viewer 恢复活动,恢复心跳/RC_OVERRIDE 发送" << std::endl;
        }

        // 心跳(viewer 停车期间跳过)
        if (!viewerTimeoutActive_ && now - lastHeartbeat >= hbInterval) {
            sendHeartbeat();
            lastHeartbeat = now;
        }

        // ---- 遥测超时看门狗:10s 无飞控数据则重置地址学习(地址可重新学习) ----
        if (addrLearned_ && now - lastFcRx_ > std::chrono::seconds(10))
            resetFcAddress();

        // ---- 串口错误重开:fd_ 无效且为串口模式时,每 2s 重试打开 ----
        if (fd_ < 0 && transport_ == Transport::Serial && now >= reopenAtMs_) {
            reopenAtMs_ = now + std::chrono::seconds(2);
            openFcTransport(); // 失败仅打日志,fd_ 保持 -1 等下轮
        }

        // 动态构建 pollfd:fd_ 可能 -1(reconfigure 回退失败 / 串口无设备),负 fd 入 poll 会 EINVAL
        pollfd fds[2];
        int fcIdx = -1, gcsIdx = -1;
        nfds_t nfds = 0;
        if (fd_ >= 0) { fds[nfds] = {fd_, POLLIN, 0}; fcIdx = nfds++; }
        if (gcsFd_ >= 0) { fds[nfds] = {gcsFd_, POLLIN, 0}; gcsIdx = nfds++; }

        int pr = 0;
        if (nfds > 0) {
            pr = poll(fds, nfds, 500);
        } else {
            // fd_ 与 gcsFd_ 均无效:休眠防 100% CPU 空转
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // fd 错误位检查(串口拔出 POLLHUP 等):立即关闭;串口安排 2s 后重开
        // (不处理会忙轮询 100% CPU 且链路永久死亡)
        if (pr > 0) {
            if (gcsIdx >= 0 && (fds[gcsIdx].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                std::cerr << "[mavlink] GCS fd 异常(revents=0x" << std::hex
                          << fds[gcsIdx].revents << std::dec << "),关闭" << std::endl;
                close(gcsFd_);
                gcsFd_ = -1;
                gcsIdx = -1; // 本轮不再处理该 fd
            }
            if (fcIdx >= 0 && (fds[fcIdx].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                std::cerr << "[mavlink] FC fd 异常(revents=0x" << std::hex
                          << fds[fcIdx].revents << std::dec << "),关闭"
                          << (transport_ == Transport::Serial ? ",2s 后重开" : "")
                          << std::endl;
                closeFcTransport();
                reopenAtMs_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                fcIdx = -1; // 本轮不再处理该 fd
            }
        }

        // GCS → 飞控(14550 收包透传,与 transport 无关)
        if (pr > 0 && gcsIdx >= 0 && (fds[gcsIdx].revents & POLLIN))
            handleGcsPacket();

        // 飞控 → 本地(Serial 用 read 无源地址;UDP 用 recvfrom 带源地址)
        if (pr > 0 && fcIdx >= 0 && (fds[fcIdx].revents & POLLIN)) {
            uint8_t buf[4096];
            ssize_t n = 0;
            sockaddr_in src{};
            socklen_t srcLen = sizeof(src);
            if (transport_ == Transport::Serial) {
                n = read(fd_, buf, sizeof(buf));
            } else {
                n = recvfrom(fd_, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr *>(&src), &srcLen);
            }
            if (n > 0) {
                ++statPkts_;
                handleFcData(buf, static_cast<size_t>(n),
                             transport_ == Transport::Serial ? nullptr : &src);
            }
        }

        // ---- 启动参数 5s 未收齐自动重试(最多 3 轮,应对飞控启动期参数流控丢包) ----
        if (paramRequested_ && !paramAllRecv_ && paramRetry_ < 3 &&
            now - paramReqAt_ > std::chrono::seconds(5)) {
            std::vector<std::string> missing;
            for (const char *pid : kStartupParams)
                if (paramRecv_.find(pid) == paramRecv_.end()) missing.push_back(pid);
            if (missing.empty()) {
                paramAllRecv_ = true;
            } else {
                ++paramRetry_;
                requestParams(missing); // 内部刷新 paramReqAt_
                std::cout << "[mavlink] 启动参数未收齐,重试 " << paramRetry_ << "/3:";
                for (const auto &m : missing) std::cout << " " << m;
                std::cout << std::endl;
            }
        }

        // 5s 统计:包数 / 解析消息数 + 各消息 ID 计数(诊断 OSD 缺数据)
        auto nowStat = std::chrono::steady_clock::now();
        if (nowStat - lastStat >= std::chrono::seconds(5)) {
            std::string ids;
            for (const auto &kv : msgCounts_) {
                ids += " " + std::to_string(kv.first) + ":" + std::to_string(kv.second);
            }
            std::cout << "[mavlink] 统计: " << statPkts_ << " 包 / " << statMsgs_
                      << " 条解析 | id:count {" << ids << " }" << std::endl;
            statPkts_ = statMsgs_ = 0;
            msgCounts_.clear();
            lastStat = nowStat;
        }
    }
}

// 飞控数据统一处理:
//   UDP(src 非空):源地址学习 + GCS 来源判定(非飞控来源透传给飞控,不本地解析)
//   Serial(src=nullptr):直接当飞控数据解析+转发(串口无地址概念)
void MavlinkBridge::handleFcData(const uint8_t *buf, size_t len, const sockaddr_in *src) {
    // UDP 模式:判断来源,非飞控来源(如 QGC 回包到 5760)视为 GCS 命令透传给飞控
    if (src != nullptr) {
        bool fromFc = false;
        {
            std::lock_guard<std::mutex> lk(sendMutex_);
            fromFc = addrLearned_ &&
                     (targetAddr_.sin_addr.s_addr == src->sin_addr.s_addr &&
                      targetAddr_.sin_port == src->sin_port);
        }
        // 地址未学习/源不匹配:仅当该包能解析出完整消息、且为 HEARTBEAT、且
        // autopilot 非 MAV_AUTOPILOT_INVALID 时才学习/更新地址(QGC 心跳 autopilot=
        // INVALID、扫描包解析不出消息,均不会毒化地址;旧版首包无条件学习会锁死错误地址)
        if (!fromFc && probeFcHeartbeat(buf, len)) {
            std::lock_guard<std::mutex> lk(sendMutex_);
            targetAddr_ = *src;
            addrLearned_ = true;
            fromFc = true;
            std::cout << "[mavlink] 源地址学习: 飞控 = "
                      << inet_ntoa(src->sin_addr) << ":" << ntohs(src->sin_port)
                      << std::endl;
        }

        if (isForwardSource(*src) || !fromFc) {
            // 地面站(GCS)→ 飞控:命令透传(不本地解析;未学习时无目标地址,只能丢弃)
            std::lock_guard<std::mutex> lk(sendMutex_);
            if (fd_ >= 0 && addrLearned_)
                sendto(fd_, buf, len, 0,
                       reinterpret_cast<const sockaddr *>(&targetAddr_),
                       sizeof(targetAddr_));
            return;
        }
    }

    lastFcRx_ = std::chrono::steady_clock::now();

    // 逐字节解析(成员 fcMsg_/fcStatus_ 跨包持久;reconfigure 清零应对分片跨链路错位)
    for (size_t i = 0; i < len; ++i) {
        if (mavlink_parse_char(MAVLINK_COMM_0, buf[i], &fcMsg_, &fcStatus_)) {
            ++statMsgs_;
            ++msgCounts_[fcMsg_.msgid];
            // 飞控心跳:地址跟踪(飞控重启换端口后命令仍可达)+ 首个心跳后自动读参数
            if (fcMsg_.msgid == MAVLINK_MSG_ID_HEARTBEAT) {
                mavlink_heartbeat_t hb;
                memset(&hb, 0, sizeof(hb));
                memcpy(&hb, fcMsg_.payload64,
                       fcMsg_.len < sizeof(hb) ? fcMsg_.len : sizeof(hb));
                if (hb.autopilot != MAV_AUTOPILOT_INVALID && src != nullptr) { // 飞控方向(UDP)
                    std::lock_guard<std::mutex> lk(sendMutex_);
                    if (targetAddr_.sin_addr.s_addr != src->sin_addr.s_addr ||
                        targetAddr_.sin_port != src->sin_port) {
                        targetAddr_ = *src;
                        std::cout << "[mavlink] 飞控地址更新: "
                                  << inet_ntoa(src->sin_addr) << ":"
                                  << ntohs(src->sin_port) << std::endl;
                    }
                }
                if (!paramRequested_) {
                    paramRequested_ = true;
                    paramRecv_.clear();
                    paramRetry_ = 0;
                    requestStartupParams();
                }
            }
            handleTelemetry(&fcMsg_);
        }
    }

    // 遥测副本 → 所有静态转发目标(QGC / MP)
    // UDP 模式经 fd_(5760);Serial 模式 fd_ 是串口不能 sendto,改经 gcsFd_(14550) 发
    {
        std::lock_guard<std::mutex> lk(fwdMutex_);
        if (!fwdAddrs_.empty()) {
            int sendFd = (transport_ == Transport::Serial) ? gcsFd_ : fd_;
            if (sendFd >= 0) {
                for (const auto &a : fwdAddrs_)
                    sendto(sendFd, buf, len, 0,
                           reinterpret_cast<const sockaddr *>(&a), sizeof(a));
            }
        }
    }

    // 遥测副本 → 动态 GCS(直连 14550 的地面站,15s 活跃窗口;两种模式都经 gcsFd_)
    {
        std::lock_guard<std::mutex> lk(dynMutex_);
        auto nowG = std::chrono::steady_clock::now();
        for (auto it = dynGcs_.begin(); it != dynGcs_.end();) {
            if (nowG - it->second.second > std::chrono::seconds(15)) {
                it = dynGcs_.erase(it);
                continue;
            }
            if (gcsFd_ >= 0)
                sendto(gcsFd_, buf, len, 0,
                       reinterpret_cast<const sockaddr *>(&it->second.first),
                       sizeof(sockaddr_in));
            ++it;
        }
    }
}

// 探测 UDP 包是否含有效飞控 HEARTBEAT(autopilot 非 INVALID 且非 GCS 心跳),地址学习判据:
// 用独立解析通道 COMM_2 逐字节试探(每次先复位残留状态),不污染飞控方向解析状态
bool MavlinkBridge::probeFcHeartbeat(const uint8_t *buf, size_t len) const {
    mavlink_reset_channel_status(MAVLINK_COMM_2);
    mavlink_message_t msg{};
    mavlink_status_t st{};
    for (size_t i = 0; i < len; ++i) {
        if (mavlink_parse_char(MAVLINK_COMM_2, buf[i], &msg, &st)) {
            if (msg.msgid != MAVLINK_MSG_ID_HEARTBEAT) continue; // 继续找包内其他消息
            mavlink_heartbeat_t hb;
            memset(&hb, 0, sizeof(hb));
            memcpy(&hb, msg.payload64,
                   msg.len < sizeof(hb) ? msg.len : sizeof(hb));
            return hb.autopilot != MAV_AUTOPILOT_INVALID && hb.type != MAV_TYPE_GCS;
        }
    }
    return false;
}

// 遥测超时重置地址学习:addrLearned_ 置 false、targetAddr_ 恢复配置初始目标
// (心跳回退到 fcTargetIp:fcTargetPort 触发飞控响应,地址可重新学习)
void MavlinkBridge::resetFcAddress() {
    std::lock_guard<std::mutex> lk(sendMutex_);
    if (!addrLearned_) return;
    addrLearned_ = false;
    targetAddr_ = {};
    targetAddr_.sin_family = AF_INET;
    targetAddr_.sin_port = htons(static_cast<uint16_t>(cfg_.fcTargetPort));
    inet_pton(AF_INET, cfg_.fcTargetIp.c_str(), &targetAddr_.sin_addr);
    std::cout << "[mavlink] 飞控遥测超时,重置源地址学习(恢复初始目标 "
              << cfg_.fcTargetIp << ":" << cfg_.fcTargetPort << ",等待重新学习)"
              << std::endl;
}

void MavlinkBridge::handleGcsPacket() {
    uint8_t buf[4096];
    sockaddr_in src{};
    socklen_t srcLen = sizeof(src);
    ssize_t n = recvfrom(gcsFd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&src), &srcLen);
    if (n <= 0) return;

    // 登记动态 GCS(键 ip:port;15s 无发包自动老化)
    {
        char key[32];
        snprintf(key, sizeof(key), "%s:%u", inet_ntoa(src.sin_addr), ntohs(src.sin_port));
        std::lock_guard<std::mutex> lk(dynMutex_);
        auto it = dynGcs_.find(key);
        if (it == dynGcs_.end()) {
            std::cout << "[mavlink] GCS 接入: " << key << std::endl;
            dynGcs_[key] = {src, std::chrono::steady_clock::now()};
        } else {
            it->second.second = std::chrono::steady_clock::now();
        }
    }

    // → 飞控透传(命令/心跳原样转发):Serial 用 write,UDP 用 sendto(需 addrLearned_)
    std::lock_guard<std::mutex> lk(sendMutex_);
    if (fd_ < 0) return;
    if (transport_ == Transport::Serial) {
        writeAll(fd_, buf, static_cast<size_t>(n));
    } else if (addrLearned_) {
        sendto(fd_, buf, static_cast<size_t>(n), 0,
               reinterpret_cast<const sockaddr *>(&targetAddr_), sizeof(targetAddr_));
    }
}

void MavlinkBridge::sendHeartbeat() {
    // pack+发送 同临界区(sendPacked):TX seq 与实际发送顺序一致
    sendPacked([&](mavlink_message_t *m) {
        mavlink_msg_heartbeat_pack(static_cast<uint8_t>(cfg_.systemId),
                                   static_cast<uint8_t>(cfg_.componentId), m, MAV_TYPE_GCS,
                                   MAV_AUTOPILOT_INVALID, 0, 0, MAV_STATE_ACTIVE);
    });
}

// viewer 丢失停车切换沿:发一次 16 通道全 1500 中立 RC_CHANNELS_OVERRIDE(不重复发)
void MavlinkBridge::sendNeutralOverride() {
    sendPacked([&](mavlink_message_t *m) {
        mavlink_msg_rc_channels_override_pack(
            static_cast<uint8_t>(cfg_.systemId),
            static_cast<uint8_t>(cfg_.componentId), m,
            1 /*targetSys*/, 1 /*targetComp*/,
            1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500,
            1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500,
            0, 0);
    });
}

// viewer 活动通知(链路丢失看门狗):先记时间再启用跟踪
// (若先置 tracked,run 线程可能观察到 tracked=true 而 ms=0,瞬间误判超时停车)
void MavlinkBridge::noteViewerActivity() {
    lastViewerActiveMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count();
    viewerTracked_ = true;
}

// 首个飞控心跳后自动读取参数(前端参数面板回填当前值)
void MavlinkBridge::requestStartupParams() {
    const size_t n = sizeof(kStartupParams) / sizeof(kStartupParams[0]);
    requestParams(std::vector<std::string>(kStartupParams, kStartupParams + n));
    std::cout << "[mavlink] 已请求飞控参数: WP_LOITER_RAD / AIRSPEED_CRUISE / ALT_HOLD_RTL"
              << std::endl;
}

// 发送指定参数的 PARAM_REQUEST_READ,并刷新 paramReqAt_(5s 重试的计时基准)
void MavlinkBridge::requestParams(const std::vector<std::string> &ids) {
    const uint8_t sys = static_cast<uint8_t>(cfg_.systemId);
    const uint8_t comp = static_cast<uint8_t>(cfg_.componentId);
    for (const auto &pid : ids) {
        sendPacked([&](mavlink_message_t *m) {
            char id[MAVLINK_MSG_PARAM_REQUEST_READ_FIELD_PARAM_ID_LEN + 1] = {};
            strncpy(id, pid.c_str(), MAVLINK_MSG_PARAM_REQUEST_READ_FIELD_PARAM_ID_LEN);
            mavlink_msg_param_request_read_pack(sys, comp, m, 1 /*target*/, 1, id, -1);
        });
    }
    paramReqAt_ = std::chrono::steady_clock::now();
}

// 已持 sendMutex_ 的发送(仅 sendBuffer/sendPacked 内部调用)
void MavlinkBridge::sendLocked(const uint8_t *buf, size_t len) {
    if (fd_ < 0) return;
    if (transport_ == Transport::Serial) {
        // 串口:write 短写循环补齐(serial 无地址概念,立即发送,不需源地址学习)
        writeAll(fd_, buf, len);
    } else {
        // UDP:始终向 targetAddr_ 发(学习前=配置的 fcTargetIp:fcTargetPort;学习后=飞控实际源地址)
        // 不要求 addrLearned_:即使刚启动/reconfigure 回退后,也向配置目标发心跳触发飞控响应,
        // 避免飞控不主动发包时 edge 永远学不到源地址的死锁
        sendto(fd_, buf, len, 0, reinterpret_cast<const sockaddr *>(&targetAddr_),
               sizeof(targetAddr_));
    }
}

void MavlinkBridge::sendBuffer(const uint8_t *buf, size_t len) {
    std::lock_guard<std::mutex> lk(sendMutex_);
    sendLocked(buf, len);
}

// 持 sendMutex_ 完成 pack+发送:mavlink pack 会推进通道 TX seq,
// pack 与发送必须在同一临界区,否则多线程并发 pack/send 导致 seq 乱序与数据竞态
void MavlinkBridge::sendPacked(const std::function<void(mavlink_message_t *)> &packer) {
    std::lock_guard<std::mutex> lk(sendMutex_);
    if (fd_ < 0) return;
    mavlink_message_t msg;
    packer(&msg);
    uint8_t buf[MAVLINK_MAX_PACKET_LEN];
    uint16_t len = mavlink_msg_to_send_buffer(buf, &msg);
    sendLocked(buf, len);
}

void MavlinkBridge::sendRawFrame(const uint8_t *data, size_t size) { sendBuffer(data, size); }

// ---- 多目标转发 ----

namespace {
// "ip:port" → sockaddr_in;非法返回 false
bool parseAddr(const std::string &s, sockaddr_in *out) {
    auto colon = s.rfind(':');
    if (colon == std::string::npos) return false;
    std::string ip = s.substr(0, colon);
    int port = atoi(s.substr(colon + 1).c_str());
    if (port <= 0 || port > 65535) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    return inet_pton(AF_INET, ip.c_str(), &a.sin_addr) == 1 && ((*out = a), true);
}
} // namespace

void MavlinkBridge::setForwardTargets(const std::vector<std::string> &targets, bool persist) {
    std::vector<sockaddr_in> addrs;
    std::vector<std::string> valid;
    for (const auto &t : targets) {
        sockaddr_in a{};
        if (parseAddr(t, &a)) {
            addrs.push_back(a);
            valid.push_back(t);
        } else {
            std::cerr << "[mavlink] 转发目标格式非法(应 ip:port): " << t << std::endl;
        }
    }

    std::lock_guard<std::mutex> lk(fwdMutex_);
    fwdAddrs_ = std::move(addrs);
    fwdTargets_ = std::move(valid);
    if (persist) persistForwardTargets();

    std::cout << "[mavlink] 转发目标更新: " << fwdTargets_.size() << " 个";
    for (const auto &t : fwdTargets_) std::cout << " " << t;
    std::cout << std::endl;
}

std::vector<std::string> MavlinkBridge::forwardTargets() const {
    std::lock_guard<std::mutex> lk(fwdMutex_);
    return fwdTargets_;
}

bool MavlinkBridge::isForwardSource(const sockaddr_in &addr) const {
    std::lock_guard<std::mutex> lk(fwdMutex_);
    for (const auto &a : fwdAddrs_) {
        if (a.sin_addr.s_addr == addr.sin_addr.s_addr && a.sin_port == addr.sin_port)
            return true;
    }
    return false;
}

void MavlinkBridge::loadForwardTargets() {
    std::ifstream ifs(fwdPersistPath_);
    if (!ifs.is_open()) return; // 无持久化文件,沿用配置值

    try {
        json j;
        ifs >> j;
        if (j.contains("targets") && j.at("targets").is_array()) {
            std::vector<std::string> targets;
            for (const auto &t : j.at("targets"))
                if (t.is_string()) targets.push_back(t.get<std::string>());
            setForwardTargets(targets, false);
            std::cout << "[mavlink] 已加载持久化转发目标: " << targets.size() << " 个"
                      << std::endl;
        }
    } catch (const json::exception &e) {
        std::cerr << "[mavlink] 转发目标文件解析失败: " << e.what() << std::endl;
    }
}

void MavlinkBridge::persistForwardTargets() const {
    // fwdMutex_ 已由调用方持有
    try {
        json j;
        j["targets"] = fwdTargets_;
        std::ofstream ofs(fwdPersistPath_);
        ofs << j.dump(2) << std::endl;
        if (!ofs.good()) {
            // 写入失败(磁盘满/目录不存在等)只告警,不影响运行
            std::cerr << "[mavlink] 转发目标持久化写入失败: " << fwdPersistPath_
                      << "(" << strerror(errno) << ")" << std::endl;
        }
    } catch (const std::exception &e) {
        std::cerr << "[mavlink] 转发目标保存失败: " << e.what() << std::endl;
    }
}

// ---- 遥测 → JSON(字段与前端 telemetry store 对齐) ----
// 使用官方 mavlink_msg_xxx_decode() 解码,自动处理 packed 对齐 + v2 扩展字段截断
// (旧代码用 memcpy + sizeof 检查,在此平台 packed 未生效导致 sizeof 偏大,多数消息被误丢)

// 板载状态采集(OSD 显示:CPU 温度/负载/内存/网速/运行时间)。2s 限频读一次 /proc,降低开销。
static void appendBoardStatus(json &j) {
    static std::chrono::steady_clock::time_point lastRead{};
    static float temp = 0, load = 0;
    static int mem = 0;
    static uint32_t uptime = 0;
    static float rxKbps = 0, txKbps = 0;
    static uint64_t lastRxBytes = 0, lastTxBytes = 0;
    static bool haveNetSample = false;
    auto now = std::chrono::steady_clock::now();
    const bool first = lastRead.time_since_epoch().count() == 0;
    if (first || std::chrono::duration_cast<std::chrono::seconds>(now - lastRead).count() >= 2) {
        auto dt = first ? 2.0f
            : std::chrono::duration<float>(now - lastRead).count();
        if (dt <= 0) dt = 2;
        lastRead = now;
        // CPU 温度(/sys/class/thermal/thermal_zone0/temp,单位 m°C)
        { std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
            if (f) { int mv = 0; f >> mv; temp = mv / 1000.0f; } }
        // 1 分钟平均负载(/proc/loadavg)
        { std::ifstream f("/proc/loadavg");
            if (f) { std::string a; f >> a; if (!a.empty()) load = std::atof(a.c_str()); } }
        // 内存使用率(%)
        { std::ifstream f("/proc/meminfo");
            if (f) { long total = 0, avail = 0; std::string k, u; long v;
                while (f >> k >> v >> u) {
                    if (k == "MemTotal:") total = v;
                    else if (k == "MemAvailable:") avail = v;
                }
                if (total > 0) mem = static_cast<int>(100.0 * (total - avail) / total); } }
        // 运行时间(秒)
        { std::ifstream f("/proc/uptime");
            if (f) { double s = 0; f >> s; uptime = static_cast<uint32_t>(s); } }
        // 网速(/proc/net/dev 两次采样差值;累加全部非 lo 接口)
        { std::ifstream f("/proc/net/dev");
            if (f) { uint64_t rx = 0, tx = 0; std::string line;
                std::getline(f, line); std::getline(f, line); // 表头两行
                while (std::getline(f, line)) {
                    auto colon = line.find(':');
                    if (colon == std::string::npos) continue;
                    std::istringstream iss(line.substr(0, colon));
                    std::string ifname; iss >> ifname;
                    if (ifname.empty() || ifname == "lo") continue;
                    std::istringstream rest(line.substr(colon + 1));
                    uint64_t rb = 0, tb = 0, skip = 0;
                    rest >> rb; // 接收 bytes
                    for (int i = 0; i < 7; i++) rest >> skip; // packets..multicast
                    rest >> tb; // 发送 bytes
                    rx += rb; tx += tb;
                }
                if (haveNetSample && rx >= lastRxBytes && tx >= lastTxBytes) {
                    rxKbps = static_cast<float>((rx - lastRxBytes) * 8) / 1000.0f / dt;
                    txKbps = static_cast<float>((tx - lastTxBytes) * 8) / 1000.0f / dt;
                }
                lastRxBytes = rx; lastTxBytes = tx; haveNetSample = true;
            } }
    }
    j["board_temp"] = temp;
    j["cpu_load"] = load;
    j["mem_pct"] = mem;
    j["uptime_s"] = uptime;
    j["rx_kbps"] = rxKbps;
    j["tx_kbps"] = txKbps;
}

void MavlinkBridge::handleTelemetry(const mavlink_message_t *msg) {
    if (!telemetryCb_) return;

    json j;
    j["t"] = "tel";
    j["id"] = msg->msgid;

    switch (msg->msgid) {
    case MAVLINK_MSG_ID_HEARTBEAT: {
        mavlink_heartbeat_t p;
        mavlink_msg_heartbeat_decode(msg, &p);
        // 过滤非飞行器心跳:飞控侧 mavlink-router 会把 GCS 心跳(type=GCS/
        // autopilot=INVALID)一并转发进来,若不过滤,前端机型识别会在
        // 真飞控(如 Plane,mav_type=1)与 GCS(mav_type=6,映射不到→回退
        // Rover 表)之间疯狂切换,模式显示抖动
        if (p.type == MAV_TYPE_GCS || p.autopilot == MAV_AUTOPILOT_INVALID)
            return;
        j["custom_mode"] = static_cast<uint32_t>(p.custom_mode);
        j["base_mode"] = static_cast<uint8_t>(p.base_mode);
        j["autopilot"] = static_cast<uint8_t>(p.autopilot);
        j["mav_type"] = static_cast<uint8_t>(p.type);   // 机型:前端据此自动切换对应固件的模式表
        j["system_status"] = static_cast<uint8_t>(p.system_status);
        static uint32_t lastMode = 0xFFFFFFFF;
        if (p.custom_mode != lastMode) {
            std::cout << "[mavlink] 飞行模式变化: custom_mode=" << p.custom_mode
                      << " base_mode=0x" << std::hex << static_cast<int>(p.base_mode)
                      << std::dec << std::endl;
            lastMode = p.custom_mode;
        }
        break;
    }
    case MAVLINK_MSG_ID_SYS_STATUS: {
        mavlink_sys_status_t p;
        mavlink_msg_sys_status_decode(msg, &p);
        j["voltage_battery"] = static_cast<uint16_t>(p.voltage_battery);     // mV
        j["current_battery"] = static_cast<int16_t>(p.current_battery);      // 10mA
        j["battery_remaining"] = static_cast<int8_t>(p.battery_remaining);   // %
        break;
    }
    case MAVLINK_MSG_ID_GPS_RAW_INT: {
        mavlink_gps_raw_int_t p;
        mavlink_msg_gps_raw_int_decode(msg, &p);
        j["lat"] = static_cast<int32_t>(p.lat); // 1e7 deg
        j["lon"] = static_cast<int32_t>(p.lon);
        j["alt"] = static_cast<int32_t>(p.alt); // mm
        j["satellites_visible"] = static_cast<uint8_t>(p.satellites_visible);
        j["fix_type"] = static_cast<uint8_t>(p.fix_type);
        break;
    }
    case MAVLINK_MSG_ID_ATTITUDE: {
        mavlink_attitude_t p;
        mavlink_msg_attitude_decode(msg, &p);
        j["roll"] = static_cast<float>(p.roll); // rad
        j["pitch"] = static_cast<float>(p.pitch);
        j["yaw"] = static_cast<float>(p.yaw);
        break;
    }
    case MAVLINK_MSG_ID_GLOBAL_POSITION_INT: {
        mavlink_global_position_int_t p;
        mavlink_msg_global_position_int_decode(msg, &p);
        j["lat"] = static_cast<int32_t>(p.lat);
        j["lon"] = static_cast<int32_t>(p.lon);
        j["relative_alt"] = static_cast<int32_t>(p.relative_alt); // mm
        j["alt"] = static_cast<int32_t>(p.alt);                   // mm MSL
        j["vx"] = static_cast<int16_t>(p.vx);                     // cm/s
        j["vy"] = static_cast<int16_t>(p.vy);
        j["vz"] = static_cast<int16_t>(p.vz);
        j["hdg"] = static_cast<uint16_t>(p.hdg); // 0.01deg
        break;
    }
    case MAVLINK_MSG_ID_RC_CHANNELS: {
        mavlink_rc_channels_t p;
        mavlink_msg_rc_channels_decode(msg, &p);
        j["rssi"] = static_cast<uint8_t>(p.rssi);
        json chans = json::array();
        chans.push_back(static_cast<uint16_t>(p.chan1_raw));
        chans.push_back(static_cast<uint16_t>(p.chan2_raw));
        chans.push_back(static_cast<uint16_t>(p.chan3_raw));
        chans.push_back(static_cast<uint16_t>(p.chan4_raw));
        chans.push_back(static_cast<uint16_t>(p.chan5_raw));
        chans.push_back(static_cast<uint16_t>(p.chan6_raw));
        chans.push_back(static_cast<uint16_t>(p.chan7_raw));
        chans.push_back(static_cast<uint16_t>(p.chan8_raw));
        chans.push_back(static_cast<uint16_t>(p.chan9_raw));
        chans.push_back(static_cast<uint16_t>(p.chan10_raw));
        chans.push_back(static_cast<uint16_t>(p.chan11_raw));
        chans.push_back(static_cast<uint16_t>(p.chan12_raw));
        chans.push_back(static_cast<uint16_t>(p.chan13_raw));
        chans.push_back(static_cast<uint16_t>(p.chan14_raw));
        chans.push_back(static_cast<uint16_t>(p.chan15_raw));
        chans.push_back(static_cast<uint16_t>(p.chan16_raw));
        j["channels"] = chans;
        break;
    }
    case MAVLINK_MSG_ID_VFR_HUD: {
        mavlink_vfr_hud_t p;
        mavlink_msg_vfr_hud_decode(msg, &p);
        j["airspeed"] = static_cast<float>(p.airspeed);     // m/s
        j["groundspeed"] = static_cast<float>(p.groundspeed);
        j["heading"] = static_cast<uint16_t>(p.heading);    // deg
        j["throttle"] = static_cast<uint16_t>(p.throttle);  // %
        // VFR alt 为米(AMSL);与 GPS/GPI 的 alt(mm)分字段,避免单位覆盖冲突
        j["alt_vfr"] = static_cast<float>(p.alt);           // m
        j["climb"] = static_cast<float>(p.climb);           // m/s
        break;
    }
    case MAVLINK_MSG_ID_PARAM_VALUE: {
        mavlink_param_value_t p;
        mavlink_msg_param_value_decode(msg, &p);
        char id[17];
        memcpy(id, p.param_id, 16);
        id[16] = '\0';
        paramRecv_.insert(id); // 记录已收参数(启动参数 5s 重试的"收齐"判断)
        json pj;
        pj["t"] = "param";
        pj["id"] = id;
        pj["value"] = static_cast<float>(p.param_value);
        telemetryCb_(pj.dump());
        return;
    }
    case MAVLINK_MSG_ID_HOME_POSITION: {
        mavlink_home_position_t p;
        mavlink_msg_home_position_decode(msg, &p);
        j["home_lat"] = static_cast<int32_t>(p.latitude);   // 1e7 deg
        j["home_lon"] = static_cast<int32_t>(p.longitude);
        j["home_alt"] = static_cast<int32_t>(p.altitude);   // mm MSL
        break;
    }
    case MAVLINK_MSG_ID_STATUSTEXT: {
        mavlink_statustext_t p;
        mavlink_msg_statustext_decode(msg, &p);
        j["severity"] = static_cast<uint8_t>(p.severity);
        char text[kStatustextLen + 1];
        memcpy(text, p.text, kStatustextLen);
        text[kStatustextLen] = '\0';
        j["text"] = text;
        // 打印所有 STATUSTEXT(Pre-Arm 检查、模式切换、警告等诊断信息)
        std::cout << "[mavlink] STATUSTEXT(" << static_cast<int>(p.severity)
                  << "): " << text << std::endl;
        break;
    }
    case MAVLINK_MSG_ID_POWER_STATUS: {
        // POWER_STATUS #125:Vcc=5V 毫伏, Vservo=舵机电压毫伏
        mavlink_power_status_t p;
        mavlink_msg_power_status_decode(msg, &p);
        j["vservo_mv"] = static_cast<uint16_t>(p.Vservo);
        j["vcc_mv"] = static_cast<uint16_t>(p.Vcc);
        break;
    }
    case MAVLINK_MSG_ID_BATTERY_STATUS: {
        // BATTERY_STATUS #147:id=电池实例, battery_function=用途(1=主,5=舵机)
        // voltages[10] 为各电芯电压 mV,-1=未知;总电压=Σ 有效电芯
        mavlink_battery_status_t p;
        mavlink_msg_battery_status_decode(msg, &p);
        j["bat_id"] = static_cast<uint8_t>(p.id);
        j["bat_function"] = static_cast<uint8_t>(p.battery_function);
        j["bat_remaining"] = static_cast<int8_t>(p.battery_remaining);
        // 求总电压(mV):跳过 -1(UINT16_MAX)
        uint32_t totalMv = 0;
        for (int i = 0; i < 10; ++i) {
            uint16_t cv = static_cast<uint16_t>(p.voltages[i]);
            if (cv != UINT16_MAX) totalMv += cv;
        }
        j["bat_voltage_mv"] = totalMv;
        // 首次遇到每个 (id, function) 组合时打印一次,便于诊断
        static uint32_t loggedBats = 0;
        uint32_t key = (static_cast<uint32_t>(p.id) << 8) | p.battery_function;
        if (!(loggedBats & (1u << (key & 31)))) {
            std::cout << "[mavlink] BATTERY_STATUS id=" << static_cast<int>(p.id)
                      << " function=" << static_cast<int>(p.battery_function)
                      << " voltage=" << (totalMv / 1000.0) << "V"
                      << " remaining=" << static_cast<int>(p.battery_remaining) << "%"
                      << std::endl;
            loggedBats |= (1u << (key & 31));
        }
        break;
    }
    case MAVLINK_MSG_ID_COMMAND_ACK: {
        mavlink_command_ack_t p;
        mavlink_msg_command_ack_decode(msg, &p);
        j["command"] = static_cast<uint16_t>(p.command);
        j["result"] = static_cast<uint8_t>(p.result);
        static const char *kResultStr[] = {"接受", "临时拒绝", "拒绝", "不支持", "进行中"};
        int ri = static_cast<int>(p.result);
        std::cout << "[mavlink] COMMAND_ACK: command=" << p.command
                  << " result=" << static_cast<int>(p.result)
                  << "(" << (ri >= 0 && ri <= 4 ? kResultStr[ri] : "未知") << ")"
                  << std::endl;
        break;
    }
    default:
        return; // 未订阅的消息不转发
    }

    appendBoardStatus(j); // 追加板载状态(OSD 显示)
    telemetryCb_(j.dump());
}

// ---- 上行命令(JSON → MAVLink 帧) ----
// 外壳:整体异常防护,畸形字段类型等异常不再击穿进程(旧版直接 terminate)
void MavlinkBridge::handleCommand(const std::string &jsonCmd) {
    try {
        handleCommandImpl(jsonCmd);
    } catch (const std::exception &e) {
        sendCmdError(std::string("命令处理异常: ") + e.what());
    } catch (...) {
        sendCmdError("命令处理发生未知异常");
    }
}

// 命令错误回复(t:"cmd_error"):字段缺失/类型非法/处理异常;尽力发送,失败静默
void MavlinkBridge::sendCmdError(const std::string &error) {
    std::cerr << "[mavlink] " << error << std::endl;
    if (telemetryCb_) {
        try {
            json reply;
            reply["t"] = "cmd_error";
            reply["error"] = error;
            telemetryCb_(reply.dump());
        } catch (...) {
            // 回复失败不影响主流程
        }
    }
}

void MavlinkBridge::handleCommandImpl(const std::string &jsonCmd) {
    json cmd = json::parse(jsonCmd, nullptr, false);
    if (cmd.is_discarded()) {
        std::cerr << "[mavlink] 命令 JSON 解析失败" << std::endl;
        return;
    }

    // 兼容 cmd / type 两种字段名,大小写不敏感(先校验 is_string,非字符串不取值防抛异常)
    std::string verb;
    for (const char *key : {"cmd", "type"}) {
        if (cmd.contains(key) && cmd.at(key).is_string()) {
            verb = cmd.at(key).get<std::string>();
            break;
        }
    }
    for (auto &c : verb) c = static_cast<char>(toupper(c));
    if (verb.empty()) return;

    const uint8_t sys = static_cast<uint8_t>(cfg_.systemId);
    const uint8_t comp = static_cast<uint8_t>(cfg_.componentId);
    const uint8_t targetSys = 1; // ArduPilot 飞控默认 system id = 1

    if (verb == "SET_MODE") {
        // 缺 mode/类型非法时报错(旧版缺省 0 有误切风险);经 double 取值+范围检查防溢出
        if (!cmd.contains("mode") || !cmd.at("mode").is_number()) {
            sendCmdError("SET_MODE 缺少/非法 mode 字段");
            return;
        }
        double modeD = cmd.at("mode").get<double>();
        if (modeD < 0.0 || modeD > 4294967295.0) {
            sendCmdError("SET_MODE mode 超范围(0-4294967295)");
            return;
        }
        uint32_t mode = static_cast<uint32_t>(modeD);
        // 方式1: SET_MODE 消息(#11)。ArduPilot 要求 base_mode 含
        // MAV_MODE_FLAG_CUSTOM_MODE_ENABLED(=1) 才会接受 custom_mode 切换,
        // 否则 GCS_Mavlink::handleMessage 直接忽略。
        sendPacked([&](mavlink_message_t *m) {
            mavlink_msg_set_mode_pack(sys, comp, m, targetSys,
                                      MAV_MODE_FLAG_CUSTOM_MODE_ENABLED, mode);
        });
        std::cout << "[mavlink] SET_MODE base=0x" << std::hex
                  << MAV_MODE_FLAG_CUSTOM_MODE_ENABLED << std::dec
                  << " custom_mode=" << mode << std::endl;
        // 方式2: COMMAND_LONG DO_SET_MODE(#176,QGC 方式)。
        // 修复:之前 pack 后未 to_send_buffer + sendBuffer,该帧从未真正发送。
        sendPacked([&](mavlink_message_t *m) {
            mavlink_msg_command_long_pack(sys, comp, m, targetSys, 1,
                                          MAV_CMD_DO_SET_MODE, 0,
                                          static_cast<float>(MAV_MODE_FLAG_CUSTOM_MODE_ENABLED),
                                          static_cast<float>(mode), 0, 0, 0, 0, 0);
        });
        std::cout << "[mavlink] DO_SET_MODE custom_mode=" << mode << std::endl;
        return;
    } else if (verb == "ARM" || verb == "DISARM") {
        float arm = (verb == "ARM") ? 1.0f : 0.0f;
        // param2=21196 强制解锁/上锁(绕过 Pre-Arm 检查),FPV 地面站场景与 QGC 强制行为一致
        sendPacked([&](mavlink_message_t *m) {
            mavlink_msg_command_long_pack(sys, comp, m, targetSys, 1,
                                          MAV_CMD_COMPONENT_ARM_DISARM, 0, arm, 21196.0f,
                                          0, 0, 0, 0, 0);
        });
        std::cout << "[mavlink] " << verb << " (强制)" << std::endl;
    } else if (verb == "SET_SPEED" || verb == "SET_ALT" || verb == "SET_RADIUS") {
        // 参数写入走 PARAM_SET(与启动自动读取闭环,重启飞控后仍生效):
        //   SET_SPEED  → AIRSPEED_CRUISE(巡航空速 m/s)
        //   SET_ALT    → ALT_HOLD_RTL(RTL 高度 m)
        //   SET_RADIUS → WP_LOITER_RAD(盘旋半径 m)
        const char *paramId = (verb == "SET_SPEED")    ? "AIRSPEED_CRUISE"
                              : (verb == "SET_ALT")    ? "ALT_HOLD_RTL"
                                                        : "WP_LOITER_RAD";
        const char *field = (verb == "SET_SPEED") ? "speed"
                              : (verb == "SET_ALT") ? "alt"
                                                    : "radius";
        // 字段缺失/非数字时报错,不再缺省 0.0 持久写入飞控参数(危险缺省值)
        if (!cmd.contains(field) || !cmd.at(field).is_number()) {
            sendCmdError(verb + " 缺少/非法 " + field + " 字段");
            return;
        }
        float v = cmd.at(field).get<float>();
        sendPacked([&](mavlink_message_t *m) {
            char pid[MAVLINK_MSG_PARAM_SET_FIELD_PARAM_ID_LEN + 1] = {};
            strncpy(pid, paramId, MAVLINK_MSG_PARAM_SET_FIELD_PARAM_ID_LEN);
            mavlink_msg_param_set_pack(sys, comp, m, targetSys, 1, pid, v,
                                       MAV_PARAM_TYPE_REAL32);
        });
        std::cout << "[mavlink] PARAM_SET " << paramId << " → " << v << std::endl;
    } else if (verb == "DO_CHANGE_SPEED") {
        // 飞行中临时切换速度(DO_CHANGE_SPEED #178,不修改飞控默认参数)
        // param1=0(airspeed), param2=speed m/s, param3=-1(不改油门), param4=0(绝对值)
        float speed = cmd.value("speed", 0.0f);
        sendPacked([&](mavlink_message_t *m) {
            mavlink_msg_command_long_pack(sys, comp, m, targetSys, 1,
                                          MAV_CMD_DO_CHANGE_SPEED, 0,
                                          0.0f, speed, -1.0f, 0.0f, 0, 0, 0);
        });
        std::cout << "[mavlink] DO_CHANGE_SPEED → " << speed << " m/s" << std::endl;
    } else if (verb == "VTOL") {
        // 3 = 悬停, 4 = 固定翼;也接受 vtol 字段数值
        float state = cmd.value("vtol", 0.0f);
        if (state == 0.0f) state = 3.0f; // 默认悬停
        sendPacked([&](mavlink_message_t *m) {
            mavlink_msg_command_long_pack(sys, comp, m, targetSys, 1,
                                          MAV_CMD_DO_VTOL_TRANSITION,
                                          0, state, 0, 0, 0, 0, 0, 0);
        });
        std::cout << "[mavlink] VTOL → " << state << std::endl;
    } else if (verb == "RC_OVERRIDE") {
        // viewer 超时停车期间丢弃(链路丢失保护,防失控摇杆值继续写入飞控)
        if (viewerTimeoutActive_) {
            std::cerr << "[mavlink] viewer 已超时停车,丢弃 RC_OVERRIDE" << std::endl;
            return;
        }
        if (!cmd.contains("channels") || !cmd.at("channels").is_array()) {
            std::cerr << "[mavlink] RC_OVERRIDE 缺少 channels 数组" << std::endl;
            return;
        }
        uint16_t ch[18] = {0}; // v2 协议为 18 通道,0 = 不改(通道释放)
        size_t i = 0;
        for (const auto &c : cmd.at("channels")) {
            if (i >= 16) break; // 上行最多 16 通道,17/18 恒为 0
            if (!c.is_number()) {
                // 非数字通道值:跳过该帧(不发送部分错误帧),打日志
                std::cerr << "[mavlink] RC_OVERRIDE 通道值非数字,跳过该帧" << std::endl;
                return;
            }
            double v = c.get<double>();
            if (v < 900.0) v = 900.0; // clamp 到 [900, 2100] 再转 uint16_t(防越界值直写飞控)
            else if (v > 2100.0) v = 2100.0;
            ch[i++] = static_cast<uint16_t>(v);
        }
        sendPacked([&](mavlink_message_t *m) {
            mavlink_msg_rc_channels_override_pack(sys, comp, m, targetSys, 1, ch[0], ch[1],
                                                  ch[2], ch[3], ch[4], ch[5], ch[6], ch[7],
                                                  ch[8], ch[9], ch[10], ch[11], ch[12],
                                                  ch[13], ch[14], ch[15], ch[16], ch[17]);
        });
    } else if (verb == "FWD_TARGETS") {
        // 遥测转发目标管理(Web 配置面板):{type:"fwd_targets", targets:["ip:port",...]}
        if (!cmd.contains("targets") || !cmd.at("targets").is_array()) {
            std::cerr << "[mavlink] FWD_TARGETS 缺少 targets 数组" << std::endl;
            return;
        }
        std::vector<std::string> targets;
        for (const auto &t : cmd.at("targets"))
            if (t.is_string()) targets.push_back(t.get<std::string>());
        setForwardTargets(targets, true);
        // 回执:当前列表
        if (telemetryCb_) {
            json reply;
            reply["t"] = "fwd_targets";
            reply["targets"] = forwardTargets();
            telemetryCb_(reply.dump());
        }
        return;
    } else if (verb == "GET_FWD_TARGETS") {
        if (telemetryCb_) {
            json reply;
            reply["t"] = "fwd_targets";
            reply["targets"] = forwardTargets();
            telemetryCb_(reply.dump());
        }
        return;
    } else if (verb == "FC_CONNECTION") {
        // 运行时热切换飞控连接方式:{cmd:"FC_CONNECTION", connection:"udp"|"usb",
        //   device?:"/dev/ttyACM0"(空=自动探测), baudrate?:115200, port?:5760}
        // port:可选数字字段(1-65535),更新 UDP 监听端口,reconfigure 重新 bind 生效
        if (cmd.contains("port")) {
            if (!cmd.at("port").is_number()) {
                sendCmdError("FC_CONNECTION port 字段非法(需数字)");
                return;
            }
            double portD = cmd.at("port").get<double>();
            if (portD < 1.0 || portD > 65535.0) {
                sendCmdError("FC_CONNECTION port 超范围(1-65535)");
                return;
            }
            cfg_.fcListenPort = static_cast<int>(portD); // reconfigure → openFcTransport 重绑
        }
        std::string conn = cmd.value("connection", "udp");
        std::string dev = cmd.value("device", "");
        int baud = cmd.value("baudrate", serialBaudrate_);
        bool ok = reconfigure(conn, dev, baud);
        replyConnectionState(true, ok);
        return;
    } else if (verb == "GET_FC_CONNECTION") {
        // 查询当前飞控连接配置(前端连接 edge 后同步用)
        replyConnectionState(false, false);
        return;
    } else {
        std::cout << "[mavlink] 未知命令(忽略): " << verb << std::endl;
        return;
    }
}

// ---- 运行时热切换 ----
// 由数据通道回调线程调用(handleCommand → reconfigure);reconfigureMutex_ 串行化
// 流程:停 run 线程(join)→ 关旧 fd_ → 切状态 + 重置学习/参数/解析 → 开新 fd_(失败回退)→ 重启线程
bool MavlinkBridge::reconfigure(const std::string &connection, const std::string &device, int baudrate) {
    std::lock_guard<std::mutex> lk(reconfigureMutex_);

    // 大小写不敏感(旧版仅认小写 "usb"/"serial","USB" 会被误判为 UDP)
    std::string conn = connection;
    for (auto &c : conn) c = static_cast<char>(tolower(c));
    Transport newT = (conn == "usb" || conn == "serial")
                         ? Transport::Serial : Transport::Udp;

    // 备份旧态(回退用)
    Transport oldT = transport_;
    std::string oldDev = serialDevice_;
    int oldBaud = serialBaudrate_;

    std::cout << "[mavlink] 切换连接方式: " << (transport_ == Transport::Serial ? "USB" : "UDP")
              << " → " << (newT == Transport::Serial ? "USB" : "UDP");
    if (newT == Transport::Serial)
        std::cout << " device=" << (device.empty() ? "<自动探测>" : device)
                  << " baud=" << baudrate;
    std::cout << std::endl;

    // lifecycleMutex_:与 stop() 互斥,串行 join/重启防 double-join UB;
    // 持至新线程启动完成,避免 stop 在切换中途抢入造成线程复活
    {
        std::lock_guard<std::mutex> life(lifecycleMutex_);

        // 1. 停 run 线程(join 后无任何线程访问 fd_/targetAddr_/fcMsg_ 等)
        stopFlag_ = true;
        if (thread_.joinable()) thread_.join();

        // 2. 关旧 FC fd(gcsFd_ 保留,与 transport 无关)
        closeFcTransport();

        // 3. 切换运行态 + 重置学习/参数/解析状态(新链路需重新建立)
        transport_ = newT;
        serialDevice_ = device;
        serialBaudrate_ = baudrate;
        addrLearned_ = false;
        paramRequested_ = false;
        paramRecv_.clear();
        paramRetry_ = 0;
        paramAllRecv_ = false;
        lastFcRx_ = {};
        memset(&fcMsg_, 0, sizeof(fcMsg_));     // 清解析状态(分片跨链路错位防护)
        memset(&fcStatus_, 0, sizeof(fcStatus_));
        statPkts_ = statMsgs_ = 0;
        msgCounts_.clear();

        // 4. 开新 transport
        bool ok = openFcTransport();
        if (!ok) {
            std::cerr << "[mavlink] 切换失败,回退到 "
                      << (oldT == Transport::Serial ? "USB" : "UDP") << std::endl;
            transport_ = oldT;
            serialDevice_ = oldDev;
            serialBaudrate_ = oldBaud;
            openFcTransport(); // 旧配置应能成功;若也失败 fd_=-1,run 守卫处理
        }

        // 5. 重启 run 线程(重建 pollfd,无 stale fd 风险)
        stopFlag_ = false;
        thread_ = std::thread(&MavlinkBridge::run, this);
        return ok;
    }
}

// 回执当前飞控连接状态(前端同步用);withOk=true 时附带切换结果 ok 字段
// 持 sendMutex_:serialDevice_ 等可能与 run 线程串口重开的写入并发
void MavlinkBridge::replyConnectionState(bool withOk, bool ok) {
    if (!telemetryCb_) return;
    std::string dev;
    int baud = 0;
    bool autoDetected = false;
    const char *conn = "udp";
    {
        std::lock_guard<std::mutex> lk(sendMutex_);
        conn = (transport_ == Transport::Serial) ? "usb" : "udp";
        dev = serialDevice_;
        baud = serialBaudrate_;
        autoDetected = serialAutoDetected_;
    }
    json reply;
    reply["t"] = "fc_connection";
    reply["connection"] = conn;
    reply["device"] = dev;
    reply["baudrate"] = baud;
    reply["auto_detected"] = autoDetected;
    if (withOk) reply["ok"] = ok;
    telemetryCb_(reply.dump());
}

} // namespace rcfpv
