#ifndef DLUTIL_H
#define DLUTIL_H

#include "DeckLinkAPI.h"

/* file types */
typedef enum {
    OTHER,
    YUV,
    YUV4MPEG,
    TS,
    PS,
    PES,
    M2V,
} filetype_t;

/* pixel formats */
typedef enum {
    UNKNOWN = 0,
    I420,
    I422,
    UYVY
} pixelformat_t;

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

#endif
