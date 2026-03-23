#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <opus/opus.h>

#include "audio_capture.h"
#include "audio_uplink.h"
#include "bithion_core_bridge.h"
#include "config.h"
#include "face_recognition.h"
#include "log.h"
#include "music_player.h"
#include "udp_tts.h"
#include "utils.h"
#include "ws.h"

static volatile int g_uplink_running = 0;
static pthread_t g_uplink_thread;
static pthread_mutex_t g_uplink_ws_mutex = PTHREAD_MUTEX_INITIALIZER;
static zh_ws_session_t *g_uplink_ws = NULL;

#if ZH_VAD_RECORD_DUMP_ENABLE
static void zh_pre_opus_pcm_dump_write(FILE *fp, const int16_t *pcm, size_t samples) {
    if (!fp || !pcm || samples == 0) {
        return;
    }
    fwrite(pcm, sizeof(int16_t), samples, fp);
    fflush(fp);
}
#endif

static zh_ws_session_t *zh_audio_uplink_get_ws(void) {
    zh_ws_session_t *ws = NULL;

    pthread_mutex_lock(&g_uplink_ws_mutex);
    ws = g_uplink_ws;
    pthread_mutex_unlock(&g_uplink_ws_mutex);
    return ws;
}

static void zh_ws_report_asr_upload_meta(zh_ws_session_t *ws, int music_playing) {
    char msg[256];
    time_t ts = time(NULL);

    if (!ws) {
        return;
    }
    if (ts < 0) {
        ts = 0;
    }
    snprintf(msg, sizeof(msg),
             "{\"type\":\"ASR_UPLOAD_META\",\"timestamp\":%lld,"
             "\"music_playing\":%s,\"required_keyword\":\"%s\"}",
             (long long)ts,
             music_playing ? "true" : "false",
             "对话模式");
    if (zh_ws_send_str(ws, msg) != 0) {
        LOGE(__func__, "ASR_UPLOAD_META send failed");
    }
}

static int zh_audio_uplink_submit_frame(OpusEncoder *enc,
                                        const int16_t *pcm,
                                        size_t samples
#if ZH_VAD_RECORD_DUMP_ENABLE
                                        ,
                                        FILE *dump_fp
#endif
                                        ) {
    int16_t mono[ZH_OPUS_FRAME_SAMPLES];
    uint8_t opus_buf[4000];
    int opus_len = 0;

    if (!enc || !pcm || samples == 0) {
        return -1;
    }
    if (samples < ZH_OPUS_FRAME_SAMPLES * ZH_AUDIO_CHANNELS) {
        return -1;
    }
    for (size_t i = 0; i < ZH_OPUS_FRAME_SAMPLES; ++i) {
        mono[i] = pcm[i * ZH_AUDIO_CHANNELS];
    }
#if ZH_VAD_RECORD_DUMP_ENABLE
    zh_pre_opus_pcm_dump_write(dump_fp, mono, ZH_OPUS_FRAME_SAMPLES);
#endif
    opus_len = opus_encode(enc, mono, ZH_OPUS_FRAME_SAMPLES, opus_buf, (opus_int32)sizeof(opus_buf));
    if (opus_len < 0) {
        LOGE(__func__, "opus encode failed: %d", opus_len);
        return -1;
    }
    return zh_core_uplink_send_opus(opus_buf, (size_t)opus_len);
}

