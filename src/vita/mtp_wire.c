#include "mtp_wire.h"

static void bytes(VcmMtpWriter *w, const uint8_t *source, uint32_t size) {
    if (w->error || size > w->capacity - w->length) { w->error = 1; return; }
    for (uint32_t i = 0; i < size; ++i) w->data[w->length++] = source[i];
}
static void u8(VcmMtpWriter *w, uint8_t value) { bytes(w, &value, 1); }
static void u16(VcmMtpWriter *w, uint16_t value) {
    u8(w, (uint8_t)value); u8(w, (uint8_t)(value >> 8));
}
static void u32(VcmMtpWriter *w, uint32_t value) {
    u16(w, (uint16_t)value); u16(w, (uint16_t)(value >> 16));
}
static void u64(VcmMtpWriter *w, uint64_t value) {
    u32(w, (uint32_t)value); u32(w, (uint32_t)(value >> 32));
}
static void array16(VcmMtpWriter *w, const uint16_t *values, uint32_t count) {
    u32(w, count);
    for (uint32_t i = 0; i < count; ++i) u16(w, values[i]);
}
/* USB MTP strings are counted UTF-16 strings, including the terminator. */
static void string(VcmMtpWriter *w, const char *utf8) {
    uint32_t units = 0;
    uint32_t start = w->length;
    const uint8_t *p = (const uint8_t *)utf8;
    if (!p || !*p) { u8(w, 0); return; }
    while (*p && units < 254) {
        uint32_t cp;
        if (*p < 0x80) { cp = *p++; }
        else if ((*p & 0xe0) == 0xc0 && p[1] && (p[1] & 0xc0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f); p += 2;
            if (cp < 0x80) cp = '?';
        } else if ((*p & 0xf0) == 0xe0 && p[1] && p[2] &&
                   (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) |
                 (p[2] & 0x3f); p += 3;
            if (cp < 0x800) cp = '?';
        } else if ((*p & 0xf8) == 0xf0 && p[1] && p[2] && p[3] &&
                   (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 &&
                   (p[3] & 0xc0) == 0x80) {
            cp = ((uint32_t)(p[0] & 7) << 18) | ((uint32_t)(p[1] & 0x3f) << 12) |
                 ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f); p += 4;
            if (cp < 0x10000 || cp > 0x10ffff) cp = '?';
        } else { cp = '?'; ++p; }
        if (cp < 0x20 || (cp >= 0xd800 && cp <= 0xdfff)) cp = '?';
        if (cp > 0xffff && units + 2 > 254) break;
        if (w->capacity - w->length < (cp > 0xffff ? 4u : 2u)) { w->error = 1; return; }
        if (cp > 0xffff) {
            cp -= 0x10000;
            u16(w, (uint16_t)(0xd800 + (cp >> 10)));
            u16(w, (uint16_t)(0xdc00 + (cp & 0x3ff)));
            units += 2;
        } else { u16(w, (uint16_t)cp); ++units; }
    }
    if (w->error || w->capacity - w->length < 3) { w->error = 1; return; }
    /* Shift the encoded payload right to prepend the character count. */
    for (uint32_t i = w->length; i > start; --i) w->data[i] = w->data[i - 1];
    w->data[start] = (uint8_t)(units + 1);
    w->length += 1;
    u16(w, 0);
}

void vcm_mtp_writer_init(VcmMtpWriter *writer, uint8_t *data, uint32_t capacity) {
    writer->data = data; writer->capacity = capacity; writer->length = 0;
    writer->error = data == 0;
}

int vcm_mtp_device_info(VcmMtpWriter *w) {
    static const uint16_t operations[] = {
        0x1001, 0x1002, 0x1003, 0x1004, 0x1005, 0x1006, 0x1007, 0x1008, 0x1009, 0x100a,
        0x9801, 0x9802, 0x9803,
        0x9ff0, 0x9ff1, 0x9ff2, 0x9ff3, 0x9ff4, 0x9ff5
    };
    static const uint16_t formats[] = {
        0x3001, /* folder */
        0x3801, 0x3804, 0x3807, 0x380b, 0x380d, 0xb301, /* photos */
        0x3008, 0x3009, 0xb215, 0xb903, /* music */
        0xb982, 0xba82 /* MP4 video and M4T sidecar */
    };
    u16(w, 100);             /* PTP standard version 1.00 */
    u32(w, 6);               /* MTP vendor extension */
    u16(w, 100);             /* vendor extension version */
    string(w, "microsoft.com: 1.0; ");
    u16(w, 0);               /* functional mode */
    array16(w, operations, sizeof(operations) / sizeof(operations[0]));
    u32(w, 0);               /* events */
    u32(w, 0);               /* device properties */
    u32(w, 0);               /* capture formats */
    array16(w, formats, sizeof(formats) / sizeof(formats[0]));
    string(w, "Content Manager Pro");
    string(w, "Virtual Media Library");
    string(w, "1.0");
    string(w, "VCM");
    return w->error ? -1 : (int)w->length;
}

int vcm_mtp_storage_info(VcmMtpWriter *w, uint64_t capacity, uint64_t free_bytes) {
    u16(w, 3);               /* fixed RAM storage (virtual view) */
    u16(w, 2);               /* generic hierarchical filesystem */
    u16(w, 1);               /* read-only from a file manager */
    u64(w, capacity);
    u64(w, free_bytes);
    u32(w, 0xffffffff);      /* free objects unknown */
    string(w, "Media on PS Vita");
    string(w, "Photo, Music and Video");
    return w->error ? -1 : (int)w->length;
}

