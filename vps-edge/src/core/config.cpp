#include "core/config.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>

namespace rcfpv {

using nlohmann::json;

namespace {

template <typename T>
T getOr(const json &j, const char *key, T def) {
    if (j.is_object() && j.contains(key) && !j.at(key).is_null()) {
        try {
            return j.at(key).get<T>();
        } catch (const json::exception &) {
            std::cerr << "[config] 字段类型错误,使用默认值: " << key << std::endl;
        }
    }
    return def;
}

} // namespace

Config loadConfig(const std::string &path) {
    Config cfg;

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        std::cerr << "[config] 无法打开配置文件,使用默认配置: " << path << std::endl;
        return cfg;
    }

    json root;
    try {
        ifs >> root;
    } catch (const json::exception &e) {
        std::cerr << "[config] JSON 解析失败: " << e.what() << std::endl;
        return cfg;
    }

    if (root.contains("rtsp")) {
        const auto &j = root.at("rtsp");
        cfg.rtsp.url = getOr<std::string>(j, "url", cfg.rtsp.url);
        cfg.rtsp.url2 = getOr<std::string>(j, "url2", cfg.rtsp.url2);
        cfg.rtsp.codec = getOr<std::string>(j, "codec", cfg.rtsp.codec);
        cfg.rtsp.transport = getOr<std::string>(j, "transport", cfg.rtsp.transport);
        cfg.rtsp.bufferSize = getOr<int>(j, "buffer_size", cfg.rtsp.bufferSize);
    }

    if (root.contains("rtsp_out")) {
        const auto &j = root.at("rtsp_out");
        cfg.rtspOut.enabled = getOr<bool>(j, "enabled", cfg.rtspOut.enabled);
        cfg.rtspOut.port = getOr<int>(j, "port", cfg.rtspOut.port);
    }

    if (root.contains("webrtc")) {
        const auto &j = root.at("webrtc");
        cfg.webrtc.videoSsrc = getOr<uint32_t>(j, "video_ssrc", cfg.webrtc.videoSsrc);
        cfg.webrtc.audioSsrc = getOr<uint32_t>(j, "audio_ssrc", cfg.webrtc.audioSsrc);
        cfg.webrtc.auxVideoSsrc = getOr<uint32_t>(j, "aux_video_ssrc", cfg.webrtc.auxVideoSsrc);
        cfg.webrtc.videoPayloadType = getOr<int>(j, "video_payload_type", cfg.webrtc.videoPayloadType);
        cfg.webrtc.audioPayloadType = getOr<int>(j, "audio_payload_type", cfg.webrtc.audioPayloadType);
        cfg.webrtc.videoPort = getOr<int>(j, "video_port", cfg.webrtc.videoPort);
        cfg.webrtc.maxBitrate = getOr<int>(j, "max_bitrate", cfg.webrtc.maxBitrate);
        if (j.contains("stun_servers") && j.at("stun_servers").is_array()) {
            for (const auto &s : j.at("stun_servers")) {
                if (s.is_string() && !s.get<std::string>().empty())
                    cfg.webrtc.stunServers.push_back(s.get<std::string>());
            }
        }
        // TURN 中继服务器(公网 NAT 后场景:打洞失败时经云服务器中继)
        if (j.contains("turn_servers") && j.at("turn_servers").is_array()) {
            for (const auto &t : j.at("turn_servers")) {
                if (!t.is_object()) continue;
                TurnServerConfig tc;
                tc.hostname = getOr<std::string>(t, "hostname", tc.hostname);
                tc.port = getOr<int>(t, "port", tc.port);
                tc.username = getOr<std::string>(t, "username", tc.username);
                tc.credential = getOr<std::string>(t, "credential", tc.credential);
                tc.transport = getOr<std::string>(t, "transport", tc.transport);
                if (!tc.hostname.empty()) cfg.webrtc.turnServers.push_back(tc);
            }
        }
    }

