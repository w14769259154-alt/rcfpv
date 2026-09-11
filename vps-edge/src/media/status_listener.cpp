#include "media/status_listener.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace rcfpv {

namespace {
// 允许的 WG 网段前缀(设备经隧道上报;公网伪造直接丢弃)
constexpr char kAllowedPrefix[] = "10.55.0.";
} // namespace

StatusListener::StatusListener(int listenPort) : listenPort_(listenPort) {}

StatusListener::~StatusListener() { stop(); }

bool StatusListener::start() {
    if (fd_ >= 0) return true;
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        perror("[status] socket");
        return false;
    }
    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(listenPort_));
    if (bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        perror("[status] bind");
        close(fd_);
        fd_ = -1;
        return false;
    }
    if (listen(fd_, 4) < 0) {
        perror("[status] listen");
        close(fd_);
        fd_ = -1;
        return false;
    }
    stopFlag_.store(false);
    thread_ = std::thread(&StatusListener::run, this);
    std::cout << "[status] 监听设备板载状态 TCP :" << listenPort_ << std::endl;
    return true;
}

void StatusListener::stop() {
    stopFlag_.store(true);
    if (fd_ >= 0) {
        shutdown(fd_, SHUT_RDWR);
        close(fd_);
        fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
}

DeviceStatus StatusListener::latest() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return status_;
}

void StatusListener::run() {
    while (!stopFlag_.load()) {
        sockaddr_in src{};
        socklen_t sl = sizeof(src);
        int c = accept(fd_, reinterpret_cast<sockaddr *>(&src), &sl);
        if (c < 0) {
            if (stopFlag_.load()) break;
            continue;
        }
        char buf[1024];
        ssize_t n = recv(c, buf, sizeof(buf) - 1, 0);
        close(c);
        if (n <= 0) continue;
        buf[n] = 0;

        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
        if (std::strncmp(ip, kAllowedPrefix, sizeof(kAllowedPrefix) - 1) != 0) {
            std::cerr << "[status] 忽略非 WG 来源: " << ip << std::endl;
            continue;
        }

        std::string line(buf);
        if (line.rfind("STATUS ", 0) != 0) continue;

        DeviceStatus ds;
        ds.updated = std::chrono::steady_clock::now();
        ds.fresh = true;
        size_t pos = 7; // "STATUS " 长度
        while (pos < line.size()) {
            auto sp = line.find(' ', pos);
            std::string kv = line.substr(pos, sp == std::string::npos ? std::string::npos
                                                                      : sp - pos);
            auto eq = kv.find('=');
            if (eq != std::string::npos && eq + 1 < kv.size()) {
                std::string k = kv.substr(0, eq);
                float v = std::atof(kv.c_str() + eq + 1);
                if (k == "temp") ds.temp = v;
                else if (k == "load") ds.load = v;
                else if (k == "mem") ds.memPct = static_cast<int>(v);
                else if (k == "disk") ds.diskPct = static_cast<int>(v);
                else if (k == "up") ds.uptimeS = static_cast<uint32_t>(v);
                else if (k == "rx") ds.rxKbps = v;
                else if (k == "tx") ds.txKbps = v;
            }
            if (sp == std::string::npos) break;
            pos = sp + 1;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            status_ = ds;
        }
    }
}

} // namespace rcfpv
