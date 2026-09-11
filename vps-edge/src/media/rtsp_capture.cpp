#include "media/rtsp_capture.h"

#include <chrono>
#include <iostream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
}

namespace rcfpv {

namespace {
constexpr int kMaxRetries = 5;
constexpr auto kRetryInterval = std::chrono::seconds(2);

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string ffErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return std::string(buf);
}

// H265 AnnexB 关键帧检测:NAL 类型 19=IDR_W_RADL 20=IDR_N_LP 21=CRA;H264: 5=IDR
// (部分廉价 RTSP 源不置 AV_PKT_FLAG_KEY,必须解析 NAL 兜底)
bool nalHasKeyframe(const uint8_t *data, size_t size, bool hevc) {
    size_t i = 0;
    int checked = 0;
    while (i + 4 < size && checked < 8) { // 扫描前几个 NAL 即可
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            size_t nal = i + 3;
            uint8_t type = hevc ? ((data[nal] >> 1) & 0x3F) : (data[nal] & 0x1F);
            if (hevc ? (type >= 19 && type <= 21) : type == 5) return true;
            ++checked;
            i = nal + 1;
        } else {
            ++i;
        }
    }
    return false;
}
} // namespace

RtspCapture::RtspCapture(std::string url, std::string transport, int bufferSize,
                         std::string codec)
    : url_(std::move(url)), transport_(std::move(transport)), bufferSize_(bufferSize),
      hevc_(codec != "H264") {}

RtspCapture::~RtspCapture() { stop(); }

void RtspCapture::setFrameCallback(FrameCallback cb) { frameCallback_ = std::move(cb); }

bool RtspCapture::start() {
    if (thread_.joinable()) return true;
    stopFlag_ = false;
    thread_ = std::thread(&RtspCapture::run, this);
    return true;
}

void RtspCapture::stop() {
    stopFlag_ = true;
    if (thread_.joinable()) thread_.join();
}

void RtspCapture::switchUrl(const std::string &url) {
    {
        std::lock_guard<std::mutex> lk(switchMtx_);
        nextUrl_ = url;
    }
    switchPending_ = true; // 采集线程检测到后断开重连
    std::cout << "[rtsp] 请求切换视频源: " << url << std::endl;
}

void RtspCapture::applyUrl(const std::string &url) {
    if (thread_.joinable()) {
        if (url.empty()) {
            std::cout << "[rtsp] 停止采集(地址清空)" << std::endl;
            stop();
        } else {
            switchUrl(url);
        }
        return;
    }
    // 未运行:直接更新地址后启动(空地址保持停止)
    if (url.empty()) return;
    url_ = url;
    std::cout << "[rtsp] 启动采集(新地址): " << url << std::endl;
    start();
}

