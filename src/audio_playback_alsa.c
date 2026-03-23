#include "config.h"

#if !ZH_AUDIO_BACKEND_ROCKIT

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>

#include "audio_playback_rockit.h"
#include "log.h"

struct zh_ao_playback {
    snd_pcm_t *pcm;
    unsigned int rate;
    unsigned int channels;
};

int zh_ao_playback_open(zh_ao_playback_t **out, unsigned int in_rate, unsigned int in_channels) {
    if (!out || in_rate == 0 || in_channels == 0) {
        errno = EINVAL;
        return -1;
    }

    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, ZH_TTS_PLAY_DEVICE, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        LOGE(__func__, "snd_pcm_open failed: %s", snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_t *params = NULL;
    snd_pcm_hw_params_malloc(&params);
    snd_pcm_hw_params_any(pcm, params);
    snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(pcm, params, in_channels);
    unsigned int rate = in_rate;
    snd_pcm_hw_params_set_rate_near(pcm, params, &rate, 0);
    snd_pcm_uframes_t period = ZH_AUDIO_FRAME_SAMPLES;
    snd_pcm_hw_params_set_period_size_near(pcm, params, &period, 0);
    err = snd_pcm_hw_params(pcm, params);
    snd_pcm_hw_params_free(params);
    if (err < 0) {
        LOGE(__func__, "snd_pcm_hw_params failed: %s", snd_strerror(err));
        snd_pcm_close(pcm);
        return -1;
    }
    snd_pcm_prepare(pcm);

    zh_ao_playback_t *pb = (zh_ao_playback_t *)calloc(1, sizeof(*pb));
    if (!pb) {
        snd_pcm_close(pcm);
        return -1;
    }
    pb->pcm = pcm;
    pb->rate = rate;
    pb->channels = in_channels;
    if (rate != in_rate) {
        LOGI(__func__, "playback rate adjusted: %u -> %u", in_rate, rate);
    }

    *out = pb;
    return 0;
}

int zh_ao_playback_write(zh_ao_playback_t *pb, const int16_t *pcm, size_t frames) {
    if (!pb || !pb->pcm || !pcm || frames == 0) {
        errno = EINVAL;
        return -1;
    }

    size_t written = 0;
    while (written < frames) {
        snd_pcm_sframes_t rc = snd_pcm_writei(pb->pcm, pcm + written * pb->channels, frames - written);
        if (rc > 0) {
            written += (size_t)rc;
            continue;
        }
        if (rc == 0) {
            // 设备暂时不可写，短等待后重试，避免卡死在忙等。
            snd_pcm_wait(pb->pcm, 20);
            continue;
        }
        int recover_rc = snd_pcm_recover(pb->pcm, (int)rc, 1);
        if (recover_rc == 0) {
            continue;
        }
        LOGE(__func__, "snd_pcm_writei failed: rc=%d (%s), recover=%d (%s)",
             (int)rc, snd_strerror((int)rc), recover_rc, snd_strerror(recover_rc));
        return -1;
    }
    return 0;
}

void zh_ao_playback_drain(zh_ao_playback_t *pb) {
    if (!pb || !pb->pcm) return;
    snd_pcm_drain(pb->pcm);
    snd_pcm_prepare(pb->pcm);
}

void zh_ao_playback_flush(zh_ao_playback_t *pb) {
    if (!pb || !pb->pcm) return;
    snd_pcm_drop(pb->pcm);
    snd_pcm_prepare(pb->pcm);
}

void zh_ao_playback_close(zh_ao_playback_t *pb) {
    if (!pb) return;
    if (pb->pcm) {
        snd_pcm_close(pb->pcm);
    }
    free(pb);
}

#endif
