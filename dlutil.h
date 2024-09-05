#ifndef DLUTIL_H
#define DLUTIL_H

#include <stddef.h>

#include "DeckLinkAPI.h"

/* file types */
typedef enum {
    OTHER,
    YUV,
    YUV4MPEG,
    M2V,
    M4V,
    AVC,
    HEVC,
    AV1,
    TS,
    FFMPEG, // use libav for decoding.
} filetype_t;

/* pixel formats */
typedef enum {
    UNKNOWN = 0,
    I420,
    I422,
    I444,
    UYVY,
    YU15,
    YU20
} pixelformat_t;

/* timestamp */
typedef long long pts_t;        /* presentation timestamp in 90kHz */
typedef long long sts_t;        /* system timestamp in 180kHz */

#define mmax(a, b) ((a) > (b) ? (a) : (b))
#define mmin(a, b) ((a) < (b) ? (a) : (b))

extern const char *pixelformatname[];

void dlapierror(HRESULT result, const char *format, ...);
void dlerror(const char *format, ...);
void dlexit(const char *format, ...);
void dlmessage(const char *format, ...);
void dlstatus(const char *format, ...);
void dlabort(const char *format, ...);

int divine_pixel_format(const char *filename, pixelformat_t *pixelformat);
int divine_video_format(const char *filename, int *width, int *height, bool *interlaced, float *framerate);
unsigned long long int get_time();
unsigned long long int get_utime();
pts_t get_ptstime();
sts_t get_ststime();
const char *describe_pts(pts_t t);
const char *describe_sts(sts_t t);
const char *describe_filetype(filetype_t f);
size_t pixelformat_get_size(pixelformat_t pixelformat, int width, int height);
bool pixelformat_is_8bit(pixelformat_t pixelformat);

#endif
