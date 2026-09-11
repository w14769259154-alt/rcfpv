/**
 * RCfpv Edge — 配置定义与加载
 *
 * 配置文件:config/edge_config.json(字段与旧版格式兼容)
 * 当前里程碑(M2)解析 RTSP / WebRTC / 信令 / MAVLink;
 * 音频 / 转发字段在后续里程碑接入,未知字段自动忽略。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rcfpv {

struct RtspConfig {
    std::string url;                // 主 RTSP 地址
    std::string url2;               // 副路 RTSP(双路切换,可为空)
    std::string codec = "H265";     // 视频编码:H265 / H264
    std::string transport = "tcp";  // RTSP 传输:tcp / udp
    int bufferSize = 212992;        // socket 缓冲区(字节)
};

// TURN 中继服务器配置(公网场景:开发板 NAT 后,ICE 打洞失败时经云服务器中继)
// 注意:必须定义在 WebRtcConfig 之前(WebRtcConfig 引用了 TurnServerConfig)
struct TurnServerConfig {
    std::string hostname;           // 云服务器 IP 或域名
    int port = 3478;                // TURN 端口
    std::string username;           // coturn 认证用户名
    std::string credential;         // coturn 认证密码
    std::string transport = "udp";  // udp / tcp / tls
};

struct WebRtcConfig {
    uint32_t videoSsrc = 42;
    uint32_t audioSsrc = 43;
    uint32_t auxVideoSsrc = 44;     // 副视频轨 SSRC(双摄 PIP)
    int videoPayloadType = 96;      // H265 PT
    int audioPayloadType = 111;     // Opus PT
    int videoPort = 5600;           // RTP 旁路转发端口(M6)
    std::vector<std::string> stunServers; // STUN 服务器 "stun:host:port"(P2P 打洞)
    std::vector<TurnServerConfig> turnServers; // TURN 中继(公网 NAT 后必须)
    int maxBitrate = 4000000;       // bps
};

struct RtspOutConfig {
    bool enabled = true;   // RTSP 输出服务器(视频转发给外部地面站)
    int port = 8554;
};

struct SignalingConfig {
    std::string serverUrl = "ws://127.0.0.1:8080/signaling";
    std::string roomId = "rcfpv-default";
    std::string clientId = "edge-node";
    bool disableTlsVerify = false; // 连云服务器 wss 自签证书时设为 true
};

struct MavlinkConfig {
    std::string connection = "udp";   // udp | usb(usb 内部走 serial)
    int fcListenPort = 5760;         // 监听飞控数据的本地 UDP 端口
    std::string fcTargetIp = "192.168.144.10";
    int fcTargetPort = 5762;
    int systemId = 255;
    int componentId = 1;
    double heartbeatRateHz = 1.0;
    std::vector<std::string> forwardTargets; // 遥测转发目标 "ip:port"(QGC/MP)
    int gcsListenPort = 14550;        // GCS 直连监听(QGC 惯例端口,双向透传)
    std::string serialDevice;         // USB 串口设备路径(空=自动探测 /dev/ttyACM* /dev/ttyUSB*)
    int serialBaudrate = 115200;      // 串口波特率(默认 115200)
};

// GCS 视频转发(RTP/UDP 推流,参考 RCfpv2.0 camera.sh 方案)
struct GcsVideoConfig {
    bool enabled = true;              // 总开关
    bool transcode = false;           // true=H265→H264 转码;false=H265 直通(新版 QGC 支持)
    int bitrateKbps = 2000;           // H264 转码码率
    std::vector<std::string> targets; // "ip:port"(默认端口 5600,QGC UDP 视频)
};

struct AudioConfig {
    std::string inputDevice = "plughw:1,0";  // ALSA 采集设备(USB 声卡 card 1)
    std::string outputDevice = "plughw:1,0"; // ALSA 播放设备(USB 声卡 card 1)
    int sampleRate = 48000;
    int channels = 1;
    int frameSize = 960;              // 20ms @48kHz
    bool enabled = true;              // input_device 为空时禁用
    float micGain = 1.0f;             // 麦克风采集软件增益(0~2,SET_MIC_GAIN 可远程调整)
    int micHwVolume = 60;             // 硬件采集音量(百分比)。Generalplus 实测:100% 底噪削波,60% 最优
    // VPS 版:与摄像头音频桥的 UDP 对接(替代本地 ALSA)
    int bridgeListenPort = 7213;      // VPS 监听:收摄像头 Opus 上行帧 + 命令回执
    std::string bridgeCameraIp = "10.55.0.2"; // 摄像头音频桥(WG 内地址)
    int bridgeCameraPort = 7214;      // 摄像头音频桥下行监听(音频帧 + 命令)
};

struct Config {
    RtspConfig rtsp;
    RtspOutConfig rtspOut;
    WebRtcConfig webrtc;
    SignalingConfig signaling;
    MavlinkConfig mavlink;
    GcsVideoConfig gcsVideo;
    AudioConfig audio;
};

// 加载 JSON 配置;文件不存在或字段缺失时使用默认值
Config loadConfig(const std::string &path);

// 保存副路 RTSP 地址到配置文件(读改写,保留其余字段);供 SET_AUX_URL 持久化
bool saveRtspUrl2(const std::string &path, const std::string &url);

// 保存主路 RTSP 地址到配置文件(读改写,保留其余字段);供 SET_MAIN_URL 持久化
bool saveRtspUrl(const std::string &path, const std::string &url);

// 保存麦克风采集增益到配置文件(读改写,保留其余字段);供 SET_MIC_GAIN 持久化
bool saveMicGain(const std::string &path, float gain);

// 保存麦克风硬件采集音量到配置文件(读改写,保留其余字段);供 SET_MIC_HW_VOLUME 持久化
bool saveMicHwVolume(const std::string &path, int volPct);

} // namespace rcfpv
