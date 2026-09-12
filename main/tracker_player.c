// main/tracker_player.c —— Tracker 播放器核心引擎实现
#include "tracker_player.h"
#include "tracker_tracks.h"

#define POCKETMOD_IMPLEMENTATION
#define POCKETMOD_NO_INTERPOLATION
#include "pocketmod.h"
#include "ibxm.h"

#ifdef ESP_PLATFORM
#include "bsp_audio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef ESP_PLATFORM
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "[%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif

static const char *TAG = "tracker_player";

#define CHUNK_SAMPLES 512
#define IBXM_MAX_TICK_SAMPLES 2048

typedef enum {
    ENGINE_NONE,
    ENGINE_POCKETMOD,
    ENGINE_IBXM,
} tracker_engine_t;

static tracker_engine_t s_engine = ENGINE_NONE;
static pocketmod_context s_ctx;
static struct ibxm_player *s_ibxm_player = NULL;
static int s_ibxm_mix_buf[(IBXM_MAX_TICK_SAMPLES + 65) * 4];
static int s_ibxm_buf_rem = 0;
static int s_ibxm_buf_pos = 0;

static bool s_running = false;
static bool s_loaded = false;
static bool s_playing = false;
static size_t s_current_track = 0;
static uint8_t s_volume = 80;
static uint32_t s_sample_rate = 22050;

#ifdef ESP_PLATFORM
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_mutex = NULL;
#endif

static tracker_status_t s_status_cached;

// ProTracker 音阶周期表 (支持 Octave 1 ~ 6 宽频音域)
static const struct { uint16_t period; const char *name; } NOTE_PERIODS[] = {
    { 856, "C-1" }, { 808, "C#1" }, { 762, "D-1" }, { 720, "D#1" },
    { 678, "E-1" }, { 640, "F-1" }, { 604, "F#1" }, { 570, "G-1" },
    { 538, "G#1" }, { 508, "A-1" }, { 480, "A#1" }, { 453, "B-1" },
    { 428, "C-2" }, { 404, "C#2" }, { 381, "D-2" }, { 360, "D#2" },
    { 339, "E-2" }, { 320, "F-2" }, { 302, "F#2" }, { 285, "G-2" },
    { 269, "G#2" }, { 254, "A-2" }, { 240, "A#2" }, { 226, "B-2" },
    { 214, "C-3" }, { 202, "C#3" }, { 190, "D-3" }, { 180, "D#3" },
    { 170, "E-3" }, { 160, "F-3" }, { 151, "F#3" }, { 143, "G-3" },
    { 135, "G#3" }, { 127, "A-3" }, { 120, "A#3" }, { 113, "B-3" },
    { 107, "C-4" }, { 101, "C#4" }, {  95, "D-4" }, {  90, "D#4" },
    {  85, "E-4" }, {  80, "F-4" }, {  75, "F#4" }, {  71, "G-4" },
    {  67, "G#4" }, {  64, "A-4" }, {  60, "A#4" }, {  57, "B-4" },
    {  54, "C-5" }, {  50, "C#5" }, {  48, "D-5" }, {  45, "D#5" },
    {  42, "E-5" }, {  40, "F-5" }, {  38, "F#5" }, {  36, "G-5" },
    {  34, "G#5" }, {  32, "A-5" }, {  30, "A#5" }, {  28, "B-5" },
    {  27, "C-6" }, {  25, "C#6" }, {  24, "D-6" }, {  22, "D#6" },
    {  21, "E-6" }, {  20, "F-6" }, {  19, "F#6" }, {  18, "G-6" },
    {  17, "G#6" }, {  16, "A-6" }, {  15, "A#6" }, {  14, "B-6" },
};

