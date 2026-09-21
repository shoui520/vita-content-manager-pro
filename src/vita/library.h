#ifndef VCM_LIBRARY_H
#define VCM_LIBRARY_H
#include <stdint.h>

enum { VCM_LIBRARY_PHOTO, VCM_LIBRARY_MUSIC, VCM_LIBRARY_VIDEO };
typedef int (*VcmLibraryWrite)(void *context, const void *data, unsigned int size);
typedef struct {
    int kind;
    uint64_t id;
    const char *name;
    const char *title;
    const char *artist;
    const char *album;
    const char *created;
    const char *modified;
    uint64_t size;
    uint16_t mtp_format;
    uint16_t thumb_width;
    uint16_t thumb_height;
    uint32_t width;
    uint32_t height;
} VcmLibraryMediaInfo;
typedef int (*VcmLibraryMedia)(void *context, const VcmLibraryMediaInfo *media);

/* Read-only queries against the Vita's own AVContent databases. */
int vcm_library_list(int kind, int offset, int limit, char *json, unsigned int capacity);
int vcm_library_photo_icons(int offset, int limit, VcmLibraryWrite write, void *context);
/* Fetch one cached Sony DXT1 photo icon, never the full photo. */
int vcm_library_photo_icon(uint64_t id, void *data, unsigned int capacity);
int vcm_library_resolve(int kind, uint64_t id, char *path, unsigned int capacity);
int vcm_library_resolve_sidecar(uint64_t id, char *path, unsigned int capacity);
/* Enumerates only registered, existing media; callback never receives a path. */
int vcm_library_enumerate(int kind, VcmLibraryMedia visit, void *context);
int vcm_library_count(int kind, uint32_t *count);
/* Bounded, path-free diagnostics for why registered music is absent from MTP. */
int vcm_library_audit_music(char *json, unsigned int capacity);
#endif
