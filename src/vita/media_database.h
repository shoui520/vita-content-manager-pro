#ifndef VCM_MEDIA_DATABASE_H
#define VCM_MEDIA_DATABASE_H
#include <psp2/types.h>
#include "sync_queue.h"
int vcm_music_sync_mp3(SceUID log, const VcmQueueItem *item, const char *source, char *output);
int vcm_verify_library_item(SceUID log, int kind, const char *path);
int vcm_video_folder(SceUID log, const char *title, const char *path);
int vcm_database_init(void);
#endif