static const char *period_to_note(uint16_t period) {
    if (period == 0) return "---";
    int n = (int)(sizeof(NOTE_PERIODS) / sizeof(NOTE_PERIODS[0]));
    int low = 0, high = n - 1;
    // Binary search (NOTE_PERIODS is sorted descending: 856 down to 113)
    while (low <= high) {
        int mid = (low + high) >> 1;
        if (NOTE_PERIODS[mid].period == period) return NOTE_PERIODS[mid].name;
        if (NOTE_PERIODS[mid].period > period) low = mid + 1;
        else high = mid - 1;
    }
    // Check nearest neighbors
    int best = -1;
    int min_diff = 99999;
    for (int k = high - 1; k <= low + 1; k++) {
        if (k >= 0 && k < n) {
            int diff = (int)period - (int)NOTE_PERIODS[k].period;
            if (diff < 0) diff = -diff;
            if (diff < min_diff) {
                min_diff = diff;
                best = k;
            }
        }
    }
    if (best >= 0 && min_diff <= 35) {
        return NOTE_PERIODS[best].name;
    }
    return "???";
}

static const char *s_xm_note_names[12] = {
    "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"
};

static void xm_key_to_str(int key, char *out, size_t sz) {
    if (key >= 1 && key <= 96) {
        int note = (key - 1) % 12;
        int oct = (key - 1) / 12;
        snprintf(out, sz, "%s%d", s_xm_note_names[note], oct);
    } else if (key == 97) {
        snprintf(out, sz, "OFF");
    } else {
        snprintf(out, sz, "---");
    }
}

static void format_note_cell(char *txt, const char *n, int sample) {
    txt[0] = n[0]; txt[1] = n[1]; txt[2] = n[2]; txt[3] = ' ';
    if (sample > 0) {
        txt[4] = '0' + (sample / 10);
        txt[5] = '0' + (sample % 10);
    } else {
        txt[4] = '.';
        txt[5] = '.';
    }
    txt[6] = '\0';
}

static void update_status_snapshot(void) {
#ifdef ESP_PLATFORM
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return;
    }
