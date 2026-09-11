/*
 * 5gipc-rc 摄像头音频桥(OpenIPC ssc338q) — USB 声卡 ↔ VPS edge 的 Opus/UDP 对接
 *
 * 链路:
 *   采集: USB声卡(ALSA) → 48k/mono/S16_LE → 软件增益 → Opus 编码(20ms/960) → UDP→VPS:7213
 *   播放: UDP←VPS(WebRTC RTP包) → 剥RTP头 → Opus 解码 → 软件音量 → ALSA 播放
 *   命令: VPS 文本命令(SET_GAIN/SET_HW_VOL/SET_SPK_VOL/GET_*) → 执行 → 回执文本
 *
 * 帧封装(与 vps-edge UdpAudioBridge 一致):
 *   [0x41][2B big-endian len][payload]    上行=裸 Opus 帧; 下行=WebRTC RTP 包
 *   命令/回执 = 纯文本 UTF-8
 *
 * 用法: audio_bridge [-l 7214] [-o 10.55.0.1:7213] [-i hw:0,0] [-p hw:1,0]
 *                    [-g 100] [-v 60] [-s 80]
 *   -i 采集设备(默认 hw:0,0 = card0 麦克风 mono); -p 播放设备(默认 hw:1,0 = card1 扬声器 stereo)
 * 依赖: alsa-lib + opus(均静态链接); 需内核 USB Audio 驱动(自编固件)
 */
#include <alsa/asoundlib.h>
#include <opus/opus.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RATE 48000
#define CH_CAP 1                 /* 采集声道(card0 mono) */
#define CH_PLAY 2                /* 播放声道(card1 stereo,仅 48k) */
#define FRAME 960                /* 20ms @48k */
#define FRAME_MAX 2880           /* 60ms @48k 最大帧:Chrome 拥塞时会发 40/60ms 帧 */
#define OPUS_MAX 1275
#define UDP_BUF 4096
#define MAGIC 0x41

/* 下行抗丢包/乱序:
 * - 解码器按 stereo 创建:edge 对讲轨 SDP 声明 opus/48000/2,浏览器按 stereo 编码,
 *   mono 解码器解 stereo 帧会数据错位出杂音;stereo 解码器兼容 mono/stereo 帧
 * - RTP 抖动缓冲重排 + 丢包 PLC 隐藏,避免蜂窝公网丢包/乱序造成断音爆音 */
#define CH_DEC 2                 /* 下行解码声道 */
#define JBUF 4                   /* RTP 重排窗口(4 帧) */
#define PLC_MAX 6                /* 连续丢包最多 PLC 补 6 帧(120ms),超限跳过缺口 */

/* ---------------- 全局状态 ---------------- */
static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static snd_pcm_t *cap = NULL, *play = NULL;
static OpusEncoder *enc = NULL;
static OpusDecoder *dec = NULL;

static int udp_fd = -1;
static struct sockaddr_in vps_addr;      /* 上行目标(10.55.0.1:7213) */
static struct sockaddr_in last_src;      /* 最近下行来源(VPS),回执发这里 */
static int have_src = 0;

static float mic_gain = 1.0f;            /* 采集软件增益(倍率) */
static int hw_vol = 60;                  /* 采集硬件音量%(mixer) */
static int spk_vol = 80;                 /* 播放软件音量% */
static float spk_scale = 0.8f;

/* 播放队列(有界,防抖):帧为 stereo 交错,支持 20/40/60ms 变长帧 */
#define QMAX 6                 /* 播放队列上限(6 帧):抗公网抖动断续 */
static short qbuf[QMAX][FRAME_MAX * 2];
static int qlen[QMAX];         /* 每帧每声道采样数(960/1920/2880) */
static int qn = 0;
static pthread_mutex_t qmtx = PTHREAD_MUTEX_INITIALIZER;

/* 下行 RTP 抖动缓冲(重排 + 丢包检测) */
static unsigned short jseq[JBUF];
static short jpcm[JBUF][FRAME_MAX * 2];
static int jlen[JBUF];         /* 每帧每声道采样数 */
static unsigned char jv[JBUF];
static unsigned short next_out = 0;
static int have_next = 0;

/* ---------------- 小工具 ---------------- */
static void set_vol_scale(int pct) {
    spk_vol = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    spk_scale = spk_vol / 100.0f;
}

