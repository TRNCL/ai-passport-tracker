// main/tracker_player.h —— Tracker 播放器核心引擎
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define TRACKER_MAX_CHANNELS 4
#define TRACKER_VISIBLE_ROWS 5
#define TRACKER_ROW_CENTER   2

typedef struct {
    int row_index;                                      // 实际行号 0..63 (-1 为无效)
    char channel_text[TRACKER_MAX_CHANNELS][8];         // 例如 "C-2 01", "--- .."
} tracker_row_info_t;

typedef struct {
    bool is_playing;
    bool is_loaded;
    int track_index;
    int total_tracks;
    const char *title;
    const char *artist;
    const char *format;
    int num_channels;                       // 实际通道数 (例如 4 或 8)
    int pattern;
    int num_patterns;
    int row;                                // 当前行 (0..63)
    int speed;                              // ticks per line
    int bpm;                                // 估算 BPM
    uint8_t volume;                         // 0..100
    uint8_t vu[TRACKER_MAX_CHANNELS];       // 各通道 0..100 电平
    char note_str[TRACKER_MAX_CHANNELS][8]; // 各通道当前音符，例如 "C-2"
    uint8_t sample_num[TRACKER_MAX_CHANNELS];// 各通道当前乐器号
    tracker_row_info_t rows[TRACKER_VISIBLE_ROWS]; // 9 行可见铺面数据快照
} tracker_status_t;

// 初始化播放器
bool tracker_player_init(uint32_t sample_rate);

// 销毁播放器及停止后台音频任务
void tracker_player_deinit(void);

// 载入指定曲目（不自动播放，处于待命就绪状态）
bool tracker_player_load_track(size_t index);

// 载入并播放指定曲目
bool tracker_player_play_track(size_t index);

// 播放 / 暂停切换
void tracker_player_toggle_pause(void);

// 上一曲 / 下一曲
void tracker_player_next_track(void);
void tracker_player_prev_track(void);

// 音量设置 (0..100)
void tracker_player_set_volume(uint8_t vol);
uint8_t tracker_player_get_volume(void);

// 线程安全获取播放器状态快照（供 UI 周期性刷新）
void tracker_player_get_status(tracker_status_t *status);