#endif

    s_status_cached.is_playing = s_playing;
    s_status_cached.is_loaded = s_loaded;
    s_status_cached.track_index = (int)s_current_track;
    s_status_cached.total_tracks = (int)tracker_tracks_count();
    s_status_cached.volume = s_volume;

    const tracker_track_t *t = tracker_tracks_get(s_current_track);
    if (t) {
        s_status_cached.title = t->title;
        s_status_cached.artist = t->artist;
        s_status_cached.format = t->format;
    } else {
        s_status_cached.title = "Unknown";
        s_status_cached.artist = "Unknown";
        s_status_cached.format = "MOD";
    }

    if (s_loaded) {
        if (s_engine == ENGINE_POCKETMOD) {
            s_status_cached.num_channels = (int)s_ctx.num_channels;
            s_status_cached.pattern = s_ctx.pattern;
            s_status_cached.num_patterns = s_ctx.length;
            s_status_cached.row = s_ctx.line >= 0 ? s_ctx.line : 0;
            s_status_cached.speed = s_ctx.ticks_per_line;
            // BPM 换算：50 * 125 / (50 * samples_per_tick * ticks_per_line / rate)
            if (s_ctx.samples_per_tick > 0.0f) {
                s_status_cached.bpm = (int)((s_sample_rate * 2.5f) / s_ctx.samples_per_tick);
            } else {
                s_status_cached.bpm = 125;
            }

            // 读取当前 4 通道状态
            for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
                if (ch < s_ctx.num_channels) {
                    // real_volume 范围 0..64
                    int raw_vol = s_ctx.channels[ch].real_volume;
                    int target_vu = (raw_vol * 100) / 64;
                    // 缓动平滑衰减
                    if (target_vu > s_status_cached.vu[ch]) {
                        s_status_cached.vu[ch] = (uint8_t)target_vu;
                    } else if (s_status_cached.vu[ch] > 6) {
                        s_status_cached.vu[ch] -= 6;
                    } else {
                        s_status_cached.vu[ch] = 0;
                    }

                    // 解析当前行音符
                    int pat_idx = (s_ctx.pattern >= 0) ? s_ctx.order[(int)s_ctx.pattern] : 0;
                    int row = s_status_cached.row;
                    if (s_ctx.patterns && pat_idx < s_ctx.num_patterns && row >= 0 && row < 64) {
                        const unsigned char *cell = s_ctx.patterns + 256 * s_ctx.num_channels * pat_idx + 4 * (row * s_ctx.num_channels + ch);
                        uint16_t period = ((cell[0] & 0x0F) << 8) | cell[1];
                        uint8_t sample = (cell[0] & 0xF0) | (cell[2] >> 4);
                        const char *n = period_to_note(period);
                        snprintf(s_status_cached.note_str[ch], sizeof(s_status_cached.note_str[ch]), "%s", n);
                        s_status_cached.sample_num[ch] = sample;
                    }
                } else {
                    s_status_cached.vu[ch] = 0;
                    snprintf(s_status_cached.note_str[ch], sizeof(s_status_cached.note_str[ch]), "---");
                    s_status_cached.sample_num[ch] = 0;
                }
            }

            // 仅在行号或 Pattern 实际变动时更新 7 行铺面快照 (节流 80%+ 冗余计算)
            static int s_last_snap_row = -999;
            static int s_last_snap_pat = -999;
            int cur_pat = (s_ctx.pattern >= 0) ? (int)s_ctx.pattern : 0;
            int cur_row = s_status_cached.row;

            if (cur_row != s_last_snap_row || cur_pat != s_last_snap_pat) {
                s_last_snap_row = cur_row;
                s_last_snap_pat = cur_pat;

                for (int i = 0; i < TRACKER_VISIBLE_ROWS; i++) {
                    int offset = i - TRACKER_ROW_CENTER; // -2 .. +2
                    int target_pat = cur_pat;
                    int target_row = cur_row + offset;

                    // 处理跨 Pattern 换页边界
                    if (target_row < 0) {
                        if (target_pat > 0) {
                            target_pat--;
                            target_row += 64;
                        } else {
                            target_row = -1;
                        }
                    } else if (target_row >= 64) {
                        if (target_pat + 1 < (int)s_ctx.length) {
                            target_pat++;
                            target_row -= 64;
                        } else {
                            target_row = -1;
                        }
                    }

                    s_status_cached.rows[i].row_index = target_row;

                    if (target_row >= 0 && target_row < 64 && s_ctx.patterns && target_pat < s_ctx.length) {
                        int pat_idx = s_ctx.order[target_pat];
                        for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
                            char *txt = s_status_cached.rows[i].channel_text[ch];
                            if (ch < s_ctx.num_channels) {
                                const unsigned char *cell = s_ctx.patterns + 256 * s_ctx.num_channels * pat_idx + 4 * (target_row * s_ctx.num_channels + ch);
                                uint16_t period = ((cell[0] & 0x0F) << 8) | cell[1];
                                uint8_t sample = (cell[0] & 0xF0) | (cell[2] >> 4);
                                const char *n = period_to_note(period);
                                format_note_cell(txt, n, sample);
                            } else {
                                memcpy(txt, "--- ..", 7);
                            }
                        }
                    } else {
                        for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
                            memcpy(s_status_cached.rows[i].channel_text[ch], "--- ..", 7);
                        }
                    }
                }
            }
        } else if (s_engine == ENGINE_IBXM && s_ibxm_player && s_ibxm_player->replay && s_ibxm_player->module) {
            struct replay *rep = s_ibxm_player->replay;
            struct module *mod = s_ibxm_player->module;

            s_status_cached.num_channels = mod->num_channels;
            s_status_cached.pattern = rep->seq_pos;
            s_status_cached.num_patterns = mod->sequence_len;
            s_status_cached.row = rep->row >= 0 ? rep->row : 0;
            s_status_cached.speed = rep->speed;
            s_status_cached.bpm = rep->tempo;

            // 读取前 4 通道状态
            for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
                if (ch < mod->num_channels) {
                    struct channel *c = &rep->channels[ch];
                    int target_vu = (c->ampl * 100) / 64;
                    if (target_vu > 100) target_vu = 100;
                    if (target_vu > s_status_cached.vu[ch]) {
                        s_status_cached.vu[ch] = (uint8_t)target_vu;
                    } else if (s_status_cached.vu[ch] > 6) {
                        s_status_cached.vu[ch] -= 6;
                    } else {
                        s_status_cached.vu[ch] = 0;
                    }

                    xm_key_to_str(c->note.key, s_status_cached.note_str[ch], sizeof(s_status_cached.note_str[ch]));
                    s_status_cached.sample_num[ch] = c->note.instrument;
                } else {
                    s_status_cached.vu[ch] = 0;
                    snprintf(s_status_cached.note_str[ch], sizeof(s_status_cached.note_str[ch]), "---");
                    s_status_cached.sample_num[ch] = 0;
                }
            }

            // 更新行谱面快照
            static int s_last_xm_row = -999;
            static int s_last_xm_seq = -999;
            int cur_seq = rep->seq_pos;
            int cur_row = rep->row;

            if (cur_row != s_last_xm_row || cur_seq != s_last_xm_seq) {
                s_last_xm_row = cur_row;
                s_last_xm_seq = cur_seq;

                int cur_pat_idx = (cur_seq >= 0 && cur_seq < mod->sequence_len) ? mod->sequence[cur_seq] : 0;
                int pat_rows = (cur_pat_idx < mod->num_patterns) ? mod->patterns[cur_pat_idx].num_rows : 64;
                if (pat_rows <= 0) pat_rows = 64;

                for (int i = 0; i < TRACKER_VISIBLE_ROWS; i++) {
                    int offset = i - TRACKER_ROW_CENTER;
                    int target_seq = cur_seq;
                    int target_row = cur_row + offset;

                    if (target_row < 0) {
                        if (target_seq > 0) {
                            target_seq--;
                            int prev_pat = mod->sequence[target_seq];
                            int prev_rows = (prev_pat < mod->num_patterns) ? mod->patterns[prev_pat].num_rows : 64;
                            if (prev_rows <= 0) prev_rows = 64;
                            target_row += prev_rows;
                        } else {
                            target_row = -1;
                        }
                    } else if (target_row >= pat_rows) {
                        if (target_seq + 1 < mod->sequence_len) {
                            target_seq++;
                            target_row -= pat_rows;
                        } else {
                            target_row = -1;
                        }
                    }

                    s_status_cached.rows[i].row_index = target_row;

                    if (target_row >= 0 && target_seq < mod->sequence_len) {
                        int pat_idx = mod->sequence[target_seq];
                        if (pat_idx < mod->num_patterns) {
                            struct pattern *p = &mod->patterns[pat_idx];
                            if (p->data && target_row < p->num_rows) {
                                for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
                                    char *txt = s_status_cached.rows[i].channel_text[ch];
                                    if (ch < p->num_channels) {
                                        int note_off = (target_row * p->num_channels + ch) * 5;
                                        int k = (unsigned char)p->data[note_off];
                                        int ins = (unsigned char)p->data[note_off + 1];
                                        char nstr[8];
                                        xm_key_to_str(k, nstr, sizeof(nstr));
                                        format_note_cell(txt, nstr, ins);
                                    } else {
                                        memcpy(txt, "--- ..", 7);
                                    }
                                }
                            } else {
                                for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
                                    memcpy(s_status_cached.rows[i].channel_text[ch], "--- ..", 7);
                                }
                            }
                        } else {
                            for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
                                memcpy(s_status_cached.rows[i].channel_text[ch], "--- ..", 7);
                            }
                        }
                    } else {
                        for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
                            memcpy(s_status_cached.rows[i].channel_text[ch], "--- ..", 7);
                        }
                    }
                }
            }
        }
    } else {
        s_status_cached.pattern = 0;
        s_status_cached.num_patterns = 0;
        s_status_cached.row = 0;
        s_status_cached.speed = 6;
        s_status_cached.bpm = 125;
        for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
            s_status_cached.vu[ch] = 0;
            snprintf(s_status_cached.note_str[ch], sizeof(s_status_cached.note_str[ch]), "---");
            s_status_cached.sample_num[ch] = 0;
        }
        for (int i = 0; i < TRACKER_VISIBLE_ROWS; i++) {
            s_status_cached.rows[i].row_index = -1;
            for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
                memcpy(s_status_cached.rows[i].channel_text[ch], "--- ..", 7);
            }
        }
    }

