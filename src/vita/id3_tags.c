#include "id3_tags.h"

static int equals4(const unsigned char *a, const char *b) {
    return a[0] == (unsigned char)b[0] && a[1] == (unsigned char)b[1] &&
           a[2] == (unsigned char)b[2] && a[3] == (unsigned char)b[3];
}

static unsigned int be32(const unsigned char *p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}

static unsigned int be16(const unsigned char *p) {
    return ((unsigned int)p[0] << 8) | p[1];
}

static int jpeg_dimensions(const unsigned char *data, size_t length,
                           int *width, int *height) {
    if (length < 4 || data[0] != 0xff || data[1] != 0xd8) return -1;
    size_t pos = 2;
    while (pos + 4 <= length) {
        if (data[pos++] != 0xff) return -1;
        while (pos < length && data[pos] == 0xff) ++pos;
        if (pos >= length) return -1;
        unsigned int marker = data[pos++];
        if (marker == 0xd9 || marker == 0xda) break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (pos + 2 > length) return -1;
        unsigned int segment = be16(data + pos);
        if (segment < 2 || segment > length - pos) return -1;
        if ((marker == 0xc0 || marker == 0xc2) && segment >= 7) {
            *height = (int)be16(data + pos + 3);
            *width = (int)be16(data + pos + 5);
            return *width > 0 && *height > 0 ? 0 : -1;
        }
        pos += segment;
    }
    return -1;
}

static void parse_jpeg_apic(const unsigned char *payload, size_t length,
                            size_t payload_file_offset, VcmId3Tags *tags) {
    if (length < 18 || tags->cover_size) return;
    unsigned int encoding = payload[0];
    if (encoding > 3) return;
    size_t p = 1, mime_start = p;
    while (p < length && payload[p]) ++p;
    if (p == length || p - mime_start != 10) return;
    const char mime[] = "image/jpeg";
    for (size_t i = 0; i < 10; ++i)
        if (payload[mime_start + i] != (unsigned char)mime[i]) return;
    ++p;
    if (p >= length || payload[p++] != 3) return; /* front cover */
    if (encoding == 0 || encoding == 3) {
        while (p < length && payload[p]) ++p;
        if (p == length) return;
        ++p;
    } else {
        while (p + 1 < length && (payload[p] || payload[p + 1])) p += 2;
        if (p + 1 >= length) return;
        p += 2;
    }
    int width = 0, height = 0;
    if (jpeg_dimensions(payload + p, length - p, &width, &height) != 0) return;
    tags->cover_offset = (long long)(payload_file_offset + p);
    tags->cover_size = (long long)(length - p);
    tags->cover_width = width;
    tags->cover_height = height;
    tags->cover_codec_type = 0x11; /* Sony JPEG image codec enum */
}

