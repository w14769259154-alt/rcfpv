/**
 * RCfpv Edge — RTSP 视频采集(FFmpeg)
 *
 * - 拉 RTSP 流(H265/H264,tcp/udp 可选),透传 Annex B NAL 帧
 * - 首个关键帧到达后才开始回调(保证解码器可初始化)
 * - 断线自动重连(最多 5 次,间隔 2s)
 * - 无数据看门狗:interrupt_callback 覆盖 open/探测/读帧全部阻塞调用,
 *   摄像头重启导致 TCP 半开挂死时强制中断重连(stimeout 不覆盖的路径兜底)
 * - RTP 时间戳全程连续(含重连),避免重连后时间戳回退致浏览器黑屏
 * - switchUrl() 运行时切换视频源(双路摄像头,线程安全)
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <stddef.h>
#include <string>
#include <thread>
#include <vector>

struct AVFormatContext; // FFmpeg C 前置声明(全局命名空间)

namespace rcfpv {

class RtspCapture {
public:
    // data: 完整 Annex B 帧(可能包含多个 NAL,带起始码)
    // isKeyframe: 是否关键帧;rtpTimestamp: 由流 pts 换算的 90kHz 时钟
    using FrameCallback = std::function<void(const uint8_t *data, size_t size,
                                             bool isKeyframe, uint32_t rtpTimestamp)>;

    RtspCapture(std::string url, std::string transport, int bufferSize,
                std::string codec = "H265");
    ~RtspCapture();

    RtspCapture(const RtspCapture &) = delete;
    RtspCapture &operator=(const RtspCapture &) = delete;

    void setFrameCallback(FrameCallback cb);
    bool start();
    void stop();

    // 通知采集线程断开当前连接并切换到新地址
    void switchUrl(const std::string &url);

    // 运行时应用新地址(副路双摄用):已运行→切换源;未运行→更新地址并启动;
    // 空地址→停止采集并释放连接
    void applyUrl(const std::string &url);

private:
    void run();
    bool openInput();
    void closeInput();

    // FFmpeg interrupt_callback:stop 请求或无数据看门狗超时返回 1 中断阻塞调用
    static int interruptCb(void *opaque);

    // 扫描包内 NAL 更新参数集缓存;返回本包是否自带参数集
    bool scanParamSets(const uint8_t *data, size_t size);
    // 参数集(H265: VPS/SPS/PPS, H264: SPS/PPS)+ 原始数据拼接为新 AnnexB 帧
    std::vector<uint8_t> buildFrameWithParamSets(const uint8_t *data, size_t size) const;

    std::string url_;
    std::string transport_;
    int bufferSize_;
    bool hevc_ = true; // H265(true) / H264(false)

    // NAL 类型 → 参数集体(不含起始码);关键帧注入用(廉价源参数集仅在流首出现)
    std::map<uint8_t, std::vector<uint8_t>> paramSets_;

    FrameCallback frameCallback_;

    std::thread thread_;
    std::atomic<bool> stopFlag_{false};
    std::atomic<bool> switchPending_{false};
    // nextUrl_ 切换交接:switchUrl(调用方线程)与采集线程的交换经 switchMtx_ 互斥。
    // 快速连续两次 switchUrl 时,第二次写 nextUrl_ 可能与采集线程的 url_=nextUrl_
    // 拷贝并发 → std::string 内部堆操作数据竞争(UB)。互斥消除之。
    std::mutex switchMtx_;
    std::string nextUrl_;

    // 无数据看门狗:上次 IO 进度时刻(steady_clock 毫秒)+ 超时阈值。
    // 摄像头重启 TCP 半开时 av_read_frame 可能永久阻塞(stimeout 不覆盖),
    // interruptCb 据此强制中断 → 走重连流程
    std::atomic<int64_t> lastProgressMs_{0};
    int64_t stallTimeoutMs_ = 10000; // 视频流持续有数据,10s 无进度必为异常

    AVFormatContext *fmtCtx_ = nullptr;
    int videoStreamIndex_ = -1;
};

} // namespace rcfpv
