// main/main.c —— FoloToy AI Passport Tracker 独立随身听入口
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_pins.h"
#include "demo_tracker.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const char *TAG = "main";

// 按键回调运行在 button 组件任务中，操作 LVGL 加锁后直接分发给 Tracker 引擎
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!bsp_lvgl_lock(500)) return;
    demo_tracker_key(btn, ev);
    bsp_lvgl_unlock();
}

#include "tracker_tracks.h"
#include "tracker_player.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

static void print_serial_help(void) {
    printf("\n=== Tracker Player 控制命令 ===\n"
           " list / ls          - 列出曲库所有曲目及当前播放位置\n"
           " <ID> / track <ID>  - 切换至指定编号曲目（待命就绪，不自动播放）\n"
           " play [ID]          - 开始播放当前曲目，或切换并立即播放指定曲目\n"
           " pause / stop       - 暂停当前播放\n"
           " next / n           - 下一首（不自动播放）\n"
           " prev / p           - 上一首（不自动播放）\n"
           " vol [0-100] / + / -- 设置或查看音量\n"
           " status / s         - 查看播放器实时状态快照\n"
           " screen / disp      - 切换 息屏 / 亮屏 状态\n"
           " help / ?           - 显示命令帮助\n"
           " [空格键]           - 快速切换播放/暂停\n"
           "================================\n\n");
}

static void process_serial_cmd(char *cmd) {
    while (*cmd == ' ' || *cmd == '\t' || *cmd == '\r' || *cmd == '\n') cmd++;
    int len = strlen(cmd);
    while (len > 0 && (cmd[len - 1] == ' ' || cmd[len - 1] == '\t' || cmd[len - 1] == '\r' || cmd[len - 1] == '\n')) {
        cmd[--len] = '\0';
    }
    if (len == 0) return;

    // 1. 检查是否为纯数字（例如直接输入 0、3、12 快速切歌）
    bool is_num = true;
    for (int i = 0; i < len; i++) {
        if (cmd[i] < '0' || cmd[i] > '9') {
            is_num = false;
            break;
        }
    }
    if (is_num) {
        int track = atoi(cmd);
        size_t count = tracker_tracks_count();
        if (track >= 0 && (size_t)track < count) {
            tracker_status_t st;
            tracker_player_get_status(&st);
            bool was_playing = st.is_playing;
            if (was_playing) {
                tracker_player_play_track(track);
            } else {
                tracker_player_load_track(track);
            }
            const tracker_track_t *t = tracker_tracks_get(track);
            ESP_LOGI(TAG, "已切换曲目: [%d/%u] %s (%s) [%s]",
                     track, (unsigned)count, t ? t->title : "", t ? t->format : "",
                     was_playing ? "播放中" : "暂停待命");
        } else {
            ESP_LOGW(TAG, "曲目编号 %d 无效，当前有效范围: 0 ~ %u", track, (unsigned)count - 1);
        }
        return;
    }

    char *arg = strchr(cmd, ' ');
    if (arg) {
        *arg = '\0';
        arg++;
        while (*arg == ' ') arg++;
    }

    if (strcasecmp(cmd, "list") == 0 || strcasecmp(cmd, "ls") == 0) {
        size_t total = tracker_tracks_count();
        tracker_status_t cur_st;
        tracker_player_get_status(&cur_st);
        printf("\n=== 曲库曲目列表 (共 %u 首) ===\n", (unsigned)total);
        for (size_t i = 0; i < total; i++) {
            const tracker_track_t *t = tracker_tracks_get(i);
            if (t) {
                printf(" [%u] %-24s (%s, %u KB)%s\n",
                       (unsigned)i, t->title, t->format,
                       (unsigned)((t->size + 1023) / 1024),
                       (i == (size_t)cur_st.track_index) ? "  <-- 当前" : "");
            }
        }
        printf("================================\n\n");
    } else if (strcasecmp(cmd, "track") == 0 || strcasecmp(cmd, "t") == 0 || strcasecmp(cmd, "select") == 0) {
        if (!arg || *arg == '\0') {
            ESP_LOGW(TAG, "用法: track <曲目ID> (例如: track 0)");
            return;
        }
        int track = atoi(arg);
        size_t count = tracker_tracks_count();
        if (track >= 0 && (size_t)track < count) {
            tracker_status_t st;
            tracker_player_get_status(&st);
            bool was_playing = st.is_playing;
            if (was_playing) {
                tracker_player_play_track(track);
            } else {
                tracker_player_load_track(track);
            }
            const tracker_track_t *t = tracker_tracks_get(track);
            ESP_LOGI(TAG, "已切换曲目: [%d/%u] %s (%s) [%s]",
                     track, (unsigned)count, t ? t->title : "", t ? t->format : "",
                     was_playing ? "播放中" : "暂停待命");
        } else {
            ESP_LOGW(TAG, "曲目编号 %d 无效，当前有效范围: 0 ~ %u", track, (unsigned)count - 1);
        }
    } else if (strcasecmp(cmd, "play") == 0) {
        if (arg && *arg != '\0') {
            int track = atoi(arg);
            size_t count = tracker_tracks_count();
            if (track >= 0 && (size_t)track < count) {
                tracker_player_play_track(track);
                const tracker_track_t *t = tracker_tracks_get(track);
                ESP_LOGI(TAG, "开始播放曲目: [%d/%u] %s (%s)",
                         track, (unsigned)count, t ? t->title : "", t ? t->format : "");
            } else {
                ESP_LOGW(TAG, "曲目编号 %d 无效，当前有效范围: 0 ~ %u", track, (unsigned)count - 1);
            }
        } else {
            tracker_status_t st;
            tracker_player_get_status(&st);
            if (!st.is_playing) {
                tracker_player_toggle_pause();
            }
            ESP_LOGI(TAG, "播放状态: [PLAY 播放中]");
        }
    } else if (strcasecmp(cmd, "pause") == 0 || strcasecmp(cmd, "stop") == 0) {
        tracker_status_t st;
        tracker_player_get_status(&st);
        if (st.is_playing) {
            tracker_player_toggle_pause();
        }
        ESP_LOGI(TAG, "播放状态: [PAUSE 已暂停]");
    } else if (strcasecmp(cmd, "next") == 0 || strcasecmp(cmd, "n") == 0) {
        tracker_player_next_track();
        tracker_status_t st;
        tracker_player_get_status(&st);
        ESP_LOGI(TAG, "下一首: [%d/%d] %s (%s) [%s]",
                 st.track_index, st.total_tracks, st.title, st.format,
                 st.is_playing ? "播放中" : "暂停待命");
    } else if (strcasecmp(cmd, "prev") == 0 || strcasecmp(cmd, "p") == 0) {
        tracker_player_prev_track();
        tracker_status_t st;
        tracker_player_get_status(&st);
        ESP_LOGI(TAG, "上一首: [%d/%d] %s (%s) [%s]",
                 st.track_index, st.total_tracks, st.title, st.format,
                 st.is_playing ? "播放中" : "暂停待命");
    } else if (strcasecmp(cmd, "vol") == 0) {
        if (arg && *arg != '\0') {
            if (*arg == '+') {
                uint8_t v = tracker_player_get_volume();
                tracker_player_set_volume(v <= 95 ? v + 5 : 100);
            } else if (*arg == '-') {
                uint8_t v = tracker_player_get_volume();
                tracker_player_set_volume(v >= 5 ? v - 5 : 0);
            } else {
                int val = atoi(arg);
                if (val < 0) val = 0;
                if (val > 100) val = 100;
                tracker_player_set_volume(val);
            }
        }
        ESP_LOGI(TAG, "当前音量: %d%%", tracker_player_get_volume());
    } else if (strcmp(cmd, "+") == 0) {
        uint8_t v = tracker_player_get_volume();
        tracker_player_set_volume(v <= 95 ? v + 5 : 100);
        ESP_LOGI(TAG, "音量: %d%%", tracker_player_get_volume());
    } else if (strcmp(cmd, "-") == 0) {
        uint8_t v = tracker_player_get_volume();
        tracker_player_set_volume(v >= 5 ? v - 5 : 0);
        ESP_LOGI(TAG, "音量: %d%%", tracker_player_get_volume());
    } else if (strcasecmp(cmd, "status") == 0 || strcasecmp(cmd, "s") == 0) {
        tracker_status_t st;
        tracker_player_get_status(&st);
        bool is_off = demo_tracker_is_screen_off();
        ESP_LOGI(TAG, "状态快照: [%d/%d] %s (%s) | Pat %d/%d Row %d BPM %d SPD %d CH %d | VU: [%d, %d, %d, %d] | 音量: %d%% | 播放: %s | 屏幕: %s",
                 st.track_index, st.total_tracks, st.title, st.format,
                 st.pattern, st.num_patterns, st.row, st.bpm, st.speed, st.num_channels,
                 st.vu[0], st.vu[1], st.vu[2], st.vu[3], st.volume,
                 st.is_playing ? "播放中" : "暂停待命",
                 is_off ? "息屏" : "亮屏");
    } else if (strcasecmp(cmd, "screen") == 0 || strcasecmp(cmd, "disp") == 0) {
        bool is_off = demo_tracker_is_screen_off();
        if (bsp_lvgl_lock(500)) {
            demo_tracker_set_screen_off(!is_off);
            bsp_lvgl_unlock();
            ESP_LOGI(TAG, "屏幕状态已切换为: %s", !is_off ? "息屏" : "亮屏");
        }
    } else if (strcasecmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        print_serial_help();
    } else {
        ESP_LOGW(TAG, "未知命令: \"%s\"。输入 'help' 或 '?' 查看可用命令", cmd);
    }
}

