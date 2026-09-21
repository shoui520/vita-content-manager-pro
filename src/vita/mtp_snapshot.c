#include "mtp_snapshot.h"
#include "library.h"
#include <paf/std/stdlib.h>

static void copy_text(char *out, unsigned capacity, const char *in) {
    unsigned i = 0;
    if (!in) in = "";
    while (in[i] && i + 1 < capacity) {
        unsigned char first = (unsigned char)in[i];
        unsigned length = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2 :
                          (first & 0xf0) == 0xe0 ? 3 : (first & 0xf8) == 0xf0 ? 4 : 0;
        if (!length || i + length >= capacity) break;
        unsigned good = 1;
        for (unsigned j = 1; j < length; ++j)
            if (!in[i + j] || ((unsigned char)in[i + j] & 0xc0) != 0x80) good = 0;
        if (!good) break;
        for (unsigned j = 0; j < length; ++j) out[i + j] = in[i + j];
        i += length;
    }
    out[i] = 0;
}

static void display_name(VcmMtpEntry *entry) {
    if (!entry->title[0] || (entry->kind != VCM_MTP_MUSIC &&
                             entry->kind != VCM_MTP_VIDEO)) return;
    const char *dot = 0;
    for (const char *p = entry->name; *p; ++p) if (*p == '.') dot = p;
    if (!dot) return;
    char safe_title[240];
    copy_text(safe_title, sizeof(safe_title), entry->title);
    unsigned used = 0;
    for (const unsigned char *p = (const unsigned char *)safe_title;
         *p && used + 8 < sizeof(entry->display_name); ++p) {
        unsigned char c = *p;
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':' ||
            c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') c = '_';
        entry->display_name[used++] = (char)c;
    }
    while (used && (entry->display_name[used - 1] == ' ' ||
                    entry->display_name[used - 1] == '.')) --used;
    if (!used || used + 7 >= sizeof(entry->display_name)) return;
    for (const char *p = dot; *p && used + 1 < sizeof(entry->display_name); ++p)
        entry->display_name[used++] = *p;
    entry->display_name[used] = 0;
}

static int add_entry(void *context, const VcmLibraryMediaInfo *media) {
    VcmMtpCatalog *catalog = (VcmMtpCatalog *)context;
    if (media->size == 0) return 0; /* Empty media has no valid GetObject data phase. */
    uint32_t handle = vcm_mtp_catalog_add(catalog, media->kind, media->id,
                                           media->name, media->size, media->mtp_format);
    if (!handle) return -1;
    VcmMtpEntry *entry = &catalog->entries[handle - 1];
    copy_text(entry->title, sizeof(entry->title), media->title);
    copy_text(entry->artist, sizeof(entry->artist), media->artist);
    copy_text(entry->album, sizeof(entry->album), media->album);
    copy_text(entry->created, sizeof(entry->created), media->created);
    copy_text(entry->modified, sizeof(entry->modified), media->modified);
    entry->thumb_width = media->thumb_width;
    entry->thumb_height = media->thumb_height;
    entry->width = media->width;
    entry->height = media->height;
    if (entry->kind == VCM_MTP_PHOTO && entry->thumb_width && entry->thumb_height) {
        unsigned stride = (entry->thumb_width * 3u + 3u) & ~3u;
        entry->thumb_size = 54u + stride * entry->thumb_height;
    }
    display_name(entry);
    return 0;
}

int vcm_mtp_snapshot_build(VcmMtpSnapshot *snapshot) {
    if (!snapshot) return -1;
    snapshot->allocation = 0;
    snapshot->catalog.entries = 0;
    snapshot->catalog.count = 0;
    snapshot->catalog.capacity = 0;
    uint32_t photo, music, video;
    if (vcm_library_count(VCM_LIBRARY_PHOTO, &photo) < 0 ||
        vcm_library_count(VCM_LIBRARY_MUSIC, &music) < 0 ||
        vcm_library_count(VCM_LIBRARY_VIDEO, &video) < 0) return -1;
    uint64_t needed = 3ull + photo + music + 2ull * video;
    if (needed > 0xfffffffeu || needed * sizeof(VcmMtpEntry) > 24ull * 1024 * 1024) return -1;
    snapshot->allocation = (VcmMtpEntry *)sce_paf_malloc((size_t)needed * sizeof(VcmMtpEntry));
    if (!snapshot->allocation) return -1;
    if (vcm_mtp_catalog_init(&snapshot->catalog, snapshot->allocation, (uint32_t)needed) < 0 ||
        vcm_library_enumerate(VCM_LIBRARY_PHOTO, add_entry, &snapshot->catalog) < 0 ||
        vcm_library_enumerate(VCM_LIBRARY_MUSIC, add_entry, &snapshot->catalog) < 0 ||
        vcm_library_enumerate(VCM_LIBRARY_VIDEO, add_entry, &snapshot->catalog) < 0) {
        vcm_mtp_snapshot_destroy(snapshot);
        return -1;
    }
    return 0;
}

void vcm_mtp_snapshot_destroy(VcmMtpSnapshot *snapshot) {
    if (!snapshot) return;
    if (snapshot->allocation) sce_paf_free(snapshot->allocation);
    snapshot->allocation = 0;
    snapshot->catalog.entries = 0;
    snapshot->catalog.count = 0;
    snapshot->catalog.capacity = 0;
}