    if (root.contains("signaling")) {
        const auto &j = root.at("signaling");
        cfg.signaling.serverUrl = getOr<std::string>(j, "server_url", cfg.signaling.serverUrl);
        cfg.signaling.roomId = getOr<std::string>(j, "room_id", cfg.signaling.roomId);
        cfg.signaling.clientId = getOr<std::string>(j, "client_id", cfg.signaling.clientId);
        cfg.signaling.disableTlsVerify =
            getOr<bool>(j, "disable_tls_verify", cfg.signaling.disableTlsVerify);
    }

    if (root.contains("mavlink")) {
        const auto &j = root.at("mavlink");
        cfg.mavlink.connection = getOr<std::string>(j, "connection", cfg.mavlink.connection);
        cfg.mavlink.fcListenPort = getOr<int>(j, "fc_listen_port", cfg.mavlink.fcListenPort);
        cfg.mavlink.fcTargetIp = getOr<std::string>(j, "fc_target_ip", cfg.mavlink.fcTargetIp);
        cfg.mavlink.fcTargetPort = getOr<int>(j, "fc_target_port", cfg.mavlink.fcTargetPort);
        cfg.mavlink.systemId = getOr<int>(j, "system_id", cfg.mavlink.systemId);
        cfg.mavlink.componentId = getOr<int>(j, "component_id", cfg.mavlink.componentId);
        cfg.mavlink.heartbeatRateHz = getOr<double>(j, "heartbeat_rate_hz", cfg.mavlink.heartbeatRateHz);
        if (j.contains("forward_targets") && j.at("forward_targets").is_array()) {
            for (const auto &t : j.at("forward_targets")) {
                if (t.is_string() && !t.get<std::string>().empty())
                    cfg.mavlink.forwardTargets.push_back(t.get<std::string>());
            }
        }
        cfg.mavlink.gcsListenPort = getOr<int>(j, "gcs_listen_port", cfg.mavlink.gcsListenPort);
        cfg.mavlink.serialDevice = getOr<std::string>(j, "serial_device", cfg.mavlink.serialDevice);
        cfg.mavlink.serialBaudrate = getOr<int>(j, "serial_baudrate", cfg.mavlink.serialBaudrate);
    }

    if (root.contains("gcs_video")) {
        const auto &j = root.at("gcs_video");
        cfg.gcsVideo.enabled = getOr<bool>(j, "enabled", cfg.gcsVideo.enabled);
        cfg.gcsVideo.transcode = getOr<bool>(j, "transcode", cfg.gcsVideo.transcode);
        cfg.gcsVideo.bitrateKbps = getOr<int>(j, "bitrate_kbps", cfg.gcsVideo.bitrateKbps);
        if (j.contains("targets") && j.at("targets").is_array()) {
            for (const auto &t : j.at("targets")) {
                if (t.is_string() && !t.get<std::string>().empty())
                    cfg.gcsVideo.targets.push_back(t.get<std::string>());
            }
        }
    }

    if (root.contains("audio")) {
        const auto &j = root.at("audio");
        cfg.audio.inputDevice = getOr<std::string>(j, "input_device", cfg.audio.inputDevice);
        cfg.audio.outputDevice = getOr<std::string>(j, "output_device", cfg.audio.outputDevice);
        cfg.audio.sampleRate = getOr<int>(j, "sample_rate", cfg.audio.sampleRate);
        cfg.audio.channels = getOr<int>(j, "channels", cfg.audio.channels);
        cfg.audio.frameSize = getOr<int>(j, "frame_size", cfg.audio.frameSize);
        cfg.audio.micGain = getOr<float>(j, "mic_gain", cfg.audio.micGain);
        cfg.audio.micHwVolume = getOr<int>(j, "mic_hw_volume", cfg.audio.micHwVolume);
        cfg.audio.bridgeListenPort = getOr<int>(j, "bridge_listen_port", cfg.audio.bridgeListenPort);
        cfg.audio.bridgeCameraIp = getOr<std::string>(j, "bridge_camera_ip", cfg.audio.bridgeCameraIp);
        cfg.audio.bridgeCameraPort = getOr<int>(j, "bridge_camera_port", cfg.audio.bridgeCameraPort);
    }
    // VPS 版:无本地 ALSA,音频是否启用由"音频桥端口 > 0"决定(input_device 可为空)
    cfg.audio.enabled = !cfg.audio.inputDevice.empty() || cfg.audio.bridgeListenPort > 0;