static void serial_cmd_task(void *arg) {
    (void)arg;
    char line_buf[64];
    size_t line_len = 0;

    while (1) {
        int c = getchar();
        if (c != EOF && c > 0) {
            if (c == '\r' || c == '\n') {
                if (line_len > 0) {
                    line_buf[line_len] = '\0';
                    process_serial_cmd(line_buf);
                    line_len = 0;
                }
            } else if (c == ' ' && line_len == 0) {
                tracker_player_toggle_pause();
                tracker_status_t st;
                tracker_player_get_status(&st);
                ESP_LOGI(TAG, "播放状态: %s", st.is_playing ? "播放中 [PLAY]" : "已暂停 [PAUSE]");
            } else if (c == '\b' || c == 0x7F) {
                if (line_len > 0) line_len--;
            } else if (c >= 32 && c <= 126) {
                if (line_len < sizeof(line_buf) - 1) {
                    line_buf[line_len++] = (char)c;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "FoloToy AI Passport Tracker Player 启动");
    esp_sleep_wakeup_cause_t wakeup = esp_sleep_get_wakeup_cause();
    if (wakeup != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "休眠唤醒原因: %d", wakeup);
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败，播放器无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_button_init(on_key, NULL);
    bsp_audio_init();
    bsp_battery_init();

    // 开机直接进入 Tracker 播放器并载入第一帧
    if (bsp_lvgl_lock(1000)) {
        demo_tracker_enter();
        bsp_lvgl_unlock();
    }

    // 画面载入后再点亮背光，避免开机白屏闪烁
    bsp_display_backlight(100);

    xTaskCreate(serial_cmd_task, "serial_cmd", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "Tracker Player 就绪 (输入 'list' 查看曲目, 'help' 查看控制命令, 空格=播/停)");
}

