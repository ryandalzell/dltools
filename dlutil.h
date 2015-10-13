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

int divine_video_format(const char *filename, int *width, int *height, bool *interlaced, float *framerate, pixelformat_t *pixelformat);
unsigned long long int get_time();
unsigned long long int get_utime();
tstamp_t get_stime();

#endif