/* 采集硬件音量:ALSA mixer "Capture" + 关 AGC */
static void apply_hw_vol(int pct) {
    hw_vol = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    snd_mixer_t *h = NULL;
    if (snd_mixer_open(&h, 0) < 0) return;
    if (snd_mixer_attach(h, "default") < 0) { snd_mixer_close(h); return; }
    snd_mixer_selem_register(h, NULL, NULL);
    snd_mixer_load(h);
    snd_mixer_selem_id_t *sid;
    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Capture");
    snd_mixer_elem_t *e = snd_mixer_find_selem(h, sid);
    if (e) {
        long mn, mx;
        snd_mixer_selem_get_capture_volume_range(e, &mn, &mx);
        snd_mixer_selem_set_capture_volume_all(e, mn + (mx - mn) * hw_vol / 100);
    }
    /* 关 AGC(通道名因卡而异,找不到不致命) */
    const char *agc_names[] = {"Auto Gain Control", "AGC", "Auto Level Control", NULL};
    for (int i = 0; agc_names[i]; i++) {
        snd_mixer_selem_id_set_name(sid, agc_names[i]);
        e = snd_mixer_find_selem(h, sid);
        if (e) { snd_mixer_selem_set_playback_switch_all(e, 0); }
    }
    snd_mixer_close(h);
}

/* 发送一帧到 VPS:[MAGIC][2B len][payload] 单包发送 */
/* 注意:必须单包(头+数据连续)。分两包发时接收端 select/recvfrom
 * 会把头包和数据包分开收,而解析按"同包内 3+len<=n"检查,数据会被丢弃
 * (UDP 单包上限 65507 > 3+OPUS_MAX,安全) */
static void send_frame(const void *payload, int len) {
    if (udp_fd < 0 || len <= 0 || len > 0xFFFF) return;
    unsigned char pkt[3 + OPUS_MAX];
    pkt[0] = MAGIC;
    pkt[1] = (unsigned char)(len >> 8);
    pkt[2] = (unsigned char)(len & 0xFF);
    memcpy(pkt + 3, payload, (size_t)len);
    sendto(udp_fd, pkt, 3 + (size_t)len, 0, (struct sockaddr *)&vps_addr,
           sizeof(vps_addr));
}

/* 发送文本回执到最近下行源 */
static void send_reply(const char *s) {
    if (!have_src) return;
    sendto(udp_fd, s, strlen(s), 0, (struct sockaddr *)&last_src, sizeof(last_src));
}

/* 处理文本命令 */
static void handle_cmd(const char *line) {
    char reply[64];
    if (strncmp(line, "SET_GAIN ", 9) == 0) {
        float pct = atof(line + 9);
        mic_gain = pct / 100.0f;
        snprintf(reply, sizeof(reply), "GAIN %.1f", mic_gain * 100.0f);
        send_reply(reply);
    } else if (strncmp(line, "GET_GAIN", 8) == 0) {
        snprintf(reply, sizeof(reply), "GAIN %.1f", mic_gain * 100.0f);
        send_reply(reply);
    } else if (strncmp(line, "SET_HW_VOL ", 11) == 0) {
        apply_hw_vol(atoi(line + 11));
        snprintf(reply, sizeof(reply), "HW_VOL %d", hw_vol);
        send_reply(reply);
    } else if (strncmp(line, "GET_HW_VOL", 10) == 0) {
        snprintf(reply, sizeof(reply), "HW_VOL %d", hw_vol);
        send_reply(reply);
    } else if (strncmp(line, "SET_SPK_VOL ", 12) == 0) {
        set_vol_scale(atoi(line + 12));
        snprintf(reply, sizeof(reply), "SPK_VOL %d", spk_vol);
        send_reply(reply);
    } else if (strncmp(line, "GET_SPK_VOL", 11) == 0) {
        snprintf(reply, sizeof(reply), "SPK_VOL %d", spk_vol);
        send_reply(reply);
    }
    /* 未知命令忽略 */
}

/* ---------------- 下行抖动缓冲 + 播放队列 ---------------- */

/* 送入播放队列(带音量钳制由播放线程做,这里只排队);len=每声道采样数 */
static void q_enqueue(const short *pcm, int len) {
    if (len <= 0 || len > FRAME_MAX) len = FRAME;
    pthread_mutex_lock(&qmtx);
    if (qn < QMAX) {
        memcpy(qbuf[qn], pcm, (size_t)len * 2 * sizeof(short));
        qlen[qn] = len;
        qn++;
    }
    pthread_mutex_unlock(&qmtx);
}