bool RtspCapture::openInput() {
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", transport_.c_str(), 0);
    av_dict_set(&opts, "stimeout", "5000000", 0); // 5s socket 超时(微秒)
    // 低延迟:禁 demux 缓冲 + 限制流探测时长(默认分析可达数秒,拉高起播延迟)
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "max_analyze_duration", "1000000", 0); // 1s
    if (bufferSize_ > 0)
        av_dict_set(&opts, "buffer_size", std::to_string(bufferSize_).c_str(), 0);

    fmtCtx_ = avformat_alloc_context();
    // 看门狗中断回调:覆盖 open/探测/读帧全部阻塞调用。
    // 摄像头重启期间 TCP 半开挂死(stimeout 不覆盖的路径)时强制中断 → 重连
    lastProgressMs_ = steadyMs(); // 握手+探测也算进度窗口的起点
    fmtCtx_->interrupt_callback.callback = &RtspCapture::interruptCb;
    fmtCtx_->interrupt_callback.opaque = this;
    int ret = avformat_open_input(&fmtCtx_, url_.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        std::cerr << "[rtsp] 打开失败(" << ffErr(ret) << "): " << url_ << std::endl;
        if (fmtCtx_) { // 部分版本失败时不释放,兜底
            avformat_free_context(fmtCtx_);
            fmtCtx_ = nullptr;
        }
        return false;
    }

    // 跳过 avformat_find_stream_info 深度探测:编码由配置指定(H265/H264),
    // RTSP demuxer 在 DESCRIBE/SDP 阶段即建立视频流并填充 codec_id/extradata
    // (sprop 参数集),无需探测。gdb 实测堆破坏发生在探测阶段(解析相机 RTP
    // 时破坏堆 → 随后读 streams[i] 野指针 SEGV,复现 30+ 次),跳过后彻底
    // 绕开该路径;副作用 = 起播更快(省 1s+ 探测)。
    // 流选择:RTSP SDP 通常仅视频;若含多媒体,按 SDP 已填的 codec_type 挑视频流
    videoStreamIndex_ = -1;
    for (unsigned i = 0; i < fmtCtx_->nb_streams; ++i) {
        AVStream *st = fmtCtx_->streams[i];
        if (st && st->codecpar && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex_ = static_cast<int>(i);
            break;
        }
    }
    if (videoStreamIndex_ < 0) {
        std::cerr << "[rtsp] 未找到视频流: " << url_ << std::endl;
        closeInput();
        return false;
    }

    // 从 extradata 初始化参数集缓存(FFmpeg 会把 RTSP SDP 的 sprop 参数集解析进
    // extradata,AnnexB 格式;多数廉价源流内不重复发参数集,必须从这里取)
    // 防御检查:流索引必须落在 nb_streams 内且流非空。曾发生过堆被破坏后
    // streams[i] 变成野指针导致 SEGV(gdb 实测 si_addr=野指针+codecpar 偏移),
    // 这里将异常降级为重连而非崩溃。不 deref 疑似野指针的 codecpar。
    if (videoStreamIndex_ < 0 || videoStreamIndex_ >= fmtCtx_->nb_streams ||
        !fmtCtx_->streams[videoStreamIndex_]) {
        std::cerr << "[rtsp] 流索引异常(idx=" << videoStreamIndex_
                  << ", nb_streams=" << fmtCtx_->nb_streams << "),重连" << std::endl;
        closeInput();
        return false;
    }
    paramSets_.clear();
    const AVCodecParameters *par = fmtCtx_->streams[videoStreamIndex_]->codecpar;
    if (par->extradata && par->extradata_size > 0) {
        scanParamSets(par->extradata, static_cast<size_t>(par->extradata_size));
        std::cout << "[rtsp] extradata " << par->extradata_size
                  << " 字节,解析出参数集 " << paramSets_.size() << " 个" << std::endl;
    }
    if (paramSets_.empty())
        std::cerr << "[rtsp] 警告:未获得参数集(关键帧注入不可用)" << std::endl;

    std::cout << "[rtsp] 已连接: " << url_ << " (视频流 #" << videoStreamIndex_ << ")"
              << std::endl;
    return true;
}

void RtspCapture::closeInput() {
    if (fmtCtx_) {
        avformat_close_input(&fmtCtx_);
        fmtCtx_ = nullptr;
    }
    videoStreamIndex_ = -1;
}

// FFmpeg 阻塞调用中断回调(在采集线程内被 FFmpeg 内部频繁调用,须极快、无锁):
// 返回 1 = 中断当前调用(返回 AVERROR_EXIT)。
// 触发条件:stop() 请求;或距上次 IO 进度超过 stallTimeoutMs_(无数据看门狗,
// 典型场景:摄像头断电重启,旧 TCP 半开无 FIN/RST,av_read_frame 永久阻塞)
int RtspCapture::interruptCb(void *opaque) {
    auto *self = static_cast<RtspCapture *>(opaque);
    if (self->stopFlag_.load(std::memory_order_relaxed)) return 1;
    const int64_t now = steadyMs();
    if (now - self->lastProgressMs_.load(std::memory_order_relaxed) > self->stallTimeoutMs_)
        return 1;
    return 0;
}

