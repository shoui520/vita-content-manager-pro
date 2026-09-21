#include "mtp_catalog.h"

static int copy_name(char target[256], const char *source) {
    if (!source || !*source) return -1;
    unsigned length = 0;
    while (source[length]) {
        unsigned char c = (unsigned char)source[length];
        if (length >= 240 || c < 32 || c == 127 || c == '/' || c == '\\' || c == ':') return -1;
        target[length] = (char)c;
        ++length;
    }
    if (target[length - 1] == ' ' || target[length - 1] == '.') return -1;
    target[length] = 0;
    return 0;
}

static void clear_entry(VcmMtpEntry *entry) {
    volatile unsigned char *bytes = (volatile unsigned char *)entry;
    for (unsigned i = 0; i < sizeof(*entry); ++i) bytes[i] = 0;
}

static void folder(VcmMtpEntry *entry, uint32_t handle, const char *name) {
    clear_entry(entry);
    entry->native_id = 0;
    entry->size = 0;
    entry->handle = handle;
    entry->parent = VCM_MTP_NO_PARENT;
    entry->format = 0x3001;
    entry->kind = VCM_MTP_FOLDER;
    copy_name(entry->name, name);
    copy_name(entry->display_name, name);
}

int vcm_mtp_catalog_init(VcmMtpCatalog *catalog, VcmMtpEntry *storage, uint32_t capacity) {
    if (!catalog || !storage || capacity < 3) return -1;
    catalog->entries = storage;
    catalog->count = 3;
    catalog->capacity = capacity;
    folder(storage, 1, "Photo");
    folder(storage + 1, 2, "Music");
    folder(storage + 2, 3, "Video");
    return 0;
}

uint32_t vcm_mtp_catalog_add(VcmMtpCatalog *catalog, int kind, uint64_t native_id,
                              const char *name, uint64_t size, uint16_t format) {
    if (!catalog || !catalog->entries || catalog->count >= catalog->capacity ||
        catalog->count >= 0xfffffffeu || native_id == 0 ||
        (unsigned)kind > VCM_MTP_SIDECAR ||
        size == 0 || size > 0xfffffff3u || !format || format == 0x3001) return 0;
    VcmMtpEntry *entry = &catalog->entries[catalog->count];
    clear_entry(entry);
    if (copy_name(entry->name, name) < 0) return 0;
    if (copy_name(entry->display_name, name) < 0) return 0;
    entry->title[0] = entry->artist[0] = entry->album[0] = 0;
    entry->created[0] = entry->modified[0] = 0;
    entry->thumb_size = 0;
    entry->thumb_width = entry->thumb_height = 0;
    entry->width = entry->height = 0;
    entry->native_id = native_id;
    entry->size = size;
    entry->handle = catalog->count + 1;
    entry->parent = kind == VCM_MTP_PHOTO ? 1 : kind == VCM_MTP_MUSIC ? 2 : 3;
    entry->format = format;
    entry->kind = (uint8_t)kind;
    ++catalog->count;
    return entry->handle;
}

const VcmMtpEntry *vcm_mtp_catalog_get(const VcmMtpCatalog *catalog, uint32_t handle) {
    if (!catalog || !catalog->entries || handle == 0 || handle > catalog->count) return 0;
    const VcmMtpEntry *entry = &catalog->entries[handle - 1];
    return entry->handle == handle ? entry : 0;
}

uint32_t vcm_mtp_catalog_children(const VcmMtpCatalog *catalog, uint32_t parent,
                                  uint16_t format, uint32_t *out, uint32_t out_capacity) {
    if (!catalog || !catalog->entries ||
        (parent != VCM_MTP_ROOT_HANDLE && parent != VCM_MTP_ALL_HANDLES &&
         (!vcm_mtp_catalog_get(catalog, parent) || parent > 3))) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < catalog->count; ++i) {
        const VcmMtpEntry *entry = &catalog->entries[i];
        if ((parent != VCM_MTP_ALL_HANDLES && entry->parent !=
             (parent == VCM_MTP_ROOT_HANDLE ? VCM_MTP_NO_PARENT : parent)) ||
            (format && entry->format != format)) continue;
        if (out && count < out_capacity) out[count] = entry->handle;
        ++count;
    }
    return count;
}
