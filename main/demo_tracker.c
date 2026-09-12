// main/demo_tracker.c —— Tracker/MOD 音乐播放器荧光绿铺面界面
#include "demo_tracker.h"
#include "tracker_player.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t   *s_scr;
static lv_obj_t   *s_play_icon;
static lv_obj_t   *s_title_lbl;
static lv_obj_t   *s_track_lbl;
static lv_obj_t   *s_battery_lbl;

static lv_obj_t   *s_ord_lbl;
static lv_obj_t   *s_row_lbl;
static lv_obj_t   *s_bpm_lbl;

// 4CH 铺面控件 (5 行 x 4 通道)
static lv_obj_t   *s_cont_4ch;
static lv_obj_t   *s_row_num_4ch[TRACKER_VISIBLE_ROWS];
static lv_obj_t   *s_row_note_4ch[TRACKER_VISIBLE_ROWS][TRACKER_MAX_CHANNELS];

// 底栏控件（双层分栏：上层音量/BPM，下层品牌文字）
static lv_obj_t   *s_vol_lbl;
static lv_obj_t   *s_brand_lbl;

// 屏保控件
static lv_obj_t   *s_screensaver_obj;
static lv_obj_t   *s_screensaver_img;
static bool        s_screensaver_active = false;
static int         s_screensaver_anim_tick = 0;

static lv_timer_t *s_timer;

static int  s_last_row = -1;
static int  s_last_pat = -1;
static int  s_last_vol = -1;
static int  s_last_bpm = -1;
static int  s_last_soc = -2;
static bool s_last_playing = false;
static int  s_last_track = -1;

// 铺面差量渲染缓存 (5 行 x 4 通道，未变化不触发 LVGL 重新布局排版)
static int  s_last_row_idx[TRACKER_VISIBLE_ROWS];
static char s_last_note_text[TRACKER_VISIBLE_ROWS][TRACKER_MAX_CHANNELS][8];

// Space Invader 经典外星人（11x8 放大 12 倍 -> 132x96 实体贴图，避免运行时插值缩放引起 layer 分配）
#define INVADER_W 132
#define INVADER_H 96
#define INVADER_SCALE 12

static const char *s_invader_f0[8] = {
    "..1.....1..",
    "...1...1...",
    "..1111111..",
    ".11.111.11.",
    "11111111111",
    "1.1111111.1",
    "1.1.....1.1",
    "...11.11...",
};

static const char *s_invader_f1[8] = {
    "..1.....1..",
    "1..1...1..1",
    "1.1111111.1",
    "111.111.111",
    ".111111111.",
    "..1111111..",
    "..1.....1..",
    ".1.......1.",
};

static uint16_t s_art_buf[INVADER_W * INVADER_H];
static const lv_image_dsc_t s_pixel_art_dsc = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .flags = 0,
        .w = INVADER_W,
        .h = INVADER_H,
        .stride = INVADER_W * 2,
    },
    .data_size = sizeof(s_art_buf),
    .data = (const uint8_t *)s_art_buf,
};

static void update_invader_art(bool frame) {
    const char * const *map = frame ? s_invader_f1 : s_invader_f0;
    uint16_t c_bg   = lv_color_to_u16(lv_color_hex(0x000000));
    uint16_t c_neon = lv_color_to_u16(lv_color_hex(0x00FF66));

    for (int sy = 0; sy < 8; sy++) {
        for (int sx = 0; sx < 11; sx++) {
            uint16_t col = (map[sy][sx] == '1') ? c_neon : c_bg;
            for (int dy = 0; dy < INVADER_SCALE; dy++) {
                int py = sy * INVADER_SCALE + dy;
                for (int dx = 0; dx < INVADER_SCALE; dx++) {
                    int px = sx * INVADER_SCALE + dx;
                    s_art_buf[py * INVADER_W + px] = col;
                }
            }
        }
    }
}

