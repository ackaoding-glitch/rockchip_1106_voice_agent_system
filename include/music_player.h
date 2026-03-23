#ifndef ZH_MUSIC_PLAYER_H
#define ZH_MUSIC_PLAYER_H

#include <stddef.h>

// 音乐播放模块：HTTP下载MP3 -> 解码 -> ALSA播放（独立线程）。

// 启动音乐播放线程（重复调用安全）。
int zh_music_player_start(void);
// 停止音乐播放线程并清理资源。
void zh_music_player_stop(void);
// 播放指定URL列表（新消息打断：清空队列并中止当前播放）。
void zh_music_player_play_urls(const char **urls, size_t count);
// 播放指定URL列表并应用增益（0~1更小声，>1放大）。
void zh_music_player_play_urls_with_gain(const char **urls, size_t count, float gain);
// 仅中断当前/排队音乐，不退出音乐线程。
void zh_music_player_interrupt(void);
// 当前是否处于音乐播放/排队状态。
int zh_music_player_is_active(void);

#endif
