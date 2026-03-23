#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "bithion_core.h"
#include "audio_playback_rockit.h"
#include "log.h"
#include "music_player.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

// 音乐播放模块：HTTP下载MP3 -> 解码 -> ALSA播放（独立线程）。

typedef struct zh_music_item {
    char *url;
    float gain;
    struct zh_music_item *next;
} zh_music_item_t;

typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    zh_music_item_t *head;
    zh_music_item_t *tail;
    zh_music_item_t *pending_head;
    zh_music_item_t *pending_tail;
    int running;
    int stop;
    int playing;
    uint64_t token;
} zh_music_player_t;

static zh_music_player_t g_music = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};
static int g_music_stream_running = 0;

static void zh_music_clear_locked(zh_music_player_t *p) {
    while (p->head) {
        zh_music_item_t *item = p->head;
        p->head = item->next;
        free(item->url);
        free(item);
    }
    p->tail = NULL;
    while (p->pending_head) {
        zh_music_item_t *item = p->pending_head;
        p->pending_head = item->next;
        free(item->url);
        free(item);
    }
    p->pending_tail = NULL;
}

static int zh_music_should_abort(uint64_t token) {
    int abort = 0;
    pthread_mutex_lock(&g_music.mutex);
    abort = g_music.stop || g_music.token != token;
    pthread_mutex_unlock(&g_music.mutex);
    return abort;
}

typedef struct {
    mp3dec_t dec;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int16_t *resample_pcm;
    size_t resample_cap_frames;
    uint8_t *buf;
    size_t len;
    size_t cap;
    zh_ao_playback_t *play;
    int opened;
    unsigned int play_rate;
    int channels;
} zh_music_decoder_t;

typedef struct {
    zh_music_decoder_t *decoder;
    uint64_t token;
    float gain;
} zh_music_stream_feed_ctx_t;

static int zh_music_play_pcm(zh_ao_playback_t *pb, const int16_t *pcm_data,
                             size_t frames, uint64_t token);

static int zh_music_is_url(const char *s) {
    if (!s) return 0;
    return (strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0);
}