static int zh_audio_uplink_flush_preroll(OpusEncoder *enc,
                                         const int16_t *preroll,
                                         size_t frame_len,
                                         size_t frames,
                                         size_t pos,
                                         int full
#if ZH_VAD_RECORD_DUMP_ENABLE
                                         ,
                                         FILE *dump_fp
#endif
                                         ) {
    if (!enc || !preroll || frame_len == 0 || frames == 0) {
        return -1;
    }
    if (!full) {
        for (size_t i = 0; i < pos; ++i) {
            if (zh_audio_uplink_submit_frame(enc, preroll + i * frame_len, frame_len
#if ZH_VAD_RECORD_DUMP_ENABLE
                                             ,
                                             dump_fp
#endif
                ) != 0) {
                return -1;
            }
        }
        return 0;
    }
    for (size_t i = pos; i < frames; ++i) {
        if (zh_audio_uplink_submit_frame(enc, preroll + i * frame_len, frame_len
#if ZH_VAD_RECORD_DUMP_ENABLE
                                         ,
                                         dump_fp
#endif
            ) != 0) {
            return -1;
        }
    }
    for (size_t i = 0; i < pos; ++i) {
        if (zh_audio_uplink_submit_frame(enc, preroll + i * frame_len, frame_len
#if ZH_VAD_RECORD_DUMP_ENABLE
                                         ,
                                         dump_fp
#endif
            ) != 0) {
            return -1;
        }
    }
    return 0;
}

