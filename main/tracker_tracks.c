// main/tracker_tracks.c —— 独立分区/动态 MMU 映射 Tracker 音乐曲库实现
#include "tracker_tracks.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_partition.h"
#include "esp_log.h"

static const char *TAG = "tracker_tracks";

#define TRACKS_MAGIC 0x534B5254  // "TRKS"
#define MAX_TRACKS 64

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint32_t toc_size;
    uint32_t total_size;
} pack_header_t;

typedef struct __attribute__((packed)) {
    char title[32];
    char artist[32];
    char format[16];
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t flags;
    uint32_t reserved;
} pack_entry_t;

typedef struct {
    char title[32];
    char artist[32];
    char format[16];
    uint32_t data_offset;
    size_t data_size;
} track_meta_t;

static const esp_partition_t *s_partition = NULL;
static esp_partition_mmap_handle_t s_mmap_handle = 0;
static bool s_has_mmap = false;
static size_t s_active_index = (size_t)-1;

static track_meta_t s_meta[MAX_TRACKS];
static size_t s_track_count = 0;
static tracker_track_t s_current_track_info;

bool tracker_tracks_init(void) {
    if (s_partition && s_track_count > 0) {
        return true;
    }

    s_track_count = 0;
    s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "tracks");
    if (!s_partition) {
        ESP_LOGE(TAG, "未在系统分区表中找到 'tracks' 分区！");
        return false;
    }

    ESP_LOGI(TAG, "找到 'tracks' 分区: offset=0x%lx, size=%lu 字节 (%.2f MB)",
             (unsigned long)s_partition->address, (unsigned long)s_partition->size,
             (double)s_partition->size / (1024.0 * 1024.0));

    pack_header_t hdr;
    esp_err_t err = esp_partition_read(s_partition, 0, &hdr, sizeof(hdr));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取 tracks 分区头部失败: err=%d", err);
        return false;
    }

    if (hdr.magic != TRACKS_MAGIC) {
        ESP_LOGE(TAG, "tracks 分区魔数不匹配: 0x%08lX (期望 0x%08lX)，请先刷入 tracks.bin",
                 (unsigned long)hdr.magic, (unsigned long)TRACKS_MAGIC);
        return false;
    }

    if (hdr.count == 0 || hdr.count > MAX_TRACKS) {
        ESP_LOGE(TAG, "tracks 分区曲目数量无效: %u", (unsigned)hdr.count);
        return false;
    }

    s_track_count = hdr.count;
    ESP_LOGI(TAG, "成功读取 tracks TOC 目录头: 版本=%u, 曲目数=%u, 总资产大小=%lu 字节",
             hdr.version, (unsigned)s_track_count, (unsigned long)hdr.total_size);

    for (size_t i = 0; i < s_track_count; i++) {
        pack_entry_t e;
        size_t entry_offset = sizeof(pack_header_t) + i * sizeof(pack_entry_t);
        err = esp_partition_read(s_partition, entry_offset, &e, sizeof(e));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "读取曲目 [%u] 元数据失败", (unsigned)i);
            s_track_count = i;
            break;
        }

        memcpy(s_meta[i].title, e.title, 32);
        s_meta[i].title[31] = '\0';
        memcpy(s_meta[i].artist, e.artist, 32);
        s_meta[i].artist[31] = '\0';
        memcpy(s_meta[i].format, e.format, 16);
        s_meta[i].format[15] = '\0';
        s_meta[i].data_offset = e.data_offset;
        s_meta[i].data_size = e.data_size;

        ESP_LOGI(TAG, "  [%u] %s - %s [%s] (%u 字节 @ 0x%lx)",
                 (unsigned)i, s_meta[i].title, s_meta[i].artist, s_meta[i].format,
                 (unsigned)s_meta[i].data_size, (unsigned long)s_meta[i].data_offset);
    }

    return (s_track_count > 0);
}

