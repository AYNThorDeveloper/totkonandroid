#pragma once
#include <stddef.h>
#include <stdint.h>
int android_video_menu_patch(uint8_t *base, uint64_t text_end, uint64_t ro_off);
void android_video_menu_block_input(int blocked);
int android_video_menu_input_blocked(void);
void android_video_menu_settings_path(const char *path);
void android_video_menu_startup(void);
void android_video_menu_active_output(unsigned width, unsigned height);
int android_video_menu_request(void);
