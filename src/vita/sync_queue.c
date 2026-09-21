#include "sync_queue.h"
#include "sync_import.h"
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2common/kernel/iofilemgr.h>

static VcmQueueInfo queue;
static VcmQueueItem items[VCM_QUEUE_MAX], pending[VCM_QUEUE_MAX];
static SceUID mutex = -1;
static void lock(void) { sceKernelLockMutex(mutex, 1, 0); }
static void unlock(void) { sceKernelUnlockMutex(mutex, 1); }
int vcm_queue_init(void) {
    mutex = sceKernelCreateMutex("vcm_queue", 0, 0, 0);
    queue.importing = -1;
    return mutex < 0 ? mutex : 0;
}
static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int id_valid(const char *id) {
    if (sceClibStrnlen(id, 34) != 32) return 0;
    for (int i = 0; i < 32; ++i)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return 0;
    return 1;
}
static int decode(char *out, unsigned int capacity, const char *in) {
    unsigned int n = 0;
    while (*in) {
        int c = (unsigned char)*in++;
        if (c == '%') {
            if (!in[0] || !in[1] || hex(in[0]) < 0 || hex(in[1]) < 0) return -1;
            c = hex(in[0]) * 16 + hex(in[1]); in += 2;
        }
        if (c < 32 || c == 127 || n + 1 >= capacity) return -1;
        out[n++] = (char)c;
    }
    out[n] = 0;
    return 0;
}
static int decimal(const char *s, uint64_t *out) {
    uint64_t value = 0;
    if (!*s) return -1;
    while (*s) {
        if (*s < '0' || *s > '9' || value > (UINT64_MAX - 9) / 10) return -1;
        value = value * 10 + *s++ - '0';
    }
    *out = value; return 0;
}
static char *field(char **cursor, char delimiter) {
    char *start = *cursor, *end = start;
    if (!start) return 0;
    while (*end && *end != delimiter) ++end;
    if (*end) { *end = 0; *cursor = end + 1; } else *cursor = 0;
    return start;
}
int vcm_queue_publish(char *body) {
    char *cursor = body;
    char *id = field(&cursor, '\n');
    if (!id || !id_valid(id)) return -1;
    int count = 0;
    uint64_t total = 0;
    while (cursor && *cursor) {
        if (count == VCM_QUEUE_MAX) return -1;
        char *line = field(&cursor, '\n'), *part = line, *f[15];
        for (int i = 0; i < 15; ++i) { f[i] = field(&part, '\t'); if (!f[i]) return -1; }
        char *folder=part?field(&part,'\t'):0;
        if (part) return -1;
        VcmQueueItem *item = &pending[count];
        sceClibMemset(item, 0, sizeof(*item));
        if(folder && decode(item->folder,sizeof(item->folder),folder)<0) return -1;
        if (!id_valid(f[0]) || decode(item->extension, sizeof(item->extension), f[1]) < 0) return -1;
        sceClibMemcpy(item->id, f[0], 33);
        for (int j = 0; j < count; ++j) if (!sceClibStrcmp(pending[j].id, item->id)) return -1;
        uint64_t numbers[8];
        int positions[] = {2,3,4,11,12,13,14};
        for (int j = 0; j < 7; ++j) if (decimal(f[positions[j]], &numbers[j]) < 0) return -1;
        item->size = numbers[0]; item->subtitle_size = numbers[1];
        if (!item->size || item->size > 8ULL*1024*1024*1024 || item->subtitle_size > 64*1024*1024 || numbers[2] > 2) return -1;
        item->kind = (int)numbers[2];
        if (numbers[3] > 360000000 || numbers[4] > 96000 || numbers[5] > 2 || numbers[6] > 99999) return -1;
        item->duration_ms = (int)numbers[3]; item->sample_rate = (int)numbers[4];
        item->channels = (int)numbers[5]; item->track = (int)numbers[6];
        const char *ext = item->extension;
        int photo = !sceClibStrcmp(ext,"jpg") || !sceClibStrcmp(ext,"jpeg") || !sceClibStrcmp(ext,"png") || !sceClibStrcmp(ext,"gif") || !sceClibStrcmp(ext,"bmp") || !sceClibStrcmp(ext,"tif") || !sceClibStrcmp(ext,"tiff") || !sceClibStrcmp(ext,"mpo");
        int music = !sceClibStrcmp(ext,"mp3") || !sceClibStrcmp(ext,"m4a") || !sceClibStrcmp(ext,"wav");
        if ((item->kind == 0 && !photo) || (item->kind == 1 && (!music || !item->duration_ms || !item->sample_rate || !item->channels)) || (item->kind == 2 && sceClibStrcmp(ext,"mp4"))) return -1;
        if (item->subtitle_size && item->kind != 2) return -1;
        if (decode(item->name, sizeof(item->name), f[5]) < 0 || !item->name[0] ||
            decode(item->title, sizeof(item->title), f[6]) < 0 ||
            decode(item->artist, sizeof(item->artist), f[7]) < 0 ||
            decode(item->album, sizeof(item->album), f[8]) < 0 ||
            decode(item->album_artist, sizeof(item->album_artist), f[9]) < 0 ||
            decode(item->genre, sizeof(item->genre), f[10]) < 0) return -1;
        total += item->size + item->subtitle_size;
        ++count;
    }
    if (!count) return -1;
    lock();
    if (queue.syncing || (queue.count && !queue.cancelled && queue.imported + queue.failed < queue.count)) { unlock(); return -2; }
    unsigned int revision = queue.revision + 1;
    sceClibMemset(&queue, 0, sizeof(queue));
    queue.revision = revision; queue.count = count; queue.total = total; queue.importing = -1;
    sceClibMemcpy(queue.id, id, 33);
    sceClibMemcpy(items, pending, count * sizeof(items[0]));
    unlock(); return 0;
}
void vcm_queue_info(VcmQueueInfo *info) { lock(); *info = queue; unlock(); }
int vcm_queue_item(int i, VcmQueueItem *item) {
    lock(); int valid = i >= 0 && i < queue.count;
    if (valid) *item = items[i];
    unlock(); return valid ? 0 : -1;
}
int vcm_queue_matches(const char *id) { lock(); int result = !sceClibStrcmp(id, queue.id); unlock(); return result; }
int vcm_queue_find(const char *name, uint64_t size, int *sidecar) {
    lock(); int found = -1;
    if (!queue.cancelled && !queue.syncing) for (int i = 0; i < queue.count; ++i) {
        VcmQueueItem *item = &items[i];
        if (sceClibStrncmp(item->id, name, 32) || name[32] != '.') continue;
        *sidecar = !sceClibStrcmp(name + 33, "m4t");
        if ((*sidecar && item->subtitle_size == size && !item->subtitle_ready) ||
            (!*sidecar && !sceClibStrcmp(name+33,item->extension) && item->size == size && !item->media_ready)) {
            item->state = VCM_RECEIVING; ++queue.revision; found = i;
        }
        break;
    }
    unlock(); return found;
}
int vcm_queue_usb_target(int index, int sidecar, char *name, unsigned int capacity,
                         uint64_t *size) {
    if (!name || capacity < 38 || !size || (sidecar != 0 && sidecar != 1)) return -1;
    lock();
    int valid = !queue.cancelled && !queue.syncing && index >= 0 && index < queue.count;
    if (valid) {
        VcmQueueItem *item = &items[index];
        const char *extension = sidecar ? "m4t" : item->extension;
        uint64_t expected = sidecar ? item->subtitle_size : item->size;
        int written = sceClibSnprintf(name, capacity, "%s.%s", item->id, extension);
        valid = expected != 0 && written > 0 && (unsigned int)written < capacity;
        if (valid) *size = expected;
    }
    unlock();
    return valid ? 0 : -1;
}
void vcm_queue_progress(int i, uint64_t received, int sidecar) {
    lock(); VcmQueueItem *item = &items[i];
    uint64_t value = received + (sidecar && item->media_ready ? item->size : 0);
    queue.transferred -= item->received; item->received = value; queue.transferred += value;
    ++queue.revision; unlock();
}
void vcm_queue_received(int i, int sidecar, int error) {
    lock(); VcmQueueItem *item = &items[i]; item->error = error;
    if (error) { if (item->state != VCM_FAILED) ++queue.failed; item->state = VCM_FAILED; }
    else {
        if (sidecar) item->subtitle_ready = 1; else item->media_ready = 1;
        item->state = item->media_ready && (!item->subtitle_size || item->subtitle_ready) ? VCM_RECEIVED : VCM_RECEIVING;
    }
    ++queue.revision; unlock();
}
int vcm_queue_sync(void) {
    lock();
    if (!queue.count || queue.cancelled || queue.syncing) { unlock(); return -1; }
    for (int i=0;i<queue.count;++i) if(items[i].state != VCM_RECEIVED && items[i].state != VCM_FAILED) { unlock(); return -1; }
    queue.syncing = 1; ++queue.revision; unlock(); return 0;
}
void vcm_queue_cancel(void) { lock(); queue.cancelled=1; ++queue.revision; unlock(); }
int vcm_queue_cancelled(void) { lock(); int value=queue.cancelled; unlock(); return value; }
int vcm_queue_run_imports(void) {
    for (int i=0;;++i) {
        lock();
        if (i>=queue.count || queue.cancelled) { queue.syncing=0; queue.importing=-1; ++queue.revision; unlock(); break; }
        if(items[i].state != VCM_RECEIVED) { unlock(); continue; }
        items[i].state=VCM_IMPORTING; queue.importing=i; ++queue.revision;
        VcmQueueItem item=items[i]; unlock();
        int status=vcm_sync_import(&item);
        lock(); items[i].error=status; items[i].state=status==0?VCM_IMPORTED:VCM_FAILED;
        if(status==0) ++queue.imported; else ++queue.failed;
        ++queue.revision; unlock();
    }
    return 0;
}