size_t tracker_tracks_count(void) {
    if (!s_partition || s_track_count == 0) {
        tracker_tracks_init();
    }
    return s_track_count;
}

const tracker_track_t *tracker_tracks_get(size_t index) {
    if (!s_partition || s_track_count == 0) {
        tracker_tracks_init();
    }
    if (index >= s_track_count) {
        return NULL;
    }

    s_current_track_info.title = s_meta[index].title;
    s_current_track_info.artist = s_meta[index].artist;
    s_current_track_info.format = s_meta[index].format;
    s_current_track_info.size = s_meta[index].data_size;

    if (s_has_mmap && s_active_index == index) {
        // 当前曲目已处于激活映射状态
    } else {
        s_current_track_info.data = NULL;
    }

    return &s_current_track_info;
}

const uint8_t *tracker_tracks_activate(size_t index, size_t *out_size) {
    if (!s_partition || s_track_count == 0) {
        if (!tracker_tracks_init()) {
            return NULL;
        }
    }
    if (index >= s_track_count) {
        ESP_LOGE(TAG, "激活曲目失败: 索引越界 %u >= %u", (unsigned)index, (unsigned)s_track_count);
        return NULL;
    }

    // 若该曲目已经被激活映射，直接复用返回
    if (s_has_mmap && s_active_index == index && s_current_track_info.data != NULL) {
        if (out_size) *out_size = s_meta[index].data_size;
        return s_current_track_info.data;
    }

    // 释放先前曲目的 MMU 映射
    tracker_tracks_deactivate();

    const void *mapped_ptr = NULL;
    esp_err_t err = esp_partition_mmap(
        s_partition,
        s_meta[index].data_offset,
        s_meta[index].data_size,
        ESP_PARTITION_MMAP_DATA,
        &mapped_ptr,
        &s_mmap_handle
    );

    if (err != ESP_OK || !mapped_ptr) {
        ESP_LOGE(TAG, "MMU 映射曲目 [%u] 失败: err=%d", (unsigned)index, err);
        return NULL;
    }

    s_has_mmap = true;
    s_active_index = index;

    s_current_track_info.title = s_meta[index].title;
    s_current_track_info.artist = s_meta[index].artist;
    s_current_track_info.format = s_meta[index].format;
    s_current_track_info.size = s_meta[index].data_size;
    s_current_track_info.data = (const uint8_t *)mapped_ptr;

    if (out_size) *out_size = s_meta[index].data_size;

    ESP_LOGI(TAG, "MMU 映射成功: [%u] %s (虚拟地址 %p, 大小 %u 字节)",
             (unsigned)index, s_meta[index].title, mapped_ptr, (unsigned)s_meta[index].data_size);

    return s_current_track_info.data;
}

void tracker_tracks_deactivate(void) {
    if (s_has_mmap) {
        esp_partition_munmap(s_mmap_handle);
        s_has_mmap = false;
        s_active_index = (size_t)-1;
        s_current_track_info.data = NULL;
        ESP_LOGD(TAG, "MMU 映射已释放");
    }
}

#else
// 宿主机单元测试 Mock 曲库
static const uint8_t s_mock_mod[1084] = {
    'T', 'e', 's', 't', ' ', 'S', 'o', 'n', 'g', 0
};

static tracker_track_t s_mock_track = {
    .title = "Host Mock Track",
    .artist = "Test Artist",
    .format = "MOD 4CH",
    .data = s_mock_mod,
    .size = sizeof(s_mock_mod),
};

bool tracker_tracks_init(void) {
    return true;
}

size_t tracker_tracks_count(void) {
    return 1;
}

const tracker_track_t *tracker_tracks_get(size_t index) {
    if (index == 0) return &s_mock_track;
    return NULL;
}

const uint8_t *tracker_tracks_activate(size_t index, size_t *out_size) {
    if (index == 0) {
        if (out_size) *out_size = sizeof(s_mock_mod);
        return s_mock_mod;
    }
    return NULL;
}

void tracker_tracks_deactivate(void) {}

#endif