#ifdef ESP_PLATFORM
    if (s_mutex) xSemaphoreGive(s_mutex);
#endif
}

static int32_t s_flt_1 = 0, s_flt_2 = 0;

#ifdef ESP_PLATFORM
static void tracker_audio_task(void *arg) {
    (void)arg;
    static int32_t mono_buf[CHUNK_SAMPLES];
    static int16_t pcm_buf[CHUNK_SAMPLES];

    ESP_LOGI(TAG, "音频渲染任务启动 (双引擎: PocketMod + IBXM)");

    while (s_running) {
        if (s_playing && s_loaded) {
            int rendered_samples = 0;
            if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                if (s_playing && s_loaded) {
                    if (s_engine == ENGINE_POCKETMOD) {
                        rendered_samples = pocketmod_render_mono_i32(&s_ctx, mono_buf, CHUNK_SAMPLES);
                    } else if (s_engine == ENGINE_IBXM && s_ibxm_player && s_ibxm_player->replay) {
                        while (rendered_samples < CHUNK_SAMPLES) {
                            if (s_ibxm_buf_rem > 0) {
                                int take = s_ibxm_buf_rem;
                                if (take > CHUNK_SAMPLES - rendered_samples) {
                                    take = CHUNK_SAMPLES - rendered_samples;
                                }
                                for (int i = 0; i < take; i++) {
                                    int l = s_ibxm_mix_buf[s_ibxm_buf_pos * 2];
                                    int r = s_ibxm_mix_buf[s_ibxm_buf_pos * 2 + 1];
                                    mono_buf[rendered_samples++] = (l + r) / 2;
                                    s_ibxm_buf_pos++;
                                }
                                s_ibxm_buf_rem -= take;
                            } else {
                                int tick_len = replay_calculate_tick_len(s_ibxm_player->replay);
                                if (tick_len <= 0 || tick_len > IBXM_MAX_TICK_SAMPLES) {
                                    tick_len = 441;
                                }
                                int got = replay_get_audio(s_ibxm_player->replay, s_ibxm_mix_buf, tick_len);
                                if (got <= 0) {
                                    break;
                                }
                                s_ibxm_buf_rem = got;
                                s_ibxm_buf_pos = 0;
                            }
                        }
                    }
                }
                xSemaphoreGive(s_mutex);
            }

            if (rendered_samples > 0) {
                if (s_engine == ENGINE_POCKETMOD) {
                    // 模拟 Amiga 500 Paula 硬件二阶低通滤波 (Q15 定点化，零浮点开销)
                    // 22.05 kHz 下截止频率约 3.2 kHz -> Q15 定点系数 18000
                    #define FLT_ALPHA_Q15 18000
                    for (int i = 0; i < rendered_samples; i++) {
                        int32_t mono = mono_buf[i];

                        s_flt_1 += ((mono - s_flt_1) * FLT_ALPHA_Q15) >> 15;
                        s_flt_2 += ((s_flt_1 - s_flt_2) * FLT_ALPHA_Q15) >> 15;

                        int32_t out = s_flt_2;
                        if (out > 32767) out = 32767;
                        else if (out < -32767) out = -32767;

                        pcm_buf[i] = (int16_t)out;
                    }
                } else {
                    // XM / FastTracker 纯净定点输出
                    for (int i = 0; i < rendered_samples; i++) {
                        int32_t mono = mono_buf[i];
                        if (mono > 32767) mono = 32767;
                        else if (mono < -32767) mono = -32767;
                        pcm_buf[i] = (int16_t)mono;
                    }
                }
                bsp_audio_write(pcm_buf, (size_t)rendered_samples * sizeof(int16_t));
                taskYIELD(); // 协作式让渡时间片给 taskLVGL
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }

            update_status_snapshot();
        } else {
            // 暂停或未加载，平缓降低电平
            for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
                if (s_status_cached.vu[ch] > 5) s_status_cached.vu[ch] -= 5;
                else s_status_cached.vu[ch] = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }

    ESP_LOGI(TAG, "音频渲染任务退出");
    vTaskDelete(NULL);
}
#endif

