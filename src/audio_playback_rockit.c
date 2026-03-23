#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "audio_playback_rockit.h"
#include "config.h"
#include "log.h"

#if ZH_AUDIO_BACKEND_ROCKIT

#include "rk_mpi_sys.h"
#include "rk_mpi_ao.h"
#include "rk_mpi_mb.h"
#include "rk_comm_aio.h"

struct zh_ao_playback {
    AUDIO_DEV dev;
    AO_CHN chn;
    unsigned int in_rate;
    unsigned int in_channels;
    int resample_enabled;
};

static int g_sys_inited = 0;

static AUDIO_SAMPLE_RATE_E zh_rk_map_rate(unsigned int rate) {
    switch (rate) {
        case 8000: return AUDIO_SAMPLE_RATE_8000;
        case 16000: return AUDIO_SAMPLE_RATE_16000;
#ifdef AUDIO_SAMPLE_RATE_24000
        case 24000: return AUDIO_SAMPLE_RATE_24000;
#endif
        case 32000: return AUDIO_SAMPLE_RATE_32000;
        case 44100: return AUDIO_SAMPLE_RATE_44100;
        case 48000: return AUDIO_SAMPLE_RATE_48000;
        default: return AUDIO_SAMPLE_RATE_16000;
    }
}

static int zh_rk_sys_init_once(void) {
    if (g_sys_inited) {
        return 0;
    }
    RK_S32 ret = RK_MPI_SYS_Init();
    if (ret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_SYS_Init failed: %d", ret);
        return -1;
    }
    g_sys_inited = 1;
    return 0;
}

int zh_ao_playback_open(zh_ao_playback_t **out, unsigned int in_rate, unsigned int in_channels) {
    if (!out || in_rate == 0 || in_channels == 0) {
        errno = EINVAL;
        return -1;
    }
    if (zh_rk_sys_init_once() != 0) {
        return -1;
    }

    zh_ao_playback_t *pb = (zh_ao_playback_t *)calloc(1, sizeof(*pb));
    if (!pb) {
        return -1;
    }
    pb->dev = ZH_RK_AO_DEV;
    pb->chn = ZH_RK_AO_CHN;
    pb->in_rate = in_rate;
    pb->in_channels = in_channels;

    AIO_ATTR_S attr;
    memset(&attr, 0, sizeof(attr));
    if (ZH_RK_AO_CARD_NAME[0] != '\0') {
        strncpy((char *)attr.u8CardName, ZH_RK_AO_CARD_NAME, sizeof(attr.u8CardName) - 1);
    }
    attr.soundCard.channels = 2;
    attr.soundCard.sampleRate = ZH_TTS_SAMPLE_RATE;
    attr.soundCard.bitWidth = AUDIO_BIT_WIDTH_16;
    attr.enSamplerate = zh_rk_map_rate(ZH_TTS_SAMPLE_RATE);
    attr.enBitwidth = AUDIO_BIT_WIDTH_16;
    attr.enSoundmode = (in_channels == 1) ? AUDIO_SOUND_MODE_MONO : AUDIO_SOUND_MODE_STEREO;
    attr.u32FrmNum = 4;
    attr.u32PtNumPerFrm = 1024;
    attr.u32EXFlag = 0;
    attr.u32ChnCnt = 2;

    RK_S32 ret = RK_MPI_AO_SetPubAttr(pb->dev, &attr);
    if (ret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AO_SetPubAttr failed: %d", ret);
        free(pb);
        return -1;
    }
    LOGD(__func__, "ao attr: dev=%d chn=%d in_rate=%u in_ch=%u dev_rate=%u dev_ch=%u",
         pb->dev, pb->chn, in_rate, in_channels, ZH_TTS_SAMPLE_RATE, attr.soundCard.channels);
    ret = RK_MPI_AO_Enable(pb->dev);
    if (ret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AO_Enable failed: %d", ret);
        free(pb);
        return -1;
    }
    if (in_channels == 1) {
        RK_S32 tm_ret = RK_MPI_AO_SetTrackMode(pb->dev, AUDIO_TRACK_OUT_STEREO);
        if (tm_ret != RK_SUCCESS) {
            LOGE(__func__, "RK_MPI_AO_SetTrackMode failed: %d", tm_ret);
        } else {
            LOGI(__func__, "RK_MPI_AO_SetTrackMode ok: AUDIO_TRACK_OUT_STEREO");
        }
    }
    ret = RK_MPI_AO_EnableChn(pb->dev, pb->chn);
    if (ret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AO_EnableChn failed: %d", ret);
        RK_MPI_AO_Disable(pb->dev);
        free(pb);
        return -1;
    }

    if (in_rate != ZH_TTS_SAMPLE_RATE) {
        ret = RK_MPI_AO_EnableReSmp(pb->dev, pb->chn, zh_rk_map_rate(in_rate));
        if (ret != RK_SUCCESS) {
            LOGE(__func__, "RK_MPI_AO_EnableReSmp failed: %d", ret);
        } else {
            pb->resample_enabled = 1;
            LOGI(__func__, "ao resample enabled: in_rate=%u -> dev_rate=%u", in_rate, ZH_TTS_SAMPLE_RATE);
        }
    }

    *out = pb;
    return 0;
}

