/**
 * 5gipc-rc 方案2 骨架 — OpenIPC(ssc338q) 上 libdatachannel 内存基线验证
 *
 * 目的: 判定 91MB RAM 下 edge(libdatachannel WebRTC 栈)能否与 majestic 共存。
 * 手段: 创建完整 PeerConnection(视频轨 H265 + 音频轨 Opus + 数据通道),
 *       周期性打印 VmRSS/VmPeak, N 秒后退出。
 *
 * 注意: 本程序不连信令/不发媒体, 测得的是"PC 创建后静态内存"(下限)。
 *       真实 P2P(ICE/DTLS/SRTP 握手 + 媒体收发)峰值会更高, 需后续实测。
 *
 * 用法: edge_skel [秒数]
 * 编译: ARMv7 静态链接(见 build_edge_skel.sh)
 */
#include <rtc/rtc.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

static long vm_kb(const char *key) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long val = -1;
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0) {
            char *p = line + klen;
            while (*p == ' ' || *p == '\t' || *p == ':') p++;
            val = atol(p); // kB
            break;
        }
    }
    fclose(f);
    return val;
}

int main(int argc, char *argv[]) {
    int seconds = argc > 1 ? atoi(argv[1]) : 30;
    rtc::InitLogger(rtc::LogLevel::Warning);

    fprintf(stderr, "[edge_skel] libdatachannel %s\n", rtc::LIBDATACHANNEL_VERSION);
    fprintf(stderr, "[edge_skel] 创建 PeerConnection(视频H265+音频Opus+数据通道)...\n");

    rtc::Configuration cfg;
    // 骨架阶段不配 ICE 服务器(不连信令), 只测对象创建后的内存基线
    cfg.enableIceTcp = false;
    cfg.portRangeBegin = 0; // 不绑定媒体端口(不握手)

    auto pc = std::make_shared<rtc::PeerConnection>(cfg);

    // 视频轨: H265 发送方向(占位, 不发帧)
    auto video = pc->addTrack(rtc::Description::Video("video",
                                                      rtc::Description::Direction::SendOnly));
    auto videoRtp = std::make_shared<rtc::RtpPacketizationConfig>(42, "rcfpv", 96,
                                                                  rtc::H265RtpPacketizer::defaultClockRate);
    video->setMediaHandler(std::make_shared<rtc::H265RtpPacketizer>(videoRtp));

    // 音频轨: Opus 发送方向(占位)
    auto audio = pc->addTrack(rtc::Description::Audio("audio",
                                                      rtc::Description::Direction::SendOnly));
    auto audioRtp = std::make_shared<rtc::RtpPacketizationConfig>(43, "rcfpv", 111, 48000);
    audio->setMediaHandler(std::make_shared<rtc::OpusRtpPacketizer>(audioRtp));

    // 对讲轨: 接收方向(地面站 mic)
    pc->addTrack(rtc::Description::Audio("talk", rtc::Description::Direction::RecvOnly));

    // 数据通道(数控/遥测)
    auto dc = pc->createDataChannel("mavlink");
    dc->onOpen([]() { fprintf(stderr, "[edge_skel] 数据通道已打开(占位)\n"); });

    fprintf(stderr, "[edge_skel] PC 创建完成, 基线 RSS=%ldkB VmSize=%ldkB\n",
            vm_kb("VmRSS"), vm_kb("VmSize"));

    long peak = 0;
    for (int i = 0; i < seconds; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        long rss = vm_kb("VmRSS");
        long vpeak = vm_kb("VmPeak");
        if (vpeak > peak) peak = vpeak;
        fprintf(stdout, "t=%ds VmRSS=%ldkB VmPeak=%ldkB\n", i + 1, rss, vpeak);
        fflush(stdout);
    }

    fprintf(stderr, "[edge_skel] 结束, 峰值 RSS=%ldkB\n", peak);
    return 0;
}