bool tracker_player_init(uint32_t sample_rate) {
    if (s_running) return true;
    s_sample_rate = sample_rate ? sample_rate : 22050;
    s_loaded = false;
    s_playing = false;
    s_current_track = 0;
    s_volume = 80;

    memset(&s_status_cached, 0, sizeof(s_status_cached));

#ifdef ESP_PLATFORM
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    bsp_audio_init();
    bsp_audio_set_format(s_sample_rate, 16, 1); // 16-bit 单声道
    bsp_audio_set_volume(s_volume);

    s_running = true;
    BaseType_t ret = xTaskCreate(tracker_audio_task, "trk_audio", 4096, NULL, 5, &s_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建音频任务失败");
        s_running = false;
        return false;
    }
#else
    s_running = true;
#endif

    tracker_tracks_init();

    // 默认载入第一首曲目（保持待命暂停状态，不自动播放）
    tracker_player_load_track(0);
    return true;
}

void tracker_player_deinit(void) {
    if (!s_running) return;

    s_playing = false;
    s_running = false;

#ifdef ESP_PLATFORM
    // 等待音频任务结束
    vTaskDelay(pdMS_TO_TICKS(120));
    s_task = NULL;
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
#endif
    if (s_ibxm_player) {
        if (s_ibxm_player->replay) dispose_replay(s_ibxm_player->replay);
        if (s_ibxm_player->module) dispose_module(s_ibxm_player->module);
        free(s_ibxm_player);
        s_ibxm_player = NULL;
    }
    tracker_tracks_deactivate();
    s_loaded = false;
    s_engine = ENGINE_NONE;
}