static int16_t zh_music_apply_gain_sample(int16_t sample, float gain) {
    int32_t v = (int32_t)((float)sample * gain);
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static size_t zh_music_resample_linear(const int16_t *in_pcm, size_t in_frames,
                                       unsigned int in_rate, int channels,
                                       int16_t *out_pcm, size_t out_cap_frames,
                                       unsigned int out_rate) {
    if (!in_pcm || !out_pcm || in_frames == 0 || out_cap_frames == 0 || channels <= 0 ||
        in_rate == 0 || out_rate == 0) {
        return 0;
    }
    if (in_rate == out_rate) {
        size_t n = in_frames < out_cap_frames ? in_frames : out_cap_frames;
        memcpy(out_pcm, in_pcm, n * (size_t)channels * sizeof(int16_t));
        return n;
    }

    const double step = (double)in_rate / (double)out_rate;
    double pos = 0.0;
    size_t out_frames = 0;
    while (out_frames < out_cap_frames) {
        size_t i0 = (size_t)pos;
        if (i0 >= in_frames) {
            break;
        }
        size_t i1 = (i0 + 1 < in_frames) ? (i0 + 1) : i0;
        double frac = pos - (double)i0;
        for (int ch = 0; ch < channels; ++ch) {
            int32_t s0 = in_pcm[i0 * (size_t)channels + (size_t)ch];
            int32_t s1 = in_pcm[i1 * (size_t)channels + (size_t)ch];
            int32_t mixed = (int32_t)((1.0 - frac) * (double)s0 + frac * (double)s1);
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            out_pcm[out_frames * (size_t)channels + (size_t)ch] = (int16_t)mixed;
        }
        out_frames++;
        pos += step;
    }
    return out_frames;
}

static void zh_music_decoder_reset(zh_music_decoder_t *d) {
    if (!d) return;
    if (d->play) {
        zh_ao_playback_close(d->play);
        d->play = NULL;
    }
    if (d->buf) {
        free(d->buf);
        d->buf = NULL;
    }
    if (d->resample_pcm) {
        free(d->resample_pcm);
        d->resample_pcm = NULL;
    }
    d->len = 0;
    d->cap = 0;
    d->resample_cap_frames = 0;
    d->opened = 0;
    d->play_rate = 0;
    d->channels = 0;
}

#define ZH_MUSIC_PCM_OPEN_RETRY_MS 200
#define ZH_MUSIC_PCM_OPEN_TIMEOUT_MS 10000

static int zh_music_decoder_open(zh_music_decoder_t *d, int ch, unsigned int rate,
                                 uint64_t token) {
    if (!d) return -1;
    int tries = ZH_MUSIC_PCM_OPEN_TIMEOUT_MS / ZH_MUSIC_PCM_OPEN_RETRY_MS;
    while (tries-- > 0) {
        if (zh_music_should_abort(token)) {
            return -1;
        }
        if (zh_ao_playback_open(&d->play, rate, (unsigned int)ch) == 0) {
            d->opened = 1;
            return 0;
        }
        usleep(ZH_MUSIC_PCM_OPEN_RETRY_MS * 1000);
    }
    LOGE(__func__, "ao playback open failed");
    return -1;
}

static int zh_music_decoder_feed(zh_music_decoder_t *d, const uint8_t *data, size_t len,
                                 uint64_t token, float gain) {
    if (!d || !data || len == 0) return 0;
    if (d->len + len > d->cap) {
        size_t new_cap = d->cap == 0 ? 8192 : d->cap * 2;
        while (new_cap < d->len + len) {
            new_cap *= 2;
        }
        uint8_t *tmp = (uint8_t *)realloc(d->buf, new_cap);
        if (!tmp) return -1;
        d->buf = tmp;
        d->cap = new_cap;
    }
    memcpy(d->buf + d->len, data, len);
    d->len += len;

    size_t offset = 0;
    mp3dec_frame_info_t info;
    while (offset < d->len) {
        if (zh_music_should_abort(token)) {
            return -1;
        }
        int samples = mp3dec_decode_frame(&d->dec, d->buf + offset, d->len - offset,
                                          d->pcm, &info);
        if (info.frame_bytes == 0) {
            break;
        }
        offset += (size_t)info.frame_bytes;
        if (samples <= 0) {
            continue;
        }
        int ch = info.channels > 0 ? info.channels : 1;
        unsigned int rate = info.hz > 0 ? (unsigned int)info.hz : ZH_TTS_SAMPLE_RATE;
        unsigned int out_rate = (rate == ZH_TTS_SAMPLE_RATE) ? rate : ZH_TTS_SAMPLE_RATE;
        if (!d->opened) {
            if (zh_music_decoder_open(d, ch, out_rate, token) != 0) {
                return -1;
            }
            d->play_rate = out_rate;
            d->channels = ch;
        }

        const int16_t *write_pcm = d->pcm;
        size_t write_frames = (size_t)samples;
        if (rate != d->play_rate) {
            size_t need_frames =
                ((size_t)samples * (size_t)d->play_rate + (size_t)rate - 1) / (size_t)rate + 2;
            if (need_frames > d->resample_cap_frames) {
                int16_t *tmp = (int16_t *)realloc(
                    d->resample_pcm, need_frames * (size_t)d->channels * sizeof(int16_t));
                if (!tmp) {
                    return -1;
                }
                d->resample_pcm = tmp;
                d->resample_cap_frames = need_frames;
            }
            write_frames = zh_music_resample_linear(d->pcm, (size_t)samples, rate, d->channels,
                                                    d->resample_pcm, d->resample_cap_frames,
                                                    d->play_rate);
            write_pcm = d->resample_pcm;
            if (write_frames == 0) {
                continue;
            }
        }
        if (gain != 1.0f) {
            int16_t *mutable_pcm = (int16_t *)write_pcm;
            size_t total = write_frames * (size_t)d->channels;
            for (size_t i = 0; i < total; ++i) {
                mutable_pcm[i] = zh_music_apply_gain_sample(mutable_pcm[i], gain);
            }
        }

        if (zh_music_play_pcm(d->play, write_pcm, write_frames, token) != 0) {
            return -1;
        }
    }

    if (offset > 0) {
        size_t remain = d->len - offset;
        if (remain > 0) {
            memmove(d->buf, d->buf + offset, remain);
        }
        d->len = remain;
    }
    return 0;
}

static void zh_music_decoder_finish(zh_music_decoder_t *d, int interrupted) {
    if (!d) return;
    if (d->play && !interrupted) {
        zh_ao_playback_drain(d->play);
    }
    if (d->play && interrupted) {
        zh_ao_playback_flush(d->play);
    }
    zh_music_decoder_reset(d);
}

static int zh_music_http_stream_on_data(void *userdata, const void *data, size_t len) {
    zh_music_stream_feed_ctx_t *ctx = (zh_music_stream_feed_ctx_t *)userdata;

    if (!ctx || !ctx->decoder || !data) {
        return -1;
    }
    return zh_music_decoder_feed(ctx->decoder, (const uint8_t *)data, len, ctx->token, ctx->gain);
}

static int zh_music_http_stream_play(const char *url, uint64_t token, float gain) {
    zh_music_decoder_t dec;
    zh_music_stream_feed_ctx_t ctx;
    int status = 0;
    int rc = -1;
    int interrupted = 0;

    if (!url) {
        return -1;
    }
    if (zh_music_should_abort(token)) {
        return -1;
    }

    memset(&dec, 0, sizeof(dec));
    memset(&ctx, 0, sizeof(ctx));
    ctx.decoder = &dec;
    ctx.token = token;
    ctx.gain = gain;
    mp3dec_init(&dec.dec);

    rc = bithion_core_http_get_stream(url,
                                      1,
                                      zh_music_http_stream_on_data,
                                      &ctx,
                                      &g_music_stream_running,
                                      &status);
    interrupted = zh_music_should_abort(token) || !g_music_stream_running;
    zh_music_decoder_finish(&dec, interrupted);

    if (rc != 0) {
        return -1;
    }
    if (status != 200) {
        LOGE(__func__, "music http status=%d url=%s", status, url);
        return -1;
    }
    return 0;
}

static int zh_music_play_pcm(zh_ao_playback_t *pb, const int16_t *pcm_data,
                             size_t frames, uint64_t token) {
    size_t offset = 0;
    if (!pb || !pcm_data || frames == 0) {
        return -1;
    }
    while (offset < frames) {
        if (zh_music_should_abort(token)) {
            return -1;
        }
        size_t chunk = frames - offset;
        if (zh_ao_playback_write(pb, pcm_data + offset, chunk) != 0) {
            return -1;
        }
        offset += chunk;
    }
    return 0;
}

static int zh_music_play_mp3(const uint8_t *data, size_t len, uint64_t token, float gain) {
    mp3dec_t dec;
    mp3dec_frame_info_t info;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t offset = 0;
    zh_ao_playback_t *play = NULL;
    int opened = 0;
    size_t decoded_frames = 0;
    int first_frame_logged = 0;

    if (!data || len == 0) {
        return -1;
    }

    mp3dec_init(&dec);
    while (offset < len) {
        int samples = mp3dec_decode_frame(&dec, data + offset, len - offset, pcm, &info);
        if (info.frame_bytes == 0) {
            break;
        }
        offset += (size_t)info.frame_bytes;
        if (samples <= 0) {
            continue;
        }
        int ch = info.channels > 0 ? info.channels : 1;
        if (!first_frame_logged) {
            LOGI(__func__, "mp3 first frame: hz=%d ch=%d samples=%d",
                 info.hz, ch, samples);
            first_frame_logged = 1;
        }
        if (!opened) {
            unsigned int rate = info.hz > 0 ? (unsigned int)info.hz : ZH_TTS_SAMPLE_RATE;
            if (zh_ao_playback_open(&play, rate, (unsigned int)ch) != 0) {
                LOGE(__func__, "ao playback open failed");
                return -1;
            }
            opened = 1;
        }
        if (gain != 1.0f) {
            size_t total = (size_t)samples * (size_t)ch;
            for (size_t i = 0; i < total; ++i) {
                pcm[i] = zh_music_apply_gain_sample(pcm[i], gain);
            }
        }
        if (zh_music_play_pcm(play, pcm, (size_t)samples, token) != 0) {
            break;
        }
        decoded_frames += (size_t)samples;
    }

    if (decoded_frames == 0) {
        LOGE(__func__, "mp3 decode got zero pcm frames");
        if (play) {
            zh_ao_playback_close(play);
        }
        return -1;
    }

    if (play) {
        zh_ao_playback_drain(play);
        zh_ao_playback_close(play);
    }
    LOGI(__func__, "mp3 play done: decoded_frames=%zu", decoded_frames);
    return 0;
}

static int zh_music_play_local_file(const char *path, uint64_t token, float gain) {
    FILE *fp = NULL;
    uint8_t buf[4096];
    zh_music_decoder_t dec;
    size_t n = 0;
    int rc = -1;
    int interrupted = 0;

    if (!path || path[0] == '\0') {
        return -1;
    }
    if (zh_music_should_abort(token)) {
        return -1;
    }
    if (access(path, R_OK) != 0) {
        LOGE(__func__, "local mp3 not readable: %s", path);
        return -1;
    }

    fp = fopen(path, "rb");
    if (!fp) {
        LOGE(__func__, "open local mp3 failed: %s", path);
        return -1;
    }
    memset(&dec, 0, sizeof(dec));
    mp3dec_init(&dec.dec);
    LOGI(__func__, "music local file: %s", path);
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (zh_music_should_abort(token)) {
            interrupted = 1;
            break;
        }
        if (zh_music_decoder_feed(&dec, buf, n, token, gain) != 0) {
            goto done;
        }
    }
    if (ferror(fp)) {
        goto done;
    }
    rc = interrupted ? -1 : 0;

done:
    zh_music_decoder_finish(&dec, interrupted);
    if (fp) fclose(fp);
    if (rc != 0) {
        LOGE(__func__, "music local file play failed: %s", path);
    }
    return rc;
}

