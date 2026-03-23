#include <errno.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include "config.h"
#include "audio_device.h"
#include "log.h"

// ALSA设备封装：录音/播放设备初始化与关闭。

// 打开录音设备并配置采样参数。
int zh_audio_capture_open(snd_pcm_t **pcm_out,
                          snd_pcm_hw_params_t **params_out,
                          unsigned int *actual_rate,
                          unsigned int *actual_channels) {
    if (!pcm_out || !params_out) {
        errno = EINVAL;
        return -1;
    }

    snd_pcm_t *pcm = NULL;
    snd_pcm_hw_params_t *params = NULL;

    // 1) 打开录音设备。
    int err = snd_pcm_open(&pcm, ZH_AUDIO_DEVICE, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        LOGE(__func__, "snd_pcm_open failed: %s", snd_strerror(err));
        return -1;
    }

    // 2) 配置采样格式/通道/采样率/周期大小。
    snd_pcm_hw_params_malloc(&params);
    snd_pcm_hw_params_any(pcm, params);
    snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, params, ZH_AUDIO_CHANNELS);
    unsigned int rate = ZH_AUDIO_SAMPLE_RATE;
    snd_pcm_hw_params_set_rate_near(pcm, params, &rate, 0);
    snd_pcm_uframes_t period = ZH_AUDIO_FRAME_SAMPLES;
    snd_pcm_hw_params_set_period_size_near(pcm, params, &period, 0);
    err = snd_pcm_hw_params(pcm, params);
    if (err < 0) {
        LOGE(__func__, "snd_pcm_hw_params failed: %s", snd_strerror(err));
        snd_pcm_hw_params_free(params);
        snd_pcm_close(pcm);
        return -1;
    }
    // 3) 回填实际配置
    if (actual_rate) {
        int dir = 0;
        snd_pcm_hw_params_get_rate(params, actual_rate, &dir);
    }
    if (actual_channels) {
        snd_pcm_hw_params_get_channels(params, actual_channels);
    }
    // 4) 进入可读状态。
    snd_pcm_prepare(pcm);

    *pcm_out = pcm;
    *params_out = params;
    return 0;
}

// 关闭录音设备并释放参数结构。
void zh_audio_capture_close(snd_pcm_t *pcm, snd_pcm_hw_params_t *params) {
    if (params) {
        snd_pcm_hw_params_free(params);
    }
    if (pcm) {
        snd_pcm_close(pcm);
    }
}
