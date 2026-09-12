// tests/test_tracker_player.c —— Tracker 播放器主机单元测试
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "tracker_tracks.h"
#include "tracker_player.h"

int main(void) {
    printf("Starting tracker player host tests...\n");

    // 1. 测试曲库列表接口
    size_t count = tracker_tracks_count();
    assert(count > 0);

    const tracker_track_t *t0 = tracker_tracks_get(0);
    assert(t0 != NULL);
    assert(t0->title != NULL);
    assert(t0->data != NULL);
    assert(t0->size > 0);

    const tracker_track_t *t_invalid = tracker_tracks_get(999);
    assert(t_invalid == NULL);

    // 2. 测试播放器基本初始化与状态
    bool ok = tracker_player_init(22050);
    assert(ok == true);

    tracker_status_t st;
    memset(&st, 0, sizeof(st));
    tracker_player_get_status(&st);
    assert(st.total_tracks == (int)count);

    // 3. 测试音量控制
    tracker_player_set_volume(90);
    assert(tracker_player_get_volume() == 90);
    tracker_player_set_volume(150); // 截断到 100
    assert(tracker_player_get_volume() == 100);

    // 4. 测试切歌逻辑：暂停状态下切歌保持暂停
    tracker_player_next_track();
    tracker_player_get_status(&st);
    assert(st.is_playing == false);

    tracker_player_prev_track();
    tracker_player_get_status(&st);
    assert(st.is_playing == false);

    // 5. 测试暂停/恢复
    tracker_player_toggle_pause();
    tracker_player_get_status(&st);
    assert(st.is_playing == true);

    // 6. 测试切歌逻辑：播放状态下切歌保持自动播放
    tracker_player_next_track();
    tracker_player_get_status(&st);
    assert(st.is_playing == true);

    tracker_player_prev_track();
    tracker_player_get_status(&st);
    assert(st.is_playing == true);

    // 7. 销毁测试
    tracker_player_deinit();

    printf("Tracker player host tests: PASS\n");
    return 0;
}