static bool tracker_player_load_track_internal(size_t index, bool auto_play) {
    size_t count = tracker_tracks_count();
    if (count == 0) return false;
    if (index >= count) index = 0;

    s_playing = false;
#ifdef ESP_PLATFORM
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
#endif

    // 在互斥锁保护下安全切换曲目 MMU 映射，杜绝音频任务并发读取未映射页
    size_t track_size = 0;
    const uint8_t *track_data = tracker_tracks_activate(index, &track_size);
    if (!track_data || track_size < 17) {
        ESP_LOGE(TAG, "曲目激活/映射失败: index=%u", (unsigned)index);
        s_engine = ENGINE_NONE;
        s_loaded = false;
#ifdef ESP_PLATFORM
        if (s_mutex) xSemaphoreGive(s_mutex);
#endif
        return false;
    }

    const tracker_track_t *t = tracker_tracks_get(index);
    if (!t) {
        ESP_LOGE(TAG, "曲目信息无效: index=%u", (unsigned)index);
        s_engine = ENGINE_NONE;
        s_loaded = false;
#ifdef ESP_PLATFORM
        if (s_mutex) xSemaphoreGive(s_mutex);
#endif
        return false;
    }

    // 清理先前引擎
    if (s_ibxm_player) {
        if (s_ibxm_player->replay) dispose_replay(s_ibxm_player->replay);
        if (s_ibxm_player->module) dispose_module(s_ibxm_player->module);
        free(s_ibxm_player);
        s_ibxm_player = NULL;
    }
    s_ibxm_buf_rem = 0;
    s_ibxm_buf_pos = 0;
    _pocketmod_zero(&s_ctx, sizeof(s_ctx));

    bool is_xm = (t->size >= 17 && memcmp(t->data, "Extended Module:", 16) == 0);

    if (is_xm) {
        struct data d;
        d.buffer = (char *)t->data;
        d.length = (int)t->size;
        s_ibxm_player = play_module_stream(&d, (int)s_sample_rate, 0, 1);
        if (!s_ibxm_player || !s_ibxm_player->replay || !s_ibxm_player->module) {
            ESP_LOGE(TAG, "IBXM 加载失败: %s", t->title);
            s_engine = ENGINE_NONE;
            s_loaded = false;
#ifdef ESP_PLATFORM
            if (s_mutex) xSemaphoreGive(s_mutex);
#endif
            return false;
        }
        s_engine = ENGINE_IBXM;
        ESP_LOGI(TAG, "IBXM 加载成功: %s (通道数: %d, 样式数: %d)",
                 t->title, s_ibxm_player->module->num_channels, s_ibxm_player->module->num_patterns);
    } else {
        int ok = pocketmod_init(&s_ctx, t->data, (int)t->size, (int)s_sample_rate);
        if (!ok) {
            ESP_LOGE(TAG, "PocketMod 初始化失败: %s", t->title);
            s_engine = ENGINE_NONE;
            s_loaded = false;
#ifdef ESP_PLATFORM
            if (s_mutex) xSemaphoreGive(s_mutex);
#endif
            return false;
        }
        s_engine = ENGINE_POCKETMOD;
        ESP_LOGI(TAG, "PocketMod 加载成功: %s (通道数: %d)", t->title, s_ctx.num_channels);
    }

    s_flt_1 = s_flt_2 = 0;
    s_current_track = index;
    s_loaded = true;
    s_playing = auto_play;
#ifdef ESP_PLATFORM
    if (s_mutex) xSemaphoreGive(s_mutex);
#endif
    update_status_snapshot();
    ESP_LOGI(TAG, "%s曲目: [%u] %s (格式 %s, 大小 %u 字节)",
             auto_play ? "开始播放" : "载入待命",
             (unsigned)index, t->title, t->format, (unsigned)t->size);
    return true;
}