// 周期性 UI 刷新回调 (50 FPS，20ms 间隔，带脏数据差分更新)
static void tracker_ui_tick(lv_timer_t *t) {
    (void)t;
    if (!s_scr) return;

    if (s_screensaver_active) {
        s_screensaver_anim_tick++;
        // Space Invader 经典外星人踏步动画（每 20 个 tick ~ 400ms 切换一次步态）
        if (s_screensaver_anim_tick % 20 == 0) {
            bool frame = (s_screensaver_anim_tick / 20) % 2;
            update_invader_art(frame);
            if (s_screensaver_img) {
                lv_obj_invalidate(s_screensaver_img);
            }
        }
        return;
    }

    tracker_status_t st;
    tracker_player_get_status(&st);

    // 1. 曲目改变时刷新
    if (st.track_index != s_last_track) {
        s_last_track = st.track_index;
        if (s_title_lbl) {
            lv_label_set_text(s_title_lbl, st.title ? st.title : "Unknown");
        }
        if (s_track_lbl) {
            lv_label_set_text_fmt(s_track_lbl, "%d/%d", st.track_index + 1, st.total_tracks);
        }
    }

    // 2. 播放状态切换
    if (st.is_playing != s_last_playing) {
        s_last_playing = st.is_playing;
        if (s_play_icon) {
            lv_label_set_text(s_play_icon, st.is_playing ? ">" : "||");
        }
    }

    // 3. 电量变化
    int soc = bsp_battery_soc();
    if (soc != s_last_soc) {
        s_last_soc = soc;
        if (s_battery_lbl) {
            if (soc >= 0) {
                lv_label_set_text_fmt(s_battery_lbl, "%d%%", soc);
            } else {
                lv_label_set_text(s_battery_lbl, "");
            }
        }
    }

    // 4. 4CH 单页铺面刷新（仅在行号或 Pattern 实际改变时重绘，带差量文本缓存）
    if (st.row != s_last_row || st.pattern != s_last_pat) {
        s_last_row = st.row;
        s_last_pat = st.pattern;

        if (s_ord_lbl) {
            lv_label_set_text_fmt(s_ord_lbl, "ORD:%02d/%02d", st.pattern + 1, st.num_patterns);
        }
        if (s_row_lbl) {
            lv_label_set_text_fmt(s_row_lbl, "ROW:%02d/63", st.row);
        }

        for (int r = 0; r < TRACKER_VISIBLE_ROWS; r++) {
            int row_idx = st.rows[r].row_index;
            if (row_idx != s_last_row_idx[r]) {
                s_last_row_idx[r] = row_idx;
                if (s_row_num_4ch[r]) {
                    if (row_idx >= 0) {
                        lv_label_set_text_fmt(s_row_num_4ch[r], "%02d", row_idx);
                    } else {
                        lv_label_set_text(s_row_num_4ch[r], "--");
                    }
                }
            }
            for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
                const char *txt = st.rows[r].channel_text[ch];
                if (s_row_note_4ch[r][ch] && strcmp(s_last_note_text[r][ch], txt) != 0) {
                    memcpy(s_last_note_text[r][ch], txt, 8);
                    lv_label_set_text(s_row_note_4ch[r][ch], txt);
                }
            }
        }
    }

    // 5. 底栏音量与 BPM 刷新（数值变化时才重刷）
    if (st.volume != s_last_vol) {
        s_last_vol = st.volume;
        if (s_vol_lbl) {
            lv_label_set_text_fmt(s_vol_lbl, "VOL %d%%", st.volume);
        }
    }
    if (st.bpm != s_last_bpm) {
        s_last_bpm = st.bpm;
        if (s_bpm_lbl) {
            lv_label_set_text_fmt(s_bpm_lbl, "BPM %d", st.bpm);
        }
    }
}