// 扫描包内 NAL,缓存参数集(H265: VPS=32/SPS=33/PPS=34;H264: SPS=7/PPS=8)
// 返回本包是否自带参数集(自带则无需注入)
bool RtspCapture::scanParamSets(const uint8_t *data, size_t size) {
    bool hasPs = false;
    size_t i = 0;
    while (i + 4 < size) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            size_t head = i + 3;
            uint8_t type = hevc_ ? ((data[head] >> 1) & 0x3F) : (data[head] & 0x1F);
            bool isPs = hevc_ ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
            // NAL 结束 = 下一个起始码起点或包尾
            size_t j = head + 1;
            while (j + 2 < size && !(data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1))
                ++j;
            if (j + 2 >= size) j = size;
            if (isPs) {
                paramSets_[type].assign(data + head, data + j); // 不含起始码
                hasPs = true;
            }
            i = head + 1;
        } else {
            ++i;
        }
    }
    return hasPs;
}

// 参数集 + 原始帧拼接(每个关键帧可独立解码,根治浏览器 PLI 循环)
std::vector<uint8_t> RtspCapture::buildFrameWithParamSets(const uint8_t *data,
                                                          size_t size) const {
    static const uint8_t kStartCode[4] = {0, 0, 0, 1};
    std::vector<uint8_t> out;
    auto append = [&](uint8_t type) {
        auto it = paramSets_.find(type);
        if (it == paramSets_.end()) return;
        out.insert(out.end(), kStartCode, kStartCode + 4);
        out.insert(out.end(), it->second.begin(), it->second.end());
    };
    if (hevc_) {
        append(32); // VPS
        append(33); // SPS
        append(34); // PPS
    } else {
        append(7);  // SPS
        append(8);  // PPS
    }
    out.insert(out.end(), data, data + size);
    return out;
}