static void *zh_music_thread_main(void *arg) {
    (void)arg;
    while (1) {
        zh_music_item_t *item = NULL;
        uint64_t token = 0;

        pthread_mutex_lock(&g_music.mutex);
        while (!g_music.stop && !g_music.head) {
            if (g_music.pending_head) {
                g_music.head = g_music.pending_head;
                g_music.pending_head = g_music.pending_head->next;
                if (!g_music.pending_head) g_music.pending_tail = NULL;
                g_music.head->next = NULL;
                g_music.tail = g_music.head;
                break;
            }
            pthread_cond_wait(&g_music.cond, &g_music.mutex);
        }
        if (g_music.stop) {
            pthread_mutex_unlock(&g_music.mutex);
            break;
        }
        item = g_music.head;
        if (item) {
            g_music.head = item->next;
            if (!g_music.head) g_music.tail = NULL;
        }
        token = g_music.token;
        g_music.playing = item ? 1 : 0;
        g_music_stream_running = item && zh_music_is_url(item->url);
        pthread_mutex_unlock(&g_music.mutex);

        if (!item) {
            continue;
        }

        if (!zh_music_should_abort(token)) {
            if (zh_music_is_url(item->url)) {
                LOGI(__func__, "music stream: %s", item->url);
                if (zh_music_http_stream_play(item->url, token, item->gain) != 0) {
                    LOGE(__func__, "music fetch failed: %s", item->url);
                }
            } else if (zh_music_play_local_file(item->url, token, item->gain) != 0) {
                LOGE(__func__, "music play failed: %s", item->url);
            }
        }

        free(item->url);
        free(item);

        pthread_mutex_lock(&g_music.mutex);
        g_music_stream_running = 0;
        if (!g_music.head) {
            if (g_music.pending_head) {
                g_music.head = g_music.pending_head;
                g_music.pending_head = g_music.pending_head->next;
                if (!g_music.pending_head) g_music.pending_tail = NULL;
                g_music.head->next = NULL;
                g_music.tail = g_music.head;
                pthread_cond_broadcast(&g_music.cond);
            }
            g_music.playing = 0;
        }
        pthread_mutex_unlock(&g_music.mutex);
    }
    return NULL;
}

