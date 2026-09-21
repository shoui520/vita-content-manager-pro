#ifndef VCM_SYNC_QUEUE_H
#define VCM_SYNC_QUEUE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define VCM_QUEUE_MAX 128
enum { VCM_WAITING, VCM_RECEIVING, VCM_RECEIVED, VCM_IMPORTING, VCM_IMPORTED, VCM_FAILED };
typedef struct {
    char id[33], extension[8], name[256];
    char folder[256];
    char title[256], artist[256], album[256], album_artist[256], genre[128];
    uint64_t size, subtitle_size, received;
    int kind, duration_ms, sample_rate, channels, track;
    int state, error, media_ready, subtitle_ready;
} VcmQueueItem;
typedef struct {
    unsigned int revision;
    int count, imported, failed, importing, syncing, cancelled;
    uint64_t transferred, total;
    char id[33];
} VcmQueueInfo;
int vcm_queue_init(void);
int vcm_queue_publish(char *body);
void vcm_queue_info(VcmQueueInfo *info);
int vcm_queue_item(int index, VcmQueueItem *item);
int vcm_queue_matches(const char *id);
int vcm_queue_find(const char *name, uint64_t size, int *sidecar);
int vcm_queue_usb_target(int index, int sidecar, char *name, unsigned int capacity,
                         uint64_t *size);
void vcm_queue_progress(int index, uint64_t received, int sidecar);
void vcm_queue_received(int index, int sidecar, int error);
int vcm_queue_sync(void);
void vcm_queue_cancel(void);
int vcm_queue_cancelled(void);
int vcm_queue_run_imports(void);
#ifdef __cplusplus
}
#endif
#endif