static void *zh_audio_uplink_thread_main(void *arg) {
    zh_audio_capture_t *cap = NULL;
    zh_core_vad_t *vad = NULL;
    OpusEncoder *enc = NULL;
    int16_t *frame = NULL;
    int16_t preroll[ZH_VAD_PREROLL_FRAMES][ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS];
    size_t preroll_pos = 0;
    int preroll_full = 0;
    int speech_active = 0;
    int stt_completed_seen = 0;
    int music_state_last_reported = -1;
    unsigned int actual_rate = 0;
    unsigned int actual_channels = 0;
    int opus_err = 0;
#if ZH_VAD_RECORD_DUMP_ENABLE
    FILE *pre_opus_dump_fp = NULL;
#endif

    (void)arg;

    if (zh_audio_capture_init(&cap, &actual_rate, &actual_channels) != 0) {
        goto cleanup;
    }
    if (zh_audio_capture_start(cap) != 0) {
        goto cleanup;
    }
    if (actual_rate != 0 && actual_channels != 0) {
        LOGI(__func__, "pcm params: rate=%u channels=%u", actual_rate, actual_channels);
    }

    vad = zh_core_vad_create();
    if (!vad) {
        LOGE(__func__, "core vad create failed");
        goto cleanup;
    }

    enc = opus_encoder_create(ZH_OPUS_SAMPLE_RATE, ZH_OPUS_CHANNELS, OPUS_APPLICATION_AUDIO, &opus_err);
    if (!enc || opus_err != OPUS_OK) {
        LOGE(__func__, "opus encoder create failed: %d", opus_err);
        goto cleanup;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(16000));

    frame = (int16_t *)calloc(ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS, sizeof(int16_t));
    if (!frame) {
        LOGE(__func__, "frame alloc failed");
        goto cleanup;
    }

#if ZH_VAD_RECORD_DUMP_ENABLE
    pre_opus_dump_fp = fopen(ZH_VAD_RECORD_DUMP_PATH, "ab");
    if (!pre_opus_dump_fp) {
        LOGE(__func__, "pre-opus pcm dump open failed: %s", strerror(errno));
    }
#endif

    LOGI(__func__, "audio uplink thread started");
    while (g_uplink_running) {
        zh_ws_session_t *ws = NULL;
        zh_core_vad_result_t result;
        int got = 0;
        int was_speech_active = 0;
        int frame_flushed_in_preroll = 0;

        got = zh_audio_capture_read(cap, frame, ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS);
        if (got < 0) {
            if (got == -EPIPE) {
                continue;
            }
            usleep(10000);
            continue;
        }
        if (got != (int)(ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS)) {
            continue;
        }

        ws = zh_audio_uplink_get_ws();
        {
            int music_active = zh_music_player_is_active() ? 1 : 0;
            if (music_state_last_reported < 0) {
                music_state_last_reported = music_active;
            } else if (music_state_last_reported != music_active) {
                zh_ws_report_asr_upload_meta(ws, music_active);
                music_state_last_reported = music_active;
            }
        }

        if (ws && zh_ws_is_stt_completed(ws)) {
            if (!stt_completed_seen) {
                stt_completed_seen = 1;
                speech_active = 0;
                preroll_pos = 0;
                preroll_full = 0;
                zh_face_recognition_set_active(0);
                zh_core_vad_reset(vad);
                zh_core_uplink_reset();
            }
        } else if (stt_completed_seen) {
            stt_completed_seen = 0;
        }

        was_speech_active = speech_active;
        if (zh_core_vad_process(vad, frame, ZH_AUDIO_FRAME_SAMPLES, ZH_AUDIO_CHANNELS, &result) != 0) {
            continue;
        }
        speech_active = result.speech_active;

        for (size_t i = 0; i < ZH_AUDIO_FRAME_SAMPLES; ++i) {
            for (size_t ch = 0; ch < ZH_AUDIO_CHANNELS; ++ch) {
                size_t idx = i * ZH_AUDIO_CHANNELS + ch;
                frame[idx] = zh_apply_gain(frame[idx], ZH_AUDIO_GAIN);
            }
        }

        if (!was_speech_active) {
            memcpy(preroll[preroll_pos],
                   frame,
                   sizeof(int16_t) * ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS);
            preroll_pos = (preroll_pos + 1) % ZH_VAD_PREROLL_FRAMES;
            if (preroll_pos == 0) {
                preroll_full = 1;
            }
        }

        if (result.speech_started) {
            zh_face_recognition_set_active(1);
            if (ws) {
                if (zh_core_uplink_begin_segment() != 0) {
                    LOGE(__func__, "uplink START send failed");
                } else {
                    if (zh_audio_uplink_flush_preroll(enc,
                                                      &preroll[0][0],
                                                      ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS,
                                                      ZH_VAD_PREROLL_FRAMES,
                                                      preroll_pos,
                                                      preroll_full
#if ZH_VAD_RECORD_DUMP_ENABLE
                                                      ,
                                                      pre_opus_dump_fp
#endif
                        ) == 0) {
                        frame_flushed_in_preroll = 1;
                    }
                }
            }
        }

        if (speech_active && ws && (!zh_ws_is_stt_completed(ws) || zh_udp_tts_is_playing())) {
            if (!frame_flushed_in_preroll) {
                (void)zh_audio_uplink_submit_frame(enc,
                                                   frame,
                                                   ZH_AUDIO_FRAME_SAMPLES * ZH_AUDIO_CHANNELS
#if ZH_VAD_RECORD_DUMP_ENABLE
                                                   ,
                                                   pre_opus_dump_fp
#endif
                );
            }
        }

        if (result.speech_ended) {
            zh_face_recognition_set_active(0);
            zh_core_uplink_flush();
        }
    }

cleanup:
    if (frame) {
        free(frame);
    }
    if (enc) {
        opus_encoder_destroy(enc);
    }
#if ZH_VAD_RECORD_DUMP_ENABLE
    if (pre_opus_dump_fp) {
        fclose(pre_opus_dump_fp);
    }
#endif
    zh_core_vad_destroy(vad);
    zh_audio_capture_stop(cap);
    zh_audio_capture_deinit(cap);
    g_uplink_running = 0;
    LOGI(__func__, "audio uplink thread exit");
    return NULL;
}

int zh_audio_uplink_preload(void) {
    return zh_core_vad_preload();
}

int zh_audio_uplink_start(void) {
    int err = 0;

    if (g_uplink_running) {
        return 0;
    }
    if (zh_core_uplink_start() != 0) {
        return -1;
    }
    g_uplink_running = 1;
    err = pthread_create(&g_uplink_thread, NULL, zh_audio_uplink_thread_main, NULL);
    if (err != 0) {
        g_uplink_running = 0;
        zh_core_uplink_stop();
        errno = err;
        return -1;
    }
    return 0;
}

void zh_audio_uplink_stop(void) {
    if (!g_uplink_running) {
        zh_core_uplink_stop();
        return;
    }
    g_uplink_running = 0;
    pthread_join(g_uplink_thread, NULL);
    zh_core_uplink_stop();
}

void zh_audio_uplink_set_ws(zh_ws_session_t *ws) {
    pthread_mutex_lock(&g_uplink_ws_mutex);
    g_uplink_ws = ws;
    pthread_mutex_unlock(&g_uplink_ws_mutex);
}
