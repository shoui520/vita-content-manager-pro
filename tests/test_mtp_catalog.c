#include "../src/vita/mtp_catalog.h"
#include <assert.h>
#include <string.h>

int main(void) {
    VcmMtpEntry entries[5] = {0};
    VcmMtpCatalog catalog;
    assert(vcm_mtp_catalog_init(&catalog, entries, 5) == 0);
    assert(catalog.count == 3);
    assert(!strcmp(entries[0].name, "Photo"));
    assert(!strcmp(entries[1].name, "Music"));
    assert(!strcmp(entries[2].name, "Video"));
    assert(!strcmp(entries[2].display_name, "Video"));
    assert(entries[2].created[0] == 0 && entries[2].thumb_size == 0);
    assert(vcm_mtp_catalog_children(&catalog, VCM_MTP_ROOT_HANDLE, 0, 0, 0) == 3);

    /* No path, empty object or unrepresentable 32-bit MTP transfer can leak
     * into the virtual store. A zero-length GetObject would lack a data phase. */
    assert(!vcm_mtp_catalog_add(&catalog, VCM_MTP_VIDEO, 12, "empty.mp4", 0, 0xb982));
    assert(!vcm_mtp_catalog_add(&catalog, VCM_MTP_PHOTO, 12, "../bad.jpg", 1, 0x3801));
    assert(!vcm_mtp_catalog_add(&catalog, VCM_MTP_PHOTO, 12, "bad:name.jpg", 1, 0x3801));
    assert(!vcm_mtp_catalog_add(&catalog, VCM_MTP_PHOTO, 12, "huge.jpg", UINT32_MAX, 0x3801));
    assert(catalog.count == 3);

    assert(vcm_mtp_catalog_add(&catalog, VCM_MTP_PHOTO, 12,
                               "日本語.jpg", 42, 0x3801) == 4);
    assert(vcm_mtp_catalog_add(&catalog, VCM_MTP_SIDECAR, 34,
                               "episode.m4t", 64, 0xba82) == 5);
    assert(entries[3].parent == 1 && entries[4].parent == 3);
    assert(!strcmp(entries[3].display_name, "日本語.jpg"));
    assert(entries[3].title[0] == 0 && entries[3].modified[0] == 0);
    assert(vcm_mtp_catalog_children(&catalog, 1, 0, 0, 0) == 1);
    assert(vcm_mtp_catalog_children(&catalog, 3, 0, 0, 0) == 1);
    assert(!vcm_mtp_catalog_add(&catalog, VCM_MTP_MUSIC, 56, "audio-input.mp3", 8, 0x3009));
    assert(!vcm_mtp_catalog_get(&catalog, 0));
    assert(!vcm_mtp_catalog_get(&catalog, 6));
    return 0;
}
