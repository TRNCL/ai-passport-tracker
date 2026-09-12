// main/tracker_tracks.h —— 独立分区/动态映射 Tracker 音乐曲库接口定义
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char *title;
    const char *artist;
    const char *format;
    const uint8_t *data;
    size_t size;
} tracker_track_t;

// 初始化曲库（扫描 tracks 分区并解析 TOC 目录表）
bool tracker_tracks_init(void);

// 获取曲目总数
size_t tracker_tracks_count(void);

// 获取指定索引的曲目信息（越界返回 NULL）
const tracker_track_t *tracker_tracks_get(size_t index);

// 激活指定索引的曲目：通过 MMU 动态映射 Flash 数据，返回连续虚拟内存指针
const uint8_t *tracker_tracks_activate(size_t index, size_t *out_size);

// 释放当前已映射的曲目 Flash MMU 页面
void tracker_tracks_deactivate(void);
