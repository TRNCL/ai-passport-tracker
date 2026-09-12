// main/demo_tracker.h —— Tracker/MOD 音乐播放器页面接口
#pragma once

#include "bsp_button.h"
#include <stdbool.h>

void demo_tracker_enter(void);
void demo_tracker_exit(void);
void demo_tracker_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void demo_tracker_set_screen_off(bool off);
bool demo_tracker_is_screen_off(void);