/* RTP 帧按序列号入抖动缓冲(槽=seq%JBUF, 窗口内 4 帧不冲突) */
static void jitter_in(unsigned short seq, const short *pcm, int len) {
    int slot = seq % JBUF;
    if (jv[slot] && jseq[slot] == seq) return; /* 重复包,丢弃 */
    jseq[slot] = seq;
    memcpy(jpcm[slot], pcm, (size_t)len * 2 * sizeof(short));
    jlen[slot] = len;
    jv[slot] = 1;
    if (!have_next || (int)(unsigned short)(seq - next_out) < 0) {
        next_out = seq;
        have_next = 1;
    }
}

/* 按序排出缓冲帧到播放队列;缺口用 PLC 补帧(最多 PLC_MAX),超限跳过 */
static void jitter_drain(void) {
    int plc = 0;
    while (have_next) {
        int slot = next_out % JBUF;
        if (jv[slot] && jseq[slot] == next_out) {
            jv[slot] = 0;
            q_enqueue(jpcm[slot], jlen[slot]);
            next_out++;
            continue;
        }
        /* 缺口:丢包,PLC 隐藏(静音延续) */
        if (plc < PLC_MAX) {
            /* static:OpenIPC ulimit -s 8192,函数栈放 11KB 数组会溢出崩溃 */
            static short pl[FRAME_MAX * 2];
            int n = opus_decode(dec, NULL, 0, pl, FRAME_MAX, 0);
            if (n > 0) {
                q_enqueue(pl, n);
                next_out++;
                plc++;
                continue;
            }
        }
        /* PLC 超限或失败:跳过缺口,定位下一个有效帧 */
        int adv;
        for (adv = 1; adv <= JBUF; adv++) {
            int s2 = (next_out + adv) % JBUF;
            if (jv[s2]) { next_out = jseq[s2]; break; }
        }
        if (adv > JBUF) have_next = 0;
    }
}

/* ---------------- 播放线程(stereo 直写 card1,变长帧) ---------------- */
static void *play_thread(void *arg) {
    (void)arg;
    static short silent[FRAME * 2];
    static short out[FRAME_MAX * 2];
    while (!g_stop) {
        short *pcm = NULL;
        int len = FRAME;
        pthread_mutex_lock(&qmtx);
        if (qn > 0) {
            pcm = qbuf[0];
            len = qlen[0];
            qn--;
            memmove(qbuf[0], qbuf[1], (size_t)qn * sizeof(qbuf[0]));
            memmove(qlen[0], qlen[1], (size_t)qn * sizeof(qlen[0]));
        }
        pthread_mutex_unlock(&qmtx);
        if (!pcm) pcm = (short *)silent;
        /* 软件音量(stereo 交错,已经是 2 声道) */
        for (int i = 0; i < len * 2; i++) {
            float v = pcm[i] * spk_scale;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            out[i] = (short)v;
        }
        snd_pcm_sframes_t r = snd_pcm_writei(play, out, len);
        if (r < 0) { snd_pcm_prepare(play); }
    }
    return NULL;
}