int vcm_mtp_object_info(VcmMtpWriter *w, const VcmMtpObjectInfo *o) {
    if (!o) return -1;
    u32(w, o->storage_id); u16(w, o->format); u16(w, 1); /* read-only */
    u32(w, o->size);
    u16(w, o->thumb_size ? 0x3804 : 0); /* BMP thumbnail */
    u32(w, o->thumb_size); u32(w, o->thumb_width); u32(w, o->thumb_height);
    u32(w, o->width); u32(w, o->height); u32(w, 0); /* image dimensions and bit depth */
    u32(w, o->parent); u16(w, o->format == 0x3001 ? 1 : 0);
    u32(w, 0); u32(w, 0); /* association description, sequence */
    string(w, o->name); string(w, o->created); string(w, o->modified); string(w, "");
    return w->error ? -1 : (int)w->length;
}

int vcm_mtp_u32_array(VcmMtpWriter *w, const uint32_t *values, uint32_t count) {
    if (count && !values) return -1;
    u32(w, count);
    for (uint32_t i = 0; i < count; ++i) u32(w, values[i]);
    return w->error ? -1 : (int)w->length;
}

/* MTP 1.0 read-only object properties; codes and data types are the standard
 * ObjectPropDesc/GetObjectPropValue wire formats, not Sony-specific commands. */
static const uint16_t common_props[] = {
    0xdc01, 0xdc02, 0xdc03, 0xdc04, 0xdc07, 0xdc08, 0xdc09, 0xdc0b,
    0xdc41, 0xdc44, 0xdc4f
};

static int supported(uint16_t code, uint16_t format) {
    for (unsigned i = 0; i < sizeof(common_props)/sizeof(common_props[0]); ++i)
        if (common_props[i] == code) return 1;
    if ((code == 0xdc46 || code == 0xdc9a) &&
        (format == 0x3008 || format == 0x3009 || format == 0xb903)) return 1;
    return (code == 0xdc87 || code == 0xdc88) &&
           (format == 0x3801 || format == 0x3804 || format == 0x3807 ||
            format == 0x380b || format == 0x380d || format == 0xb301 ||
            format == 0xb982);
}

static uint16_t prop_type(uint16_t code) {
    switch (code) {
    case 0xdc4f: return 0x0002;                   /* UINT8 */
    case 0xdc01: case 0xdc0b: case 0xdc87: case 0xdc88: return 0x0006; /* UINT32 */
    case 0xdc02: case 0xdc03: return 0x0004; /* UINT16 */
    case 0xdc04: return 0x0008;              /* UINT64 */
    case 0xdc41: return 0x000a;              /* UINT128 */
    default: return 0xffff;                   /* counted UTF-16 string */
    }
}

int vcm_mtp_object_props_supported(VcmMtpWriter *w, uint16_t format) {
    uint16_t props[sizeof(common_props)/sizeof(common_props[0]) + 2];
    uint32_t count = sizeof(common_props)/sizeof(common_props[0]);
    for (uint32_t i = 0; i < count; ++i) props[i] = common_props[i];
    if (supported(0xdc46, format)) { props[count++] = 0xdc46; props[count++] = 0xdc9a; }
    else if (supported(0xdc87, format)) { props[count++] = 0xdc87; props[count++] = 0xdc88; }
    array16(w, props, count);
    return w->error ? -1 : (int)w->length;
}

int vcm_mtp_object_prop_desc(VcmMtpWriter *w, uint16_t code, uint16_t format) {
    if (!supported(code, format)) return -1;
    uint16_t type = prop_type(code);
    u16(w, code); u16(w, type); u8(w, 0); /* read-only */
    if (type == 0x0002) u8(w, 0);
    else if (type == 0x0004) u16(w, 0);
    else if (type == 0x0006) u32(w, 0);
    else if (type == 0x0008) u64(w, 0);
    else if (type == 0x000a) { u64(w, 0); u64(w, 0); }
    else string(w, "");
    u32(w, 0); u8(w, 0); /* no property group or form */
    return w->error ? -1 : (int)w->length;
}

int vcm_mtp_object_prop_value(VcmMtpWriter *w, const VcmMtpEntry *entry, uint16_t code) {
    if (!entry || !supported(code, entry->format)) return -1;
    switch (code) {
    case 0xdc01: u32(w, VCM_MTP_STORAGE_ID); break;
    case 0xdc02: u16(w, entry->format); break;
    case 0xdc03: u16(w, 1); break; /* protected against writes */
    case 0xdc04: u64(w, entry->size); break;
    case 0xdc07: string(w, entry->display_name); break;
    case 0xdc08: string(w, entry->created); break;
    case 0xdc09: string(w, entry->modified); break;
    case 0xdc0b: u32(w, entry->parent); break;
    case 0xdc41:
        /* Stable across MTP sessions, unlike the ephemeral object handle. */
        u64(w, entry->kind == VCM_MTP_FOLDER ? entry->handle : entry->native_id);
        u32(w, entry->kind);
        u32(w, 0x56434d32u); /* VCM2 namespace */
        break;
    case 0xdc44: string(w, entry->title[0] ? entry->title : entry->display_name); break;
    case 0xdc4f: u8(w, 0); break;
    case 0xdc46: string(w, entry->artist); break;
    case 0xdc9a: string(w, entry->album); break;
    case 0xdc87: u32(w, entry->width); break;
    case 0xdc88: u32(w, entry->height); break;
    default: return -1;
    }
    return w->error ? -1 : (int)w->length;
}