void RtspCapture::run() {
    int retries = 0;

    // 低延迟时间戳基准:用本地到达时钟替代摄像头 PTS 生成 RTP 时间戳。
    // 廉价摄像头 PTS 常有抖动/回跳,会撑大浏览器 jitter buffer(可感知延迟)。
    // 注意:基准与 lastTs 声明在重连循环之外 —— RTP 时间戳必须跨重连续续,
    // 否则重连后从 0 重新计数,被 streamer 钳位成每帧仅 +1(正常 +3600),
    // 浏览器 jitter buffer 时间轴错乱 → 摄像头重启恢复后仍然黑屏。
    const auto tsBase = std::chrono::steady_clock::now();
    uint32_t lastTs = 0;

    while (!stopFlag_) {
        if (switchPending_.exchange(false)) {
            std::lock_guard<std::mutex> lk(switchMtx_);
            url_ = nextUrl_;
            closeInput();
            retries = 0; // 切换源后重置重连计数
        }

        if (!fmtCtx_ && !openInput()) {
            if (++retries > kMaxRetries) {
                std::cerr << "[rtsp] 重连次数超过 " << kMaxRetries << ",暂停 10s 后重试"
                          << std::endl;
                std::this_thread::sleep_for(std::chrono::seconds(10));
                retries = 0;
            } else {
                std::this_thread::sleep_for(kRetryInterval);
            }
            continue;
        }
        retries = 0;

        AVPacket *pkt = av_packet_alloc();
        bool haveKeyframe = false;

        // 帧率统计(每 5s 一条日志,便于诊断媒体链)
        uint64_t statFrames = 0, statBytes = 0, statKeys = 0, statInj = 0;
        auto statStart = std::chrono::steady_clock::now();

        // 内层读帧循环:退出条件 = 停止 / 切换源 / 读帧失败(断线或看门狗超时)
        while (!stopFlag_ && !switchPending_) {
            int ret = av_read_frame(fmtCtx_, pkt);
            if (ret < 0) {
                if (ret == AVERROR_EXIT && !stopFlag_) {
                    // interrupt_callback 中断:无数据看门狗触发(摄像头重启挂死)
                    // 或 stop();后者由外层 while 条件退出,这里只报看门狗
                    if (steadyMs() - lastProgressMs_.load() > stallTimeoutMs_) {
                        std::cerr << "[rtsp] 无数据看门狗触发(" << stallTimeoutMs_
                                  << "ms 无帧),强制断开重连(摄像头可能已重启)"
                                  << std::endl;
                    }
                } else {
                    std::cerr << "[rtsp] 读帧中断: " << ffErr(ret) << ",准备重连"
                              << std::endl;
                }
                break;
            }
            lastProgressMs_ = steadyMs(); // 喂狗:任何读到包都算 IO 进度
            if (pkt->stream_index != videoStreamIndex_) {
                av_packet_unref(pkt);
                continue;
            }

            bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            if (!key) key = nalHasKeyframe(pkt->data, static_cast<size_t>(pkt->size), hevc_);
            // 关键帧对齐:解码器需要从关键帧开始
            if (!haveKeyframe) {
                if (!key) {
                    av_packet_unref(pkt);
                    continue;
                }
                haveKeyframe = true;
            }

            uint32_t rtpTs = 0;
            {
                // 90kHz,steady_clock 保证单调平滑 → 接收端 jitter buffer 最小化。
                // 仅强制严格递增(+1),帧率由源时间戳自然决定;此前硬编码
                // lastTs+3600(40ms 帧距)假设 25fps,30fps 源会被强制
                // 0.75 倍速慢放
                auto now = std::chrono::steady_clock::now();
                uint64_t us =
                    std::chrono::duration_cast<std::chrono::microseconds>(now - tsBase)
                        .count();
                uint32_t arrival = static_cast<uint32_t>(us * 9 / 100);
                rtpTs = std::max(lastTs + 1, arrival);
                lastTs = rtpTs;
            }

            if (frameCallback_) {
                // 关键帧自带参数集则直发;否则前置注入缓存的 VPS/SPS/PPS
                bool hasPs = scanParamSets(pkt->data, static_cast<size_t>(pkt->size));
                if (key && !hasPs && !paramSets_.empty()) {
                    auto injected = buildFrameWithParamSets(pkt->data,
                                                            static_cast<size_t>(pkt->size));
                    frameCallback_(injected.data(), injected.size(), true, rtpTs);
                    ++statInj;
                } else {
                    frameCallback_(pkt->data, static_cast<size_t>(pkt->size), key, rtpTs);
                }
            }

            ++statFrames;
            statBytes += static_cast<uint64_t>(pkt->size);
            if (key) ++statKeys;
            auto elapsed = std::chrono::steady_clock::now() - statStart;
            if (elapsed >= std::chrono::seconds(5)) {
                auto secs =
                    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() /
                    1000.0;
                std::cout << "[rtsp] 统计: " << statFrames << " 帧 / "
                          << (statBytes / 1024) << " KB / 关键帧 " << statKeys
                          << " / 参数集注入 " << statInj << " (" << (statFrames / secs)
                          << " fps)" << std::endl;
                statFrames = statBytes = statKeys = statInj = 0;
                statStart = std::chrono::steady_clock::now();
            }

            av_packet_unref(pkt);
        }

        // 仅在确实一帧未收时才提示:此前按 statLogged(是否满 5s 打过统计)判断,
        // 连接不足 5 秒但已收到帧也会误报
        if (statFrames == 0)
            std::cerr << "[rtsp] 连接期间未收到任何视频帧(检查关键帧对齐/流内容)"
                      << std::endl;

        av_packet_free(&pkt);
        closeInput();

        if (!stopFlag_ && !switchPending_)
            std::this_thread::sleep_for(kRetryInterval);
    }
}

} // namespace rcfpv
