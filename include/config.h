#ifndef ZH_CONFIG_H
#define ZH_CONFIG_H

#include "bithion_core_config.h"

// 客户端宿主配置：板级资源、本地路径与体验参数。
// 设备身份、默认路由与共享音频契约由 bithion-core SDK 公共头提供。

// 字嗨客户端配置
// 这里后边要做成配置文件
#define ZH_WORK_BASE "/data/zh_work/"
#define ZH_KEY_PATH ZH_WORK_BASE"key" // 从字嗨科技官方获取的key
#define ZH_LOG_PATH ZH_WORK_BASE"logs/" // 日志目录路径

// 人脸识别相关
#define ZH_FACE_ENGINE_PATH ZH_WORK_BASE "face/face_engine" // 人脸识别引擎路径
#define ZH_RETINA_FACE_MODEL_PATH ZH_WORK_BASE "face/model/RetinaFace.rknn" // retinaface模型路径
#define ZH_FACENET_MODEL_PATH ZH_WORK_BASE "face/model/w600k_mbf_conv_fixed.rknn" // facenet模型路径
#define ZH_FACE_EMB_DIR ZH_WORK_BASE "face/save/" // 本地人脸emb文件夹路径


#define ZH_CARD_NAME "wlan0" // 客户端无线网卡名称

// websocket 重连
#define ZH_WS_RECONNECT_INTERVAL_MS 3000 // 重连超时
#define ZH_WS_RECONNECT_MAX 0 // 0 表示无限重连

// 音频后端选择：1=Rockit(RK_MPI), 0=ALSA
#define ZH_AUDIO_BACKEND_ROCKIT 1

// 录音/VAD 采集参数
#define ZH_AUDIO_DEVICE "hw:0,0" // 采集设备
#define ZH_AUDIO_CHANNELS 2 // ALSA 需要双通道；Rockit 录音为单通道并在软件中上混到双通道
#define ZH_AUDIO_GAIN 1.0f // 录音增益倍率，>1放大，<1衰减

// Rockit/RK_MPI AI+VQE(AEC) 配置
#define ZH_RK_AIVQE_CONFIG_PATH "/oem/usr/share/vqefiles/config_aivqe.json"
#define ZH_RK_AI_CARD_NAME "hw:0,0" // Rockit SDK 示例使用 hw:0,0
#define ZH_RK_AI_DEV 0
#define ZH_RK_AI_CHN 0
#define ZH_RK_AO_DEV 0
#define ZH_RK_AO_CHN 0
#define ZH_RK_AO_CARD_NAME "hw:0,0"
#define ZH_RK_ENABLE_LOOPBACK 1
#define ZH_RK_LOOPBACK_MODE "Mode2"
// VQE 声道布局：左声道为录音(rec=0x1)，右声道为参考(ref=0x2)
#define ZH_RK_REC_LAYOUT 0x1
#define ZH_RK_REF_LAYOUT 0x2
#define ZH_RK_CH_LAYOUT (ZH_RK_REC_LAYOUT | ZH_RK_REF_LAYOUT)

// TTS 播放参数
#define ZH_TTS_SAMPLE_RATE 16000 // 播放采样率
#define ZH_TTS_CHANNELS 1 // 播放通道数
#define ZH_TTS_MAX_SAMPLES 5760 // 单帧解码最大采样点
#define ZH_TTS_PLAY_DEVICE "plughw:0,0" // ALSA 播放设备
#define ZH_TTS_IDLE_TIMEOUT_MS 2000 // 长时间无可播放的包后,清理播放状态，避免一直卡在播放中
#define ZH_TTS_GAIN 0.5f // 播放增益倍率

// 系统提示音（支持本地文件路径或 HTTP/HTTPS URL；留空表示禁用）
#define ZH_PROMPT_MP3_BASE ZH_WORK_BASE "prompt_mp3/"
#define ZH_PROMPT_BOOT_MP3_URL ZH_PROMPT_MP3_BASE "boot.mp3"
// #define ZH_PROMPT_BOOT_MP3_URL ZH_PROMPT_MP3_BASE "ciallo.mp3"
#define ZH_PROMPT_PROVISION_MP3_URL ZH_PROMPT_MP3_BASE "provision.mp3"
#define ZH_PROMPT_NET_CONNECTED_MP3_URL ZH_PROMPT_MP3_BASE "net_connect.mp3"
#define ZH_PROMPT_NET_DISCONNECTED_MP3_URL ZH_PROMPT_MP3_BASE "net_disconnect.mp3"
// 系统提示音增益（0~1更小声，1为原始音量）
#define ZH_PROMPT_TONE_GAIN 1.0f

// 调试相关
// 是否存储 VAD 录音数据到文件
// 该文件保存的是“Opus编码前”的单声道PCM（与服务器收到前一致）
// 保存后，播放：ffplay -f s16le -ar 16000 -ch_layout mono record_pcm_dump.pcm
// 转换wav：ffmpeg -f s16le -ar 16000 -ch_layout mono -i record_pcm_dump.pcm ./test.wav

#ifndef ZH_VAD_RECORD_DUMP_ENABLE
#define ZH_VAD_RECORD_DUMP_ENABLE 0
#endif
#ifndef ZH_VAD_RECORD_DUMP_PATH
#define ZH_VAD_RECORD_DUMP_PATH "/tmp/record_pcm_dump.pcm"
#endif
#ifndef ZH_FORCE_LOCAL_ROUTE_CONFIG
#define ZH_FORCE_LOCAL_ROUTE_CONFIG 0
#endif

void zh_get_config(zh_config_t *cfg);

#endif
