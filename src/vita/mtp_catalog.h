#ifndef VCM_MTP_CATALOG_H
#define VCM_MTP_CATALOG_H

#include <stdint.h>

enum {
    VCM_MTP_PHOTO = 0,
    VCM_MTP_MUSIC = 1,
    VCM_MTP_VIDEO = 2,
    VCM_MTP_SIDECAR = 3,
    VCM_MTP_FOLDER = 4,
    VCM_MTP_STORAGE_ID = 0x00010001u,
    /* GetObjectHandles parent parameter: 0 means all, FFFFFFFF root only.
     * ObjectInfo.ParentObject still uses 0 for a root-level object. */
    VCM_MTP_ALL_HANDLES = 0,
    VCM_MTP_ROOT_HANDLE = 0xffffffffu,
    VCM_MTP_NO_PARENT = 0
};

/* A catalogue never contains an on-device path. Handles are session-scoped. */
typedef struct {
    uint64_t native_id;
    uint64_t size;
    uint32_t handle;
    uint32_t parent;
    uint16_t format;
    uint8_t kind;
    char name[256];
    char display_name[256];
    char title[256];
    char artist[128];
    char album[128];
    char created[24];
    char modified[24];
    uint32_t thumb_size;
    uint16_t thumb_width;
    uint16_t thumb_height;
    uint32_t width;
    uint32_t height;
} VcmMtpEntry;

typedef struct {
    VcmMtpEntry *entries;
    uint32_t count;
    uint32_t capacity;
} VcmMtpCatalog;

/* Application-to-shell loopback response, little-endian on Vita ARM. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t entry_size;
    uint64_t max_size;
    uint64_t free_size;
} VcmMtpCatalogHeader;

_Static_assert(sizeof(VcmMtpCatalogHeader) == 32, "MTP catalog header ABI");

/* Returns 0 only if space exists for the three immutable root folders. */
int vcm_mtp_catalog_init(VcmMtpCatalog *catalog, VcmMtpEntry *storage, uint32_t capacity);
/* Returns a nonzero handle; 0 means invalid input or catalogue full. */
uint32_t vcm_mtp_catalog_add(VcmMtpCatalog *catalog, int kind, uint64_t native_id,
                              const char *name, uint64_t size, uint16_t format);
const VcmMtpEntry *vcm_mtp_catalog_get(const VcmMtpCatalog *catalog, uint32_t handle);
/* A format of 0 matches all; parent 0 means all, 0xffffffff root only. */
uint32_t vcm_mtp_catalog_children(const VcmMtpCatalog *catalog, uint32_t parent,
                                  uint16_t format, uint32_t *out, uint32_t out_capacity);

#endif