    return cfg;
}

bool saveRtspUrl2(const std::string &path, const std::string &url) {
    // 读改写:保留文件中其余字段(含未知字段),仅更新 rtsp.url2
    json root;
    {
        std::ifstream ifs(path);
        if (ifs.is_open()) {
            try {
                ifs >> root;
            } catch (const json::exception &) {
                std::cerr << "[config] 保存 url2:原文件解析失败,将重建" << std::endl;
            }
        }
    }
    if (!root.is_object()) root = json::object();
    if (!root.contains("rtsp") || !root.at("rtsp").is_object())
        root["rtsp"] = json::object();
    root["rtsp"]["url2"] = url;

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        std::cerr << "[config] 保存 url2 失败(不可写): " << path << std::endl;
        return false;
    }
    ofs << root.dump(2) << std::endl;
    return true;
}

bool saveRtspUrl(const std::string &path, const std::string &url) {
    // 读改写:保留文件中其余字段(含未知字段),仅更新 rtsp.url
    json root;
    {
        std::ifstream ifs(path);
        if (ifs.is_open()) {
            try {
                ifs >> root;
            } catch (const json::exception &) {
                std::cerr << "[config] 保存 url:原文件解析失败,将重建" << std::endl;
            }
        }
    }
    if (!root.is_object()) root = json::object();
    if (!root.contains("rtsp") || !root.at("rtsp").is_object())
        root["rtsp"] = json::object();
    root["rtsp"]["url"] = url;

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        std::cerr << "[config] 保存 url 失败(不可写): " << path << std::endl;
        return false;
    }
    ofs << root.dump(2) << std::endl;
    return true;
}

bool saveMicGain(const std::string &path, float gain) {
    // 读改写:保留文件中其余字段(含未知字段),仅更新 audio.mic_gain(限幅 0~2)
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 2.0f) gain = 2.0f;
    json root;
    {
        std::ifstream ifs(path);
        if (ifs.is_open()) {
            try {
                ifs >> root;
            } catch (const json::exception &) {
                std::cerr << "[config] 保存 mic_gain:原文件解析失败,将重建" << std::endl;
            }
        }
    }
    if (!root.is_object()) root = json::object();
    if (!root.contains("audio") || !root.at("audio").is_object())
        root["audio"] = json::object();
    root["audio"]["mic_gain"] = gain;

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        std::cerr << "[config] 保存 mic_gain 失败(不可写): " << path << std::endl;
        return false;
    }
    ofs << root.dump(2) << std::endl;
    return true;
}

bool saveMicHwVolume(const std::string &path, int volPct) {
    // 读改写:保留文件中其余字段(含未知字段),仅更新 audio.mic_hw_volume(限幅 0~100)
    if (volPct < 0) volPct = 0;
    if (volPct > 100) volPct = 100;
    json root;
    {
        std::ifstream ifs(path);
        if (ifs.is_open()) {
            try {
                ifs >> root;
            } catch (const json::exception &) {
                std::cerr << "[config] 保存 mic_hw_volume:原文件解析失败,将重建" << std::endl;
            }
        }
    }
    if (!root.is_object()) root = json::object();
    if (!root.contains("audio") || !root.at("audio").is_object())
        root["audio"] = json::object();
    root["audio"]["mic_hw_volume"] = volPct;

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs.is_open()) {
        std::cerr << "[config] 保存 mic_hw_volume 失败(不可写): " << path << std::endl;
        return false;
    }
    ofs << root.dump(2) << std::endl;
    return true;
}

} // namespace rcfpv
