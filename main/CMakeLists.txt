idf_component_register(
    SRCS "main.c"
         "demo_tracker.c"
         "tracker_player.c"
         "tracker_tracks.c"
         "ibxm.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer nvs_flash esp_partition esp_hw_support
)
