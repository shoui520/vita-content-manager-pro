#ifndef VCM_ID3_TAGS_H
#define VCM_ID3_TAGS_H

#include <stddef.h>

typedef struct {
    char title[256];
    char artist[256];
    char album[256];
    char album_artist[256];
    char genre[128];
    int track;
    long long cover_offset;
    long long cover_size;
    int cover_width;
    int cover_height;
    int cover_codec_type;
} VcmId3Tags;

/* Parse a complete ID3v2.3 tag, including the 10-byte file header.
 * Returns 0 only for a structurally valid tag with title and artist. */
int vcm_parse_id3v23(const unsigned char *data, size_t length, VcmId3Tags *tags);

#endif
