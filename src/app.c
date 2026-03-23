#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "audio_uplink.h"
#include "ble_provision.h"
#include "bithion_core_bridge.h"
#include "config.h"
#include "face_recognition.h"
#include "http.h"
#include "log.h"
#include "music_player.h"
#include "prompt_tone.h"
#include "udp_tts.h"
#include "utils.h"
#include "wifi_bootstrap.h"
#include "ws.h"

static void zh_wait_prompt_done(int timeout_ms) {
    int waited_ms = 0;
    while (zh_music_player_is_active() && waited_ms < timeout_ms) {
        usleep(50 * 1000);
        waited_ms += 50;
    }
}

// 单次会话流程：绑定设备->WS鉴权->启动UDP/TTS->进入WS循环。
static int zh_run_session(zh_config_t *zh_config, int *audio_started) {
    if (!zh_config) {
        errno = EINVAL;
        return -1;
    }

    // 绑定设备
    if (zh_http_bind_device(zh_config) != 0) {
        LOGE(__func__, "bind_device failed");
        return -1;
    }

    LOGI(__func__, "bind_device ok");
    // 若开机提示音仍在播放，先等待结束，避免联网提示音打断导致听感不完整。
    zh_wait_prompt_done(4000);
    LOGI(__func__, "trigger net connected prompt by bind_device");
    zh_prompt_tone_play(ZH_PROMPT_TONE_NET_CONNECTED);
    zh_wait_prompt_done(4000);

    // 绑定状态验证，刷新内部路由配置
    if (zh_http_check_bind(zh_config) != 0) {
        LOGE(__func__, "check_bind failed");
        return -1;
    }

    LOGI(__func__, "check_bind ok");

    // 启动录音+vad线程
    if (audio_started && !*audio_started) {
        if (zh_face_recognition_start() != 0) {
            LOGE(__func__, "face recognition start failed");
        }
        if (zh_audio_uplink_start() != 0) {
            zh_face_recognition_stop();
            LOGE(__func__, "audio uplink start failed");
        } else {
            *audio_started = 1;
        }
    }

    // ws建立连接
    // 建立WS连接（用于鉴权/状态通知）。
    zh_ws_session_t *ws = zh_ws_connect(zh_config, NULL);
    if (!ws) {
        LOGE(__func__, "ws connect failed");
        return -1;
    }

    // 等待服务器发送鉴权成功消息
    if (zh_ws_wait_authenticated(ws, 5000) != 0) {
        LOGE(__func__, "ws auth failed");
        zh_ws_close(ws);
        return -1;
    }

    // 启动 UDP 接收线程，等待下行 TTS
    if (zh_udp_tts_start(zh_config, ws) != 0) {
        LOGE(__func__, "udp tts start failed");
    }

    // 鉴权成功后，设置 WS 句柄供录音线程发送 START
    zh_audio_uplink_set_ws(ws);
    zh_face_recognition_set_ws(ws);

    // 持续处理 WS 消息，直到连接关闭或出错
    zh_ws_run_loop(ws);
    zh_audio_uplink_set_ws(NULL);
    zh_face_recognition_set_ws(NULL);
    zh_udp_tts_wait(2000);
    zh_udp_tts_stop();
    zh_ws_close(ws);

    return 0;
}

// 主业务循环：启动会话并负责断线重连。
int zh_app_run(zh_config_t *cfg) {
    // 如果device_id和key为空或者NULL，程序立即退出
    if (!cfg || cfg->device_id[0] == '\0' || cfg->key[0] == '\0') {
        LOGE(__func__, "invalid config: device_id or key is empty!!!");
        errno = EINVAL;
        return -1;
    }
    if (zh_music_player_start() != 0) {
        LOGE(__func__, "music player start failed");
    }
    int audio_started = 0; // 音频线程是否已启动过
    int retries = 0; // 重连次数统计
    while (1) {
        int rc = zh_run_session(cfg, &audio_started);
        if (ZH_WS_RECONNECT_MAX > 0 && ++retries > ZH_WS_RECONNECT_MAX) {
            LOGE(__func__, "ws reconnect retries exceeded");
            errno = ECONNRESET;
            return -1;
        }
        LOGI(__func__, "ws session ended (rc=%d), reconnecting in %d ms",
             rc, ZH_WS_RECONNECT_INTERVAL_MS);
        usleep(ZH_WS_RECONNECT_INTERVAL_MS * 1000);
    }
}

int zh_app_start(void) {
    zh_config_t cfg;

    LOGI(__func__, "==================================");
    LOGI(__func__, "zh uclibc linux client starting...");
    LOGI(__func__, "==================================");
    LOGI(__func__, "isatty(stdout)=%d", isatty(fileno(stdout)));

    // 获取客户端配置
    zh_get_config(&cfg);
    // 开机后优先预加载ONNX模型，再播放开机音并进入配网流程。
    if (zh_audio_uplink_preload() != 0) {
        LOGW(__func__, "preload onnx model failed, fallback to lazy init");
    }
    // 开机提示音：仅在提示音模块初始化成功后触发。
    if (zh_prompt_tone_init() != 0) {
        LOGE(__func__, "prompt tone init failed");
    } else {
        LOGI(__func__, "trigger boot prompt");
        zh_prompt_tone_play(ZH_PROMPT_TONE_BOOT);
    }

    int force_ble_retry = 0;
    while (1) {
        if (!force_ble_retry && zh_wifi_ensure_connected() == 0) {
            break;
        }

        // 进入 BLE 配网流程前提示。
        LOGI(__func__, "trigger provision prompt");
        zh_prompt_tone_play(ZH_PROMPT_TONE_PROVISION);
        if (zh_ble_provision_enter(cfg.device_id) != 0) {
            LOGW(__func__, "ble provision failed, retry ble provision immediately");
            force_ble_retry = 1;
        } else {
            LOGI(__func__, "ble provision exited, re-check network");
            force_ble_retry = 0;
        }
    }
    LOGI(__func__, "network ready, enter main app");

    LOGI(__func__, "api_base url=%s", ZH_API_BASE_URL);
    if (zh_ensure_realtime_clock_valid() < 0) {
        LOGW(__func__, "realtime clock is still invalid, TLS certificate verification may fail");
    }

    if (zh_app_run(&cfg) != 0) {
        LOGE(__func__, "app run failed: errno=%d (%s)", errno, strerror(errno));
    }
    LOGI(__func__, "**********************************");
    LOGI(__func__, "zh uclibc linux client exit.");
    LOGI(__func__, "**********************************");

    return 0;
}
