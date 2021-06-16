#ifndef DLUTIL_H
#define DLUTIL_H

#include "DeckLinkAPI.h"

/* file types */
typedef enum {
    OTHER,
    YUV,
    YUV4MPEG,
    M2V,
    AVC,
    HEVC,
    TS,
    FFMPEG, // use libav for decoding.
} filetype_t;

/* pixel formats */
typedef enum {
    UNKNOWN = 0,
    I420,
    I422,
    UYVY
} pixelformat_t;

/* timestamp */
typedef long long tstamp_t;

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
tstamp_t get_stime();
const char *describe_timestamp(tstamp_t t);
const char *describe_filetype(filetype_t f);

#endif
