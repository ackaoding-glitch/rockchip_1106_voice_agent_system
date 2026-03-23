#include <pthread.h>
#include <stdint.h>

#include "config.h"
#include "log.h"
#include "music_player.h"
#include "prompt_tone.h"
#include "utils.h"

// 保护初始化状态和断网提示时间戳。
static pthread_mutex_t g_prompt_mutex = PTHREAD_MUTEX_INITIALIZER;
// music_player 是否已初始化，避免重复启动。
static int g_prompt_inited = 0;
// 最近一次播放“网络断开”提示音的时间戳（毫秒）。
static uint64_t g_last_disconnected_ms = 0;

static const char *zh_prompt_tone_event_name(zh_prompt_tone_event_t event) {
    switch (event) {
        case ZH_PROMPT_TONE_BOOT:
            return "boot";
        case ZH_PROMPT_TONE_PROVISION:
            return "provision";
        case ZH_PROMPT_TONE_NET_CONNECTED:
            return "net_connected";
        case ZH_PROMPT_TONE_NET_DISCONNECTED:
            return "net_disconnected";
        default:
            return "unknown";
    }
}

// 将事件映射为配置中的 MP3 URL。
static const char *zh_prompt_tone_url(zh_prompt_tone_event_t event) {
    switch (event) {
        case ZH_PROMPT_TONE_BOOT:
            return ZH_PROMPT_BOOT_MP3_URL;
        case ZH_PROMPT_TONE_PROVISION:
            return ZH_PROMPT_PROVISION_MP3_URL;
        case ZH_PROMPT_TONE_NET_CONNECTED:
            return ZH_PROMPT_NET_CONNECTED_MP3_URL;
        case ZH_PROMPT_TONE_NET_DISCONNECTED:
            return ZH_PROMPT_NET_DISCONNECTED_MP3_URL;
        default:
            return "";
    }
}

int zh_prompt_tone_init(void) {
    int rc = 0;
    pthread_mutex_lock(&g_prompt_mutex);
    if (!g_prompt_inited) {
        rc = zh_music_player_start();
        if (rc == 0) {
            g_prompt_inited = 1;
            LOGI(__func__, "prompt tone init ok");
        }
    }
    pthread_mutex_unlock(&g_prompt_mutex);
    return rc;
}

void zh_prompt_tone_play(zh_prompt_tone_event_t event) {
    const char *url = zh_prompt_tone_url(event);
    const char *urls[1] = {url};

    if (!url || url[0] == '\0') {
        LOGW(__func__, "prompt tone skipped: event=%s url is empty",
             zh_prompt_tone_event_name(event));
        return;
    }

    if (zh_prompt_tone_init() != 0) {
        LOGE(__func__, "prompt tone init failed");
        return;
    }

    if (event == ZH_PROMPT_TONE_NET_DISCONNECTED) {
        uint64_t now_ms = zh_now_ms();
        pthread_mutex_lock(&g_prompt_mutex);
        // 断网事件可能由 WS ERROR/CLOSE 连续触发，5 秒内只播一次。
        if (g_last_disconnected_ms != 0 && now_ms - g_last_disconnected_ms < 5000) {
            pthread_mutex_unlock(&g_prompt_mutex);
            LOGI(__func__, "prompt tone debounced: event=%s", zh_prompt_tone_event_name(event));
            return;
        }
        g_last_disconnected_ms = now_ms;
        pthread_mutex_unlock(&g_prompt_mutex);
    }

    LOGI(__func__, "prompt tone play: event=%s src=%s",
         zh_prompt_tone_event_name(event), url);
    zh_music_player_play_urls_with_gain(urls, 1, ZH_PROMPT_TONE_GAIN);
}
