#include "media/udp_audio_bridge.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

namespace rcfpv {

namespace {
constexpr uint8_t kAudioMagic = 0x41; // 'A':音频帧前缀
constexpr int kSamplesPerFrame = 960; // 48kHz 单声道 20ms
} // namespace

UdpAudioBridge::UdpAudioBridge(int listenPort, std::string cameraIp, int cameraPort)
    : listenPort_(listenPort), cameraIp_(std::move(cameraIp)), cameraPort_(cameraPort) {}

UdpAudioBridge::~UdpAudioBridge() { stop(); }

bool UdpAudioBridge::start() {
    if (fd_ >= 0) return true;
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        perror("[udp-audio] socket");
        return false;
    }
    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(static_cast<uint16_t>(listenPort_));
    if (bind(fd_, reinterpret_cast<sockaddr *>(&bindAddr), sizeof(bindAddr)) < 0) {
        perror("[udp-audio] bind");
        close(fd_);
        fd_ = -1;
        return false;
    }
    camAddr_.sin_family = AF_INET;
    camAddr_.sin_port = htons(static_cast<uint16_t>(cameraPort_));
    if (inet_pton(AF_INET, cameraIp_.c_str(), &camAddr_.sin_addr) != 1) {
        std::cerr << "[udp-audio] 摄像头地址非法: " << cameraIp_ << std::endl;
        close(fd_);
        fd_ = -1;
        return false;
    }
    stopFlag_.store(false);
    thread_ = std::thread(&UdpAudioBridge::run, this);
    std::cout << "[udp-audio] 监听 0.0.0.0:" << listenPort_
              << " <- 摄像头音频桥, 下行 -> " << cameraIp_ << ":" << cameraPort_
              << std::endl;
    return true;
}

void UdpAudioBridge::stop() {
    stopFlag_.store(true);
    // 唤醒阻塞的 recvfrom
    if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

void UdpAudioBridge::sendDownstream(const uint8_t *data, size_t size) {
    if (fd_ < 0 || size == 0 || size > 0xFFFF) return;
    // 必须单包发送(头+数据连续):摄像头端按"同包内 3+len<=n"解析,
    // 分两包发会被当成两条独立 UDP 包,数据帧永远匹配不上
    uint8_t pkt[3 + 0xFFFF];
    pkt[0] = kAudioMagic;
    pkt[1] = static_cast<uint8_t>(size >> 8);
    pkt[2] = static_cast<uint8_t>(size & 0xFF);
    memcpy(pkt + 3, data, size);
    std::lock_guard<std::mutex> lk(sendMutex_);
    sendto(fd_, pkt, 3 + size, 0, reinterpret_cast<sockaddr *>(&camAddr_),
           sizeof(camAddr_));
}

void UdpAudioBridge::sendCommand(const std::string &cmd) {
    if (fd_ < 0 || cmd.empty()) return;
    std::lock_guard<std::mutex> lk(sendMutex_);
    sendto(fd_, cmd.data(), cmd.size(), 0, reinterpret_cast<sockaddr *>(&camAddr_),
           sizeof(camAddr_));
}

void UdpAudioBridge::run() {
    uint8_t buf[4096];
    while (!stopFlag_.load()) {
        sockaddr_in src{};
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(fd_, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr *>(&src), &slen);
        if (n <= 0) {
            if (stopFlag_.load()) break;
            continue;
        }
        if (buf[0] == kAudioMagic && n >= 3) {
            int len = (buf[1] << 8) | buf[2];
            if (len > 0 && 3 + len <= n && frameCb_)
                frameCb_(buf + 3, static_cast<size_t>(len), kSamplesPerFrame);
        } else if (replyCb_) {
            // 文本命令回执(摄像头音频桥主动上报或应答)
            std::string line(reinterpret_cast<char *>(buf), static_cast<size_t>(n));
            // 去掉行尾 \r\n
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            replyCb_(line);
        }
    }
}

} // namespace rcfpv
