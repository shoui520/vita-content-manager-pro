#ifndef VCM_USB_CATALOG_CLIENT_H
#define VCM_USB_CATALOG_CLIENT_H
#include "../vita/mtp_catalog.h"
#include <psp2/types.h>

typedef struct {
    SceUID block;
    VcmMtpCatalog catalog;
    uint64_t max_size;
    uint64_t free_size;
} VcmBridgeCatalog;

typedef struct {
    int socket;
    uint64_t remaining;
} VcmBridgeStream;

typedef struct {
    int socket;
    uint64_t expected;
    uint64_t written;
} VcmBridgeUpload;

typedef struct {
    uint32_t count, imported, failed, syncing, cancelled;
    uint64_t transferred, total;
} VcmBridgeQueueStatus;

int vcm_bridge_catalog_fetch(VcmBridgeCatalog *out);
void vcm_bridge_catalog_release(VcmBridgeCatalog *catalog);
/* The application resolves native IDs. The bridge never accepts a filesystem path. */
int vcm_bridge_stream_open(VcmBridgeStream *stream, const VcmMtpEntry *entry);
int vcm_bridge_stream_read(VcmBridgeStream *stream, void *data, unsigned int capacity);
void vcm_bridge_stream_close(VcmBridgeStream *stream);
int vcm_bridge_thumbnail_fetch(const VcmMtpEntry *entry, void *bmp, unsigned int capacity);
int vcm_bridge_queue_begin(VcmBridgeUpload *upload, uint64_t size);
int vcm_bridge_file_begin(VcmBridgeUpload *upload, unsigned int index, int sidecar,
                          uint64_t size);
int vcm_bridge_upload_write(VcmBridgeUpload *upload, const void *data, unsigned int size);
int vcm_bridge_upload_finish(VcmBridgeUpload *upload);
void vcm_bridge_upload_abort(VcmBridgeUpload *upload);
int vcm_bridge_queue_action(const char *action);
int vcm_bridge_queue_status(VcmBridgeQueueStatus *status);
int vcm_bridge_item_status(unsigned int index, uint32_t *state, int32_t *error);

#endif
