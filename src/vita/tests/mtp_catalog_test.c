#include "../mtp_catalog.h"
#include <assert.h>
#include <string.h>

int main(void) {
    VcmMtpEntry entries[8];
    VcmMtpCatalog catalog;
    assert(vcm_mtp_catalog_init(&catalog, entries, 8) == 0);
    assert(vcm_mtp_catalog_children(&catalog, VCM_MTP_ROOT_HANDLE, 0, 0, 0) == 3);
    assert(vcm_mtp_catalog_children(&catalog, VCM_MTP_ALL_HANDLES, 0, 0, 0) == 3);
    assert(entries[0].parent == VCM_MTP_NO_PARENT);
    assert(strcmp(vcm_mtp_catalog_get(&catalog, 3)->name, "Video") == 0);
    assert(vcm_mtp_catalog_get(&catalog, 0) == 0);
    assert(vcm_mtp_catalog_get(&catalog, 4) == 0);

    const uint64_t id = 144115192370823591ull;
    assert(vcm_mtp_catalog_add(&catalog, VCM_MTP_PHOTO, id, "写真.jpg", 12345, 0x3801) == 4);
    assert(vcm_mtp_catalog_get(&catalog, 4)->native_id == id);
    assert(vcm_mtp_catalog_get(&catalog, 4)->parent == 1);
    assert(vcm_mtp_catalog_add(&catalog, VCM_MTP_VIDEO, 8, "Episode.mp4", 99, 0xb982) == 5);
    assert(vcm_mtp_catalog_add(&catalog, VCM_MTP_SIDECAR, 8, "Episode.m4t", 20, 0xba82) == 6);
    uint32_t handles[3] = {0};
    assert(vcm_mtp_catalog_children(&catalog, 3, 0, handles, 3) == 2);
    assert(vcm_mtp_catalog_children(&catalog, VCM_MTP_ALL_HANDLES, 0, 0, 0) == 6);
    assert(vcm_mtp_catalog_children(&catalog, VCM_MTP_ROOT_HANDLE, 0, 0, 0) == 3);
    assert(handles[0] == 5 && handles[1] == 6);
    assert(vcm_mtp_catalog_children(&catalog, 3, 0xb982, handles, 3) == 1);
    assert(handles[0] == 5);
    assert(vcm_mtp_catalog_children(&catalog, 4, 0, handles, 3) == 0);

    assert(vcm_mtp_catalog_add(&catalog, VCM_MTP_PHOTO, 9, "ux0:/private.jpg", 8, 0x3801) == 0);
    assert(vcm_mtp_catalog_add(&catalog, VCM_MTP_PHOTO, 9, "../private.jpg", 8, 0x3801) == 0);
    assert(vcm_mtp_catalog_add(&catalog, VCM_MTP_PHOTO, 9, "photo.jpg", 0x100000000ull, 0x3801) == 0);
    assert(catalog.count == 6);
    return 0;
}