int zh_music_player_start(void) {
    pthread_mutex_lock(&g_music.mutex);
    if (g_music.running) {
        pthread_mutex_unlock(&g_music.mutex);
        return 0;
    }
    g_music.stop = 0;
    g_music.running = 1;
    pthread_mutex_unlock(&g_music.mutex);

    int err = pthread_create(&g_music.thread, NULL, zh_music_thread_main, NULL);
    if (err != 0) {
        pthread_mutex_lock(&g_music.mutex);
        g_music.running = 0;
        pthread_mutex_unlock(&g_music.mutex);
        errno = err;
        return -1;
    }
    pthread_detach(g_music.thread);
    return 0;
}

void zh_music_player_stop(void) {
    pthread_mutex_lock(&g_music.mutex);
    g_music.stop = 1;
    g_music_stream_running = 0;
    zh_music_clear_locked(&g_music);
    pthread_cond_broadcast(&g_music.cond);
    pthread_mutex_unlock(&g_music.mutex);
}

void zh_music_player_play_urls_with_gain(const char **urls, size_t count, float gain) {
    if (!urls || count == 0) return;
    if (gain < 0.0f) gain = 0.0f;

    pthread_mutex_lock(&g_music.mutex);
    g_music.token++;
    g_music_stream_running = 0;
    zh_music_clear_locked(&g_music);
    for (size_t i = 0; i < count; ++i) {
        const char *u = urls[i];
        if (!u || !u[0]) continue;
        zh_music_item_t *item = (zh_music_item_t *)calloc(1, sizeof(*item));
        if (!item) continue;
        item->url = strdup(u);
        if (!item->url) {
            free(item);
            continue;
        }
        item->gain = gain;
        if (i == 0 && !g_music.head) {
            g_music.head = g_music.tail = item;
        } else {
            if (!g_music.pending_tail) {
                g_music.pending_head = g_music.pending_tail = item;
            } else {
                g_music.pending_tail->next = item;
                g_music.pending_tail = item;
            }
        }
    }
    pthread_cond_broadcast(&g_music.cond);
    pthread_mutex_unlock(&g_music.mutex);
}

void zh_music_player_play_urls(const char **urls, size_t count) {
    zh_music_player_play_urls_with_gain(urls, count, 1.0f);
}

void zh_music_player_interrupt(void) {
    pthread_mutex_lock(&g_music.mutex);
    g_music.token++;
    g_music_stream_running = 0;
    zh_music_clear_locked(&g_music);
    g_music.playing = 0;
    pthread_cond_broadcast(&g_music.cond);
    pthread_mutex_unlock(&g_music.mutex);
}

int zh_music_player_is_active(void) {
    int active = 0;
    pthread_mutex_lock(&g_music.mutex);
    active = g_music.playing || g_music.head != NULL || g_music.pending_head != NULL;
    pthread_mutex_unlock(&g_music.mutex);
    return active;
}