void demo_tracker_enter(void) {
    // 启动音频播放器 (22.05 kHz 纯正采样率)
    tracker_player_init(22050);
    s_screensaver_active = false;
    s_screensaver_anim_tick = 0;

    // 重置缓存标识
    s_last_row = -1;
    s_last_pat = -1;
    s_last_vol = -1;
    s_last_bpm = -1;
    s_last_soc = -2;
    s_last_playing = false;
    s_last_track = -1;

    for (int r = 0; r < TRACKER_VISIBLE_ROWS; r++) {
        s_last_row_idx[r] = -999;
        for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
            s_last_note_text[r][ch][0] = '\0';
        }
    }

    // 根屏幕：纯黑底色 #000000（四角露出纯黑圆弧）
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    // 1. 顶栏：荧光绿背景（#00FF66），高度 48px，R=24 圆角（底部被 y=24 的 meta_bar 遮盖，首层高度精确为 24px）
    lv_obj_t *top_bar = lv_obj_create(s_scr);
    lv_obj_set_pos(top_bar, 0, 0);
    lv_obj_set_size(top_bar, 240, 48);
    lv_obj_set_style_bg_color(top_bar, lv_color_hex(0x00FF66), 0);
    lv_obj_set_style_radius(top_bar, 24, 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_remove_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_play_icon = lv_label_create(top_bar);
    lv_obj_set_pos(s_play_icon, 16, 4);
    lv_obj_set_style_text_font(s_play_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_play_icon, lv_color_hex(0x031508), 0);
    lv_label_set_text(s_play_icon, ">");

    s_title_lbl = lv_label_create(top_bar);
    lv_obj_set_pos(s_title_lbl, 30, 4);
    lv_obj_set_size(s_title_lbl, 112, 18);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_title_lbl, lv_color_hex(0x031508), 0);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_title_lbl, "Loading...");

    s_track_lbl = lv_label_create(top_bar);
    lv_obj_set_pos(s_track_lbl, 146, 4);
    lv_obj_set_style_text_font(s_track_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_track_lbl, lv_color_hex(0x031508), 0);
    lv_label_set_text(s_track_lbl, "1/3");

    s_battery_lbl = lv_label_create(top_bar);
    lv_obj_set_pos(s_battery_lbl, 184, 4);
    lv_obj_set_style_text_font(s_battery_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_battery_lbl, lv_color_hex(0x031508), 0);
    lv_label_set_text(s_battery_lbl, "94%");

    // 2. 顶栏第二层：Meta 栏（高度 24px，y: 24..48，严格与底栏上层 24px 对称）
    lv_obj_t *meta_bar = lv_obj_create(s_scr);
    lv_obj_set_pos(meta_bar, 0, 24);
    lv_obj_set_size(meta_bar, 240, 24);
    lv_obj_set_style_bg_color(meta_bar, lv_color_hex(0x051409), 0);
    lv_obj_set_style_radius(meta_bar, 0, 0);
    lv_obj_set_style_border_width(meta_bar, 1, 0);
    lv_obj_set_style_border_side(meta_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(meta_bar, lv_color_hex(0x0E3518), 0);
    lv_obj_set_style_pad_all(meta_bar, 0, 0);
    lv_obj_remove_flag(meta_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_ord_lbl = lv_label_create(meta_bar);
    lv_obj_set_pos(s_ord_lbl, 20, 3);
    lv_obj_set_style_text_font(s_ord_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_ord_lbl, lv_color_hex(0x00FF66), 0);
    lv_label_set_text(s_ord_lbl, "ORD:01/32");

    s_row_lbl = lv_label_create(meta_bar);
    lv_obj_set_pos(s_row_lbl, 136, 3);
    lv_obj_set_style_text_font(s_row_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_row_lbl, lv_color_hex(0x00FF66), 0);
    lv_label_set_text(s_row_lbl, "ROW:00/63");

    // 3. 4CH 铺面容器 (y: 48, 240x224，占满中间全部 224px 视口)
    s_cont_4ch = lv_obj_create(s_scr);
    lv_obj_set_pos(s_cont_4ch, 0, 48);
    lv_obj_set_size(s_cont_4ch, 240, 224);
    lv_obj_set_style_bg_opa(s_cont_4ch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cont_4ch, 0, 0);
    lv_obj_set_style_pad_all(s_cont_4ch, 0, 0);
    lv_obj_remove_flag(s_cont_4ch, LV_OBJ_FLAG_SCROLLABLE);

    // 4CH 列标头 (y: 4)
    lv_obj_t *h_idx_4ch = lv_label_create(s_cont_4ch);
    lv_obj_set_pos(h_idx_4ch, 4, 4);
    lv_obj_set_style_text_font(h_idx_4ch, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h_idx_4ch, lv_color_hex(0x1B4329), 0);
    lv_label_set_text(h_idx_4ch, "#");

    for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
        int col_x = 28 + ch * 53;
        lv_obj_t *h_ch = lv_label_create(s_cont_4ch);
        lv_obj_set_pos(h_ch, col_x, 4);
        lv_obj_set_style_text_font(h_ch, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(h_ch, lv_color_hex(0x1B4329), 0);
        lv_label_set_text_fmt(h_ch, "CH%d", ch + 1);
    }

    // 4CH 中央行高亮瞄准槽 (y: 104, 宽幅 40px，屏幕绝对 y=152..192，正对屏幕中轴)
    lv_obj_t *cursor_4ch = lv_obj_create(s_cont_4ch);
    lv_obj_set_pos(cursor_4ch, 0, 104);
    lv_obj_set_size(cursor_4ch, 240, 40);
    lv_obj_set_style_bg_color(cursor_4ch, lv_color_hex(0x00FF66), 0);
    lv_obj_set_style_bg_opa(cursor_4ch, LV_OPA_20, 0);
    lv_obj_set_style_radius(cursor_4ch, 0, 0);
    lv_obj_set_style_border_width(cursor_4ch, 1, 0);
    lv_obj_set_style_border_side(cursor_4ch, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(cursor_4ch, lv_color_hex(0x00FF66), 0);
    lv_obj_remove_flag(cursor_4ch, LV_OBJ_FLAG_SCROLLABLE);

    // 4CH 5 行铺面文本 (行高 40px，居中垂直排布，起始 y=24)
    for (int r = 0; r < TRACKER_VISIBLE_ROWS; r++) {
        int y = 24 + r * 40 + 12;
        bool is_center = (r == TRACKER_ROW_CENTER);
        int dist = (r > TRACKER_ROW_CENTER) ? (r - TRACKER_ROW_CENTER) : (TRACKER_ROW_CENTER - r);

        s_row_num_4ch[r] = lv_label_create(s_cont_4ch);
        lv_obj_set_pos(s_row_num_4ch[r], 4, y);
        lv_obj_set_style_text_font(s_row_num_4ch[r], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_row_num_4ch[r],
            is_center ? lv_color_hex(0x00FF66) : lv_color_hex(0x1B4329), 0);
        lv_label_set_text(s_row_num_4ch[r], "--");

        for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
            int col_x = 28 + ch * 53;
            s_row_note_4ch[r][ch] = lv_label_create(s_cont_4ch);
            lv_obj_set_pos(s_row_note_4ch[r][ch], col_x, y);
            lv_obj_set_style_text_font(s_row_note_4ch[r][ch], &lv_font_montserrat_14, 0);
            uint32_t color_hex = is_center ? 0xFFFFFF : (dist == 1 ? 0x32C864 : 0x16582C);
            lv_obj_set_style_text_color(s_row_note_4ch[r][ch], lv_color_hex(color_hex), 0);
            lv_label_set_text(s_row_note_4ch[r][ch], "--- ..");
        }
    }

    // 4. 底栏下层：品牌栏（高度 24px，y: 296..320，带物理 R=24 底部圆角，居中写 FOLO TRACKER）
    // 采用与顶栏完全一致的覆盖技术：bot_bar 尺寸 240x48，起始 y=272，圆角 R=24（总高度严格 48px）
    lv_obj_t *bot_bar = lv_obj_create(s_scr);
    lv_obj_set_pos(bot_bar, 0, 272);
    lv_obj_set_size(bot_bar, 240, 48);
    lv_obj_set_style_bg_color(bot_bar, lv_color_hex(0x00FF66), 0);
    lv_obj_set_style_radius(bot_bar, 24, 0);
    lv_obj_set_style_border_width(bot_bar, 0, 0);
    lv_obj_set_style_pad_all(bot_bar, 0, 0);
    lv_obj_remove_flag(bot_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_brand_lbl = lv_label_create(bot_bar);
    lv_obj_set_pos(s_brand_lbl, 0, 28);
    lv_obj_set_size(s_brand_lbl, 240, 18);
    lv_obj_set_style_text_align(s_brand_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_brand_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_brand_lbl, lv_color_hex(0x031508), 0);
    lv_label_set_text(s_brand_lbl, "FOLO TRACKER");

    // 5. 底栏上层：Meta 栏（高度 24px，y: 272..296，覆盖在 bot_bar 上半部，直角矩形）
    // 彻底平滑掩盖 bot_bar 上部圆角，并在 y=272 与 y=296 呈现 1px 细分割线
    lv_obj_t *bot_meta = lv_obj_create(s_scr);
    lv_obj_set_pos(bot_meta, 0, 272);
    lv_obj_set_size(bot_meta, 240, 24);
    lv_obj_set_style_bg_color(bot_meta, lv_color_hex(0x051409), 0);
    lv_obj_set_style_radius(bot_meta, 0, 0);
    lv_obj_set_style_border_width(bot_meta, 1, 0);
    lv_obj_set_style_border_side(bot_meta, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bot_meta, lv_color_hex(0x0E3518), 0);
    lv_obj_set_style_pad_all(bot_meta, 0, 0);
    lv_obj_remove_flag(bot_meta, LV_OBJ_FLAG_SCROLLABLE);

    s_vol_lbl = lv_label_create(bot_meta);
    lv_obj_set_pos(s_vol_lbl, 20, 3);
    lv_obj_set_style_text_font(s_vol_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_vol_lbl, lv_color_hex(0x00FF66), 0);
    lv_label_set_text(s_vol_lbl, "VOL 80%");

    s_bpm_lbl = lv_label_create(bot_meta);
    lv_obj_set_pos(s_bpm_lbl, 156, 3);
    lv_obj_set_style_text_font(s_bpm_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_bpm_lbl, lv_color_hex(0x69F0AE), 0);
    lv_label_set_text(s_bpm_lbl, "BPM 125");

    // 6. 屏保全屏图层（置于 s_scr 最顶层，纯黑全屏，无文字）
    update_invader_art(false);

    s_screensaver_obj = lv_obj_create(s_scr);
    lv_obj_set_pos(s_screensaver_obj, 0, 0);
    lv_obj_set_size(s_screensaver_obj, 240, 320);
    lv_obj_set_style_bg_color(s_screensaver_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_radius(s_screensaver_obj, 0, 0);
    lv_obj_set_style_border_width(s_screensaver_obj, 0, 0);
    lv_obj_set_style_pad_all(s_screensaver_obj, 0, 0);
    lv_obj_remove_flag(s_screensaver_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_screensaver_obj, LV_OBJ_FLAG_HIDDEN); // 初始隐藏

    // Space Invader 经典外星人图像（132x96 实体贴图，居中无文字，无运行时缩放）
    s_screensaver_img = lv_image_create(s_screensaver_obj);
    lv_image_set_src(s_screensaver_img, &s_pixel_art_dsc);
    lv_obj_align(s_screensaver_img, LV_ALIGN_CENTER, 0, 0);

    tracker_ui_tick(NULL);
    s_timer = lv_timer_create(tracker_ui_tick, 25, NULL);
    lv_screen_load(s_scr);
}

void demo_tracker_exit(void) {
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }

    tracker_player_deinit();

    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_play_icon = s_title_lbl = s_track_lbl = s_battery_lbl = NULL;
        s_ord_lbl = s_row_lbl = s_bpm_lbl = NULL;
        s_vol_lbl = s_brand_lbl = NULL;
        s_cont_4ch = NULL;
        for (int r = 0; r < TRACKER_VISIBLE_ROWS; r++) {
            s_row_num_4ch[r] = NULL;
            for (int ch = 0; ch < TRACKER_MAX_CHANNELS; ch++) {
                s_row_note_4ch[r][ch] = NULL;
            }
        }
    }
    bsp_display_backlight(100);
}

void demo_tracker_set_screen_off(bool off) {
    if (s_screensaver_active == off) return;
    s_screensaver_active = off;
    if (off) {
        s_screensaver_anim_tick = 0;
        bsp_display_backlight(5); // 息屏/屏保暗光模式：保持 5% 微光背光播放 Space Invader 经典外星人屏保
        if (s_screensaver_obj) {
            lv_obj_remove_flag(s_screensaver_obj, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        bsp_display_backlight(100); // 唤醒恢复全亮
        if (s_screensaver_obj) {
            lv_obj_add_flag(s_screensaver_obj, LV_OBJ_FLAG_HIDDEN);
        }
        s_last_row = -1;
        s_last_pat = -1;
        s_last_vol = -1;
        s_last_bpm = -1;
        s_last_soc = -2;
        s_last_track = -1;
        s_last_playing = !s_last_playing;
        for (int r = 0; r < TRACKER_VISIBLE_ROWS; r++) {
            s_last_row_idx[r] = -1;
        }
        tracker_ui_tick(NULL);
    }
}

bool demo_tracker_is_screen_off(void) {
    return s_screensaver_active;
}

void demo_tracker_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 息屏拦截与唤醒：处于息屏状态时，仅单击 OK 唤醒屏幕且不触发播放暂停；UP/DOWN 保持正常调音与切歌且保持息屏
    if (s_screensaver_active) {
        if (btn == BSP_BTN_OK) {
            if (ev == BSP_BTN_CLICK) {
                demo_tracker_set_screen_off(false);
            }
            return;
        }
        // UP / DOWN 按键穿透继续往下执行，保持和亮屏时一样的单击调音与长按切歌，且保持息屏
    }

    if (btn == BSP_BTN_UP) {
        if (ev == BSP_BTN_CLICK) {
            uint8_t vol = tracker_player_get_volume();
            if (vol <= 95) tracker_player_set_volume(vol + 5);
            else tracker_player_set_volume(100);
        } else if (ev == BSP_BTN_LONG) {
            tracker_player_prev_track();
        }
    } else if (btn == BSP_BTN_DOWN) {
        if (ev == BSP_BTN_CLICK) {
            uint8_t vol = tracker_player_get_volume();
            if (vol >= 5) tracker_player_set_volume(vol - 5);
            else tracker_player_set_volume(0);
        } else if (ev == BSP_BTN_LONG) {
            tracker_player_next_track();
        }
    } else if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK) {
            tracker_player_toggle_pause();
        } else if (ev == BSP_BTN_LONG) {
            demo_tracker_set_screen_off(true);
        }
    }
}