bool tracker_player_load_track(size_t index) {
    return tracker_player_load_track_internal(index, false);
}

bool tracker_player_play_track(size_t index) {
    return tracker_player_load_track_internal(index, true);
}

void tracker_player_toggle_pause(void) {
    if (!s_loaded) return;
    s_playing = !s_playing;
    update_status_snapshot();
}

void tracker_player_next_track(void) {
    size_t count = tracker_tracks_count();
    if (count == 0) return;
    size_t next = (s_current_track + 1) % count;
    bool was_playing = s_playing;
    tracker_player_load_track_internal(next, was_playing);
}

void tracker_player_prev_track(void) {
    size_t count = tracker_tracks_count();
    if (count == 0) return;
    size_t prev = (s_current_track + count - 1) % count;
    bool was_playing = s_playing;
    tracker_player_load_track_internal(prev, was_playing);
}

void tracker_player_set_volume(uint8_t vol) {
    if (vol > 100) vol = 100;
    s_volume = vol;
#ifdef ESP_PLATFORM
    bsp_audio_set_volume(s_volume);
#endif
    update_status_snapshot();
}

uint8_t tracker_player_get_volume(void) {
    return s_volume;
}

void tracker_player_get_status(tracker_status_t *status) {
    if (!status) return;
#ifdef ESP_PLATFORM
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        memcpy(status, &s_status_cached, sizeof(tracker_status_t));
        xSemaphoreGive(s_mutex);
        return;
    }
#endif
    memcpy(status, &s_status_cached, sizeof(tracker_status_t));
}