/* ---------------- 启动 ---------------- */
static int config_pcm(snd_pcm_t *p, int channels) {
    snd_pcm_hw_params_t *hp;
    snd_pcm_hw_params_alloca(&hp);
    snd_pcm_hw_params_any(p, hp);
    snd_pcm_hw_params_set_access(p, hp, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(p, hp, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(p, hp, channels);
    unsigned int rate = RATE;
    snd_pcm_hw_params_set_rate_near(p, hp, &rate, 0);
    snd_pcm_uframes_t per = FRAME;
    snd_pcm_hw_params_set_period_size_near(p, hp, &per, 0);
    /* buffer 收紧到 2 个 period(40ms):默认 buffer 可达 60ms+,对讲延迟大 */
    snd_pcm_uframes_t buf = per * 2;
    snd_pcm_hw_params_set_buffer_size_near(p, hp, &buf);
    return snd_pcm_hw_params(p, hp);
}

static int open_alsa(const char *capdev, const char *playdev) {
    if (snd_pcm_open(&cap, capdev, SND_PCM_STREAM_CAPTURE, 0) < 0) {
        fprintf(stderr, "capture open %s 失败: %s\n", capdev, snd_strerror(errno));
        return -1;
    }
    if (snd_pcm_open(&play, playdev, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "playback open %s 失败: %s\n", playdev, snd_strerror(errno));
        return -1;
    }
    if (config_pcm(cap, CH_CAP) < 0 || config_pcm(play, CH_PLAY) < 0) {
        fprintf(stderr, "hw params 设置失败\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *capdev = "hw:0,0";       /* 麦克风 card0(mono) */
    const char *playdev = "hw:1,0";      /* 扬声器 card1(stereo 48k) */
    int listen_port = 7214;
    const char *vps = "10.55.0.1:7213";
    int init_gain = 100, init_hw = 60, init_spk = 80;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l") && i + 1 < argc) listen_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) vps = argv[++i];
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) capdev = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) playdev = argv[++i];
        else if (!strcmp(argv[i], "-g") && i + 1 < argc) init_gain = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v") && i + 1 < argc) init_hw = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) init_spk = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h")) {
            printf("用法: %s [-l 7214] [-o IP:PORT] [-i 采集设备] [-p 播放设备] [-g 增益%%] [-v 采集音量%%] [-s 扬声器音量%%]\n", argv[0]);
            return 0;
        }
    }
    mic_gain = init_gain / 100.0f;
    hw_vol = init_hw;
    set_vol_scale(init_spk);

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    /* ---- ALSA ---- */
    if (open_alsa(capdev, playdev) < 0) return 1;
    apply_hw_vol(hw_vol);

    /* ---- Opus ---- */
    int oe = 0, od = 0;
    enc = opus_encoder_create(RATE, CH_CAP, OPUS_APPLICATION_VOIP, &oe);
    /* 下行解码器按 stereo:浏览器按 SDP opus/48000/2 编码,mono 解码器解 stereo 帧会错乱 */
    dec = opus_decoder_create(RATE, CH_DEC, &od);
    if (!enc || !dec) { fprintf(stderr, "opus init 失败 (%d/%d)\n", oe, od); return 1; }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(32000));
    opus_encoder_ctl(enc, OPUS_SET_VBR(1));

    /* ---- UDP ---- */
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) { perror("socket"); return 1; }
    struct sockaddr_in binda;
    memset(&binda, 0, sizeof(binda));
    binda.sin_family = AF_INET;
    binda.sin_addr.s_addr = htonl(INADDR_ANY);
    binda.sin_port = htons((unsigned short)listen_port);
    if (bind(udp_fd, (struct sockaddr *)&binda, sizeof(binda)) < 0) { perror("bind"); return 1; }
    char ip[64]; int port = 7213;
    sscanf(vps, "%63[^:]:%d", ip, &port);
    memset(&vps_addr, 0, sizeof(vps_addr));
    vps_addr.sin_family = AF_INET;
    vps_addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &vps_addr.sin_addr) != 1) { fprintf(stderr, "VPS 地址非法: %s\n", vps); return 1; }

    fprintf(stderr, "[audio_bridge] 采集=%s 播放=%s 监听:%d 上行=%s 增益=%.2f hw=%d%% spk=%d%%\n",
            capdev, playdev, listen_port, vps, mic_gain, hw_vol, spk_vol);

    /* ---- 播放线程 ---- */
    pthread_t pt;
    pthread_create(&pt, NULL, play_thread, NULL);

    /* ---- 主循环:采集 + UDP 轮询 ---- */
    short pcm[FRAME];
    unsigned char opus[OPUS_MAX];
    unsigned char buf[UDP_BUF];
    int startup_tick = 0;
    while (!g_stop) {
        /* 1. drain UDP(下行帧 → 播放队列;文本 → 命令) */
        for (;;) {
            struct timeval tv = {0, 0};
            fd_set rfds; FD_ZERO(&rfds); FD_SET(udp_fd, &rfds);
            if (select(udp_fd + 1, &rfds, NULL, NULL, &tv) <= 0) break;
            struct sockaddr_in src; socklen_t sl = sizeof(src);
            ssize_t n = recvfrom(udp_fd, buf, sizeof(buf), 0, (struct sockaddr *)&src, &sl);
            if (n <= 0) break;
            if (!have_src || src.sin_addr.s_addr != last_src.sin_addr.s_addr ||
                src.sin_port != last_src.sin_port) {
                last_src = src; have_src = 1;
            }
            if (buf[0] == MAGIC && n >= 3) {
                int len = (buf[1] << 8) | buf[2];
                if (len > 0 && 3 + len <= n) {
                    /* 剥 RTP 头(12B + csrc + ext + padding)取 Opus 帧 */
                    const unsigned char *p = buf + 3;
                    int plen = len;
                    unsigned short seq = 0;
                    if (plen >= 12 && (p[0] >> 6) == 2) {
                        seq = (unsigned short)((p[2] << 8) | p[3]);
                        int off = 12 + ((p[0] & 0x0F) << 2); /* 12 + csrc*4 */
                        /* padding:末字节为 pad 字节数,剥掉避免 opus 解包报错 */
                        if ((p[0] & 0x20) && plen >= off + 1) {
                            int pad = p[plen - 1];
                            if (pad > 0 && pad < plen) plen -= pad;
                        }
                        if ((p[0] & 0x10) && plen >= off + 4) {
                            /* RTP extension: [16bit profile][16bit len(4字节单位)] */
                            /* 长度字是 p[off+2]/p[off+3],不是 p[off]/p[off+1] */
                            int extlen = (((p[off + 2] & 0xFF) << 8) |
                                          (p[off + 3] & 0xFF)) << 2;
                            if (extlen > 0 && extlen <= plen - off - 4)
                                off += 4 + extlen;
                        }
                        if (off < plen) { p += off; plen -= off; }
                    }
                    if (plen > 0) {
                        /* static:OpenIPC ulimit -s 8192,栈上放 11KB 数组会溢出崩溃 */
                        static short out[FRAME_MAX * 2];
                        int got = opus_decode(dec, p, plen, out, FRAME_MAX, 0);
                        if (got < 0) got = 0;
                        /* 帧内样本不足 20ms(罕见):PLC 补齐到 FRAME,避免播放残留脏数据 */
                        if (got > 0 && got < FRAME) {
                            short tmp[FRAME];
                            int n2 = opus_decode(dec, NULL, 0, tmp, FRAME - got, 0);
                            if (n2 > 0) {
                                memcpy(out + (size_t)got * 2, tmp, (size_t)n2 * 2);
                                got += n2;
                            }
                        }
                        if (got <= 0) {
                            /* 整帧解码失败(损坏/半包/帧长不匹配):PLC 补一帧顶替,避免播放静音突断 */
                            int n3 = opus_decode(dec, NULL, 0, out, FRAME_MAX, 0);
                            got = n3 > 0 ? n3 : 0;
                        }
                        if (got > 0) jitter_in(seq, out, got);
                    }
                }
            } else {
                char line[512];
                size_t cl = (size_t)n; if (cl >= sizeof(line)) cl = sizeof(line) - 1;
                memcpy(line, buf, cl); line[cl] = 0;
                while (cl && (line[cl - 1] == '\n' || line[cl - 1] == '\r')) line[--cl] = 0;
                handle_cmd(line);
            }
        }

        /* 2.5 抖动缓冲按序排出(重排 + PLC 补丢包) */
        jitter_drain();

        /* 2. 采集 → 编码 → 发送 */
        snd_pcm_sframes_t rd = snd_pcm_readi(cap, pcm, FRAME);
        if (rd < 0) { snd_pcm_prepare(cap); continue; }
        if (rd != FRAME) continue;
        if (mic_gain != 1.0f) {
            for (int i = 0; i < FRAME; i++) {
                float v = pcm[i] * mic_gain;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                pcm[i] = (short)v;
            }
        }
        int ol = opus_encode(enc, pcm, FRAME, opus, sizeof(opus));
        if (ol > 0) send_frame(opus, ol);

        /* 3. 启动后 1s 主动上报一次(让 VPS 同步缓存值) */
        if (++startup_tick == 50) {
            char r[32];
            snprintf(r, sizeof(r), "GAIN %.1f", mic_gain * 100.0f); send_reply(r);
            snprintf(r, sizeof(r), "HW_VOL %d", hw_vol); send_reply(r);
            snprintf(r, sizeof(r), "SPK_VOL %d", spk_vol); send_reply(r);
            startup_tick = -1;
        }
    }

    pthread_join(pt, NULL);
    opus_encoder_destroy(enc);
    opus_decoder_destroy(dec);
    snd_pcm_close(cap);
    snd_pcm_close(play);
    close(udp_fd);
    fprintf(stderr, "[audio_bridge] 退出\n");
    return 0;
}