int zh_ao_playback_write(zh_ao_playback_t *pb, const int16_t *pcm, size_t frames) {
    if (!pb || !pcm || frames == 0) {
        errno = EINVAL;
        return -1;
    }

    int16_t *stereo_pcm = NULL;
    const int16_t *send_pcm = pcm;
    unsigned int send_channels = pb->in_channels;
    if (pb->in_channels == 1) {
        stereo_pcm = (int16_t *)malloc(frames * 2 * sizeof(int16_t));
        if (!stereo_pcm) {
            return -1;
        }
        for (size_t i = 0; i < frames; ++i) {
            int16_t s = pcm[i];
            stereo_pcm[i * 2] = s;
            stereo_pcm[i * 2 + 1] = s;
        }
        send_pcm = stereo_pcm;
        send_channels = 2;
    }

    AUDIO_FRAME_S frame;
    memset(&frame, 0, sizeof(frame));
    frame.u32Len = (RK_U32)(frames * send_channels * sizeof(int16_t));
    frame.u64TimeStamp = 0;
    frame.s32SampleRate = (RK_S32)pb->in_rate;
    frame.enBitWidth = AUDIO_BIT_WIDTH_16;
    frame.enSoundMode = (send_channels == 1) ? AUDIO_SOUND_MODE_MONO : AUDIO_SOUND_MODE_STEREO;
    frame.bBypassMbBlk = RK_FALSE;

    MB_EXT_CONFIG_S extConfig;
    memset(&extConfig, 0, sizeof(extConfig));
    extConfig.pOpaque = (void *)send_pcm;
    extConfig.pu8VirAddr = (RK_U8 *)send_pcm;
    extConfig.u64Size = frame.u32Len;

    RK_S32 ret = RK_MPI_SYS_CreateMB(&(frame.pMbBlk), &extConfig);
    if (ret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_SYS_CreateMB failed: %d", ret);
        free(stereo_pcm);
        return -1;
    }

    ret = RK_MPI_AO_SendFrame(pb->dev, pb->chn, &frame, -1);
    RK_MPI_MB_ReleaseMB(frame.pMbBlk);
    free(stereo_pcm);
    if (ret != RK_SUCCESS) {
        LOGE(__func__, "RK_MPI_AO_SendFrame failed: %d", ret);
        return -1;
    }
    return 0;
}

void zh_ao_playback_drain(zh_ao_playback_t *pb) {
    if (!pb) return;
    RK_MPI_AO_WaitEos(pb->dev, pb->chn, 1000);
    RK_MPI_AO_ClearChnBuf(pb->dev, pb->chn);
}

void zh_ao_playback_flush(zh_ao_playback_t *pb) {
    if (!pb) return;
    RK_MPI_AO_ClearChnBuf(pb->dev, pb->chn);
}

void zh_ao_playback_close(zh_ao_playback_t *pb) {
    if (!pb) return;
    if (pb->resample_enabled) {
        RK_MPI_AO_DisableReSmp(pb->dev, pb->chn);
    }
    RK_MPI_AO_DisableChn(pb->dev, pb->chn);
    RK_MPI_AO_Disable(pb->dev);
    free(pb);
}

#endif