static int append_utf8(char *out, size_t capacity, size_t *used, unsigned int code) {
    if (code == 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        return -1;
    size_t n = code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
    if (*used + n >= capacity) return -1;
    if (n == 1) out[(*used)++] = (char)code;
    else if (n == 2) {
        out[(*used)++] = (char)(0xc0 | (code >> 6));
        out[(*used)++] = (char)(0x80 | (code & 0x3f));
    } else if (n == 3) {
        out[(*used)++] = (char)(0xe0 | (code >> 12));
        out[(*used)++] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[(*used)++] = (char)(0x80 | (code & 0x3f));
    } else {
        out[(*used)++] = (char)(0xf0 | (code >> 18));
        out[(*used)++] = (char)(0x80 | ((code >> 12) & 0x3f));
        out[(*used)++] = (char)(0x80 | ((code >> 6) & 0x3f));
        out[(*used)++] = (char)(0x80 | (code & 0x3f));
    }
    out[*used] = 0;
    return 0;
}

static unsigned int utf16_unit(const unsigned char *p, int little_endian) {
    return little_endian ? (unsigned int)p[0] | ((unsigned int)p[1] << 8)
                         : ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

static int decode_text(const unsigned char *data, size_t length,
                       char *out, size_t capacity) {
    if (length < 2 || capacity < 2) return -1;
    out[0] = 0;
    size_t used = 0;
    unsigned int encoding = data[0];
    if (encoding == 0) {
        for (size_t i = 1; i < length && data[i]; ++i)
            if (append_utf8(out, capacity, &used, data[i]) != 0) return -1;
        return used ? 0 : -1;
    }
    if (encoding != 1 || length < 5) return -1;
    int little_endian;
    if (data[1] == 0xff && data[2] == 0xfe) little_endian = 1;
    else if (data[1] == 0xfe && data[2] == 0xff) little_endian = 0;
    else return -1;
    for (size_t i = 3; i + 1 < length; i += 2) {
        unsigned int code = utf16_unit(data + i, little_endian);
        if (!code) break;
        if (code >= 0xd800 && code <= 0xdbff) {
            if (i + 3 >= length) return -1;
            unsigned int low = utf16_unit(data + i + 2, little_endian);
            if (low < 0xdc00 || low > 0xdfff) return -1;
            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
            i += 2;
        }
        if (append_utf8(out, capacity, &used, code) != 0) return -1;
    }
    return used ? 0 : -1;
}

static int parse_track(const char *value, int *track) {
    int result = 0;
    int digits = 0;
    for (const char *p = value; *p >= '0' && *p <= '9'; ++p) {
        if (result > 9999) return -1;
        result = result * 10 + (*p - '0');
        ++digits;
    }
    if (!digits || result <= 0 || result > 9999) return -1;
    *track = result;
    return 0;
}

int vcm_parse_id3v23(const unsigned char *data, size_t length, VcmId3Tags *tags) {
    if (!data || !tags || length < 10 ||
        data[0] != 'I' || data[1] != 'D' || data[2] != '3' ||
        data[3] != 3 || data[4] != 0 || data[5] != 0) return -1;
    for (int i = 6; i < 10; ++i) if (data[i] & 0x80) return -1;
    size_t tag_size = ((size_t)data[6] << 21) | ((size_t)data[7] << 14) |
                      ((size_t)data[8] << 7) | (size_t)data[9];
    if (tag_size > length - 10) return -1;
    volatile unsigned char *out = (volatile unsigned char *)tags;
    for (size_t i = 0; i < sizeof(*tags); ++i) out[i] = 0;
    size_t pos = 10, end = 10 + tag_size;
    while (pos + 10 <= end) {
        const unsigned char *frame = data + pos;
        if (frame[0] == 0 && frame[1] == 0 && frame[2] == 0 && frame[3] == 0)
            break;
        unsigned int frame_size = be32(frame + 4);
        if (frame_size == 0 || frame_size > end - pos - 10) return -1;
        if (frame[8] != 0 || frame[9] != 0) return -1;
        const unsigned char *payload = frame + 10;
        int status = 0;
        if (equals4(frame, "TIT2"))
            status = decode_text(payload, frame_size, tags->title, sizeof(tags->title));
        else if (equals4(frame, "TPE1"))
            status = decode_text(payload, frame_size, tags->artist, sizeof(tags->artist));
        else if (equals4(frame, "TALB"))
            status = decode_text(payload, frame_size, tags->album, sizeof(tags->album));
        else if (equals4(frame, "TPE2"))
            status = decode_text(payload, frame_size, tags->album_artist,
                                 sizeof(tags->album_artist));
        else if (equals4(frame, "TCON"))
            status = decode_text(payload, frame_size, tags->genre, sizeof(tags->genre));
        else if (equals4(frame, "APIC"))
            parse_jpeg_apic(payload, frame_size, pos + 10, tags);
        else if (equals4(frame, "TRCK")) {
            char track_text[32];
            status = decode_text(payload, frame_size, track_text, sizeof(track_text));
            if (status == 0) status = parse_track(track_text, &tags->track);
        }
        if (status != 0) return -1;
        pos += 10 + frame_size;
    }
    if (!tags->title[0] || !tags->artist[0]) return -1;
    if (!tags->album_artist[0]) {
        size_t i = 0;
        while (tags->artist[i] && i + 1 < sizeof(tags->album_artist)) {
            tags->album_artist[i] = tags->artist[i];
            ++i;
        }
        tags->album_artist[i] = 0;
        if (tags->artist[i]) return -1;
    }
    return 0;
}
