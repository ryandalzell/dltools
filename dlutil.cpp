/*
 * Description: Common functions for dltools applications.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2010 4i2i Communications Ltd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <sys/time.h>

#include "dlutil.h"

extern const char *appname;
static int not_at_home;

void dlapierror(HRESULT result, const char *format, ...)
{
    va_list ap;
    char message[256];
    int len = 0;

    /* move cursor to home position */
    if (not_at_home) {
        len += snprintf(message+len, sizeof(message)-len, "\n");
        not_at_home = 0;
    }

    /* start output with application name */
    len += snprintf(message+len, sizeof(message)-len, "%s: ", appname);

    /* print message and system error message */
    va_start(ap, format);
    vsnprintf(message+len, sizeof(message)-len, format, ap);
    fprintf(stderr, "%s: ", message);
    va_end(ap);

    /* append with api error message */
    const char *apimessage;
    switch (result) {
        case S_OK           : apimessage = "success"; break;
        case S_FALSE        : apimessage = "last in list"; break;
        case E_UNEXPECTED   : apimessage = "unexpected use of api"; break;
        case E_NOTIMPL      : apimessage = "not implemented"; break;
        case E_OUTOFMEMORY  : apimessage = "out of memory"; break;
        case E_INVALIDARG   : apimessage = "invalid argument"; break;
        case E_NOINTERFACE  : apimessage = "interface was not found"; break;
        case E_POINTER      : apimessage = "invalid pointer argument"; break;
        case E_HANDLE       : apimessage = "invalid handle argument"; break;
        case E_ABORT        : apimessage = "abort"; break;
        case E_FAIL         : apimessage = "failure"; break;
        case E_ACCESSDENIED : apimessage = "access denied"; break;
        default             : apimessage = "unknown error"; break;
    }
    fprintf(stderr, "%s\n", apimessage);

    exit(2);
}

void dlerror(const char *format, ...)
{
    va_list ap;
    char message[256];
    int len = 0;
    int exitcode = errno;

    /* move cursor to home position */
    if (not_at_home) {
        len += snprintf(message+len, sizeof(message)-len, "\n");
        not_at_home = 0;
    }

    /* start output with application name */
    len += snprintf(message+len, sizeof(message)-len, "%s: ", appname);

    /* print message and system error message */
    va_start(ap, format);
    vsnprintf(message+len, sizeof(message)-len, format, ap);
    perror(message);
    va_end(ap);

    exit(exitcode);
}

void dlexit(const char *format, ...)
{
    va_list ap;
    char message[256];
    int len = 0;

    /* move cursor to home position */
    if (not_at_home) {
        len += snprintf(message+len, sizeof(message)-len, "\n");
        not_at_home = 0;
    }

    /* start output with application name */
    len += snprintf(message+len, sizeof(message)-len, "%s: ", appname);

    /* print message */
    va_start(ap, format);
    vsnprintf(message+len, sizeof(message)-len, format, ap);
    fprintf(stderr, "%s\n", message);
    va_end(ap);

    exit(1);
}

void dlmessage(const char *format, ...)
{
    va_list ap;
    char message[256];
    int len = 0;

    /* move cursor to home position */
    if (not_at_home) {
        len += snprintf(message+len, sizeof(message)-len, "\n");
        not_at_home = 0;
    }

    /* start output with application name */
    len += snprintf(message+len, sizeof(message)-len, "%s: ", appname);

    /* print message */
    va_start(ap, format);
    vsnprintf(message+len, sizeof(message)-len, format, ap);
    fprintf(stderr, "%s\n", message);
    va_end(ap);
}

void dlstatus(const char *format, ...)
{
    va_list ap;
    char message[256];

    /* start output with carriage return and application name */
    int len = snprintf(message, sizeof(message), "\r%s: ", appname);

    /* print message */
    va_start(ap, format);
    vsnprintf(message+len, sizeof(message)-len, format, ap);
    fprintf(stderr, "%s", message);
    va_end(ap);

    /* mark cursor not at home position */
    not_at_home = 1;
}

void dlabort(const char *format, ...)
{
    va_list ap;
    char message[256];
    int len = 0;

    /* move cursor to home position */
    if (not_at_home) {
        len += snprintf(message+len, sizeof(message)-len, "\n");
        not_at_home = 0;
    }

    /* start output with application name */
    len += snprintf(message+len, sizeof(message)-len, "%s: ", appname);

    /* print message */
    va_start(ap, format);
    vsnprintf(message+len, sizeof(message)-len, format, ap);
    fprintf(stderr, "%s\n", message);
    va_end(ap);

    abort();
}

/* pixel format strings */
const char *pixelformatname[] = {
    "",
    "I420",
    "I422",
    "I444",
    "UYVY",
    "YU15",
    "YU20"
};

int divine_pixel_format(const char *filename, pixelformat_t *pixelformat)
{
    if (!filename || !pixelformat)
        return -1;

    if (strstr(filename, "uyvy")!=NULL || strstr(filename, "UYVY")!=NULL)
        *pixelformat = UYVY;
    else if (strstr(filename, "yu15")!=NULL || strstr(filename, "YU15")!=NULL)
        *pixelformat = YU20;
    else if (strstr(filename, "yu20")!=NULL || strstr(filename, "YU20")!=NULL)
        *pixelformat = YU20;
    else if (strstr(filename, "444")!=NULL)
        *pixelformat = I444;
    else if (strstr(filename, "422")!=NULL)
        *pixelformat = I422;
    else if (strstr(filename, "420")!=NULL)
        *pixelformat = I420;
    else
        /* couldn't find a pixel format string */
        return -1;

    return 0;
}

int divine_video_format(const char *filename, int *width, int *height, bool *interlaced, float *framerate)
{
    /* sanity check */
    if (!filename)
        return -1;

    struct format_t {
        const char *name;
        int width;
        int height;
        bool interlaced;
        float framerate;
    };

    const struct format_t formats[] = {
        { "2160p60",    3840, 2160, false, 60 },
        { "2160p5994",  3840, 2160, false, 60000.0/1001.0 },
        { "2160p50",    3840, 2160, false, 50 },
        { "2160p30",    3840, 2160, false, 30 },
        { "2160p2997",  3840, 2160, false, 30000.0/1001.0 },
        { "2160p25",    3840, 2160, false, 25 },
        { "2160p24",    3840, 2160, false, 24 },
        { "2160p2398",  3840, 2160, false, 24000.0/1001.0 },
        { "2160p",      3840, 2160, false, 60000.0/1001.0 },
        { "1080p60",    1920, 1080, false, 60.0 },
        { "1080p5994",  1920, 1080, false, 60000.0/1001.0 },
        { "1080p50",    1920, 1080, false, 50.0 },
        { "1080p30",    1920, 1080, false, 30.0 },
        { "1080p2997",  1920, 1080, false, 30000.0/1001.0 },
        { "1080p25",    1920, 1080, false, 25.0 },
        { "1080p24",    1920, 1080, false, 24.0 },
        { "1080p",      1920, 1080, false, 30000.0/1001.0 },
        { "1080i50",    1920, 1080, true,  25.0 },
        { "1080i59.94", 1920, 1080, true,  30000.0/1001.0 },
        { "1080i5994",  1920, 1080, true,  30000.0/1001.0 },
        { "1080i60",    1920, 1080, true,  30.0 },
        { "1080i",      1920, 1080, true,  30000.0/1001.0 },
        { "720p50",     1280, 720,  false, 50.0 },
        { "720p60",     1280, 720,  false, 60.0 },
        { "720p59.94",  1280, 720,  false, 60000.0/1001.0 },
        { "720p5994",   1280, 720,  false, 60000.0/1001.0 },
        { "720p",       1280, 720,  false, 60000.0/1001.0 },
        { "576i50",     720,  576,  true,  25.0 },
        { "576i",       720,  576,  true,  25.0 },
        { "pal",        720,  576,  true,  25.0 },
        { "480i60",     720,  480,  true,  30.0 },
        { "480i59.94",  720,  480,  true,  30000.0/1001.0 },
        { "480i5994",   720,  480,  true,  30000.0/1001.0 },
        { "480i",       720,  480,  true,  30000.0/1001.0 },
        { "ntsc",       720,  480,  true,  30000.0/1001.0 },
        { "hdv",        1440, 1080, true,  30.0 },
        { "cif",        352,  288,  true,  30.0 },
    };

    for (unsigned i=0; i<sizeof(formats)/sizeof(struct format_t); i++) {
        struct format_t f = formats[i];

        if (strstr(filename, f.name)!=NULL) {
            if (width     ) *width      = f.width;
            if (height    ) *height     = f.height;
            if (interlaced) *interlaced = f.interlaced;
            if (framerate ) *framerate  = f.framerate;
            return 0;
        }
    }
    return -1;
}

/* return current time in msecs */
unsigned long long int get_time()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long int)(tv.tv_usec/1000) + tv.tv_sec*1000ll;
}

/* return current time in usecs */
unsigned long long int get_utime()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long int)(tv.tv_usec) + tv.tv_sec*1000000ll;
}

/* return current time in 90kHz */
pts_t get_ptstime()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (pts_t)(tv.tv_usec*9/100) + tv.tv_sec*90000ll;
}

/* return current time in 180kHz */
sts_t get_ststime()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (sts_t)(tv.tv_usec*9/50) + tv.tv_sec*180000ll;
}

/* breakdown a 90kHz timestamp in human readable terms */
const char *describe_pts(pts_t t)
{
    /* not thread safe */
    static char s[32];

    t /= 90;
    int msec = t % 1000;
    t /= 1000;
    int sec = t % 60;
    t /= 60;
    int min = t % 60;
    t /= 60;
    int hour = t % 60;

    snprintf(s, sizeof(s), "%02d:%02d:%02d.%03d", hour, min, sec, msec);

    return s;
}

/* breakdown a 180kHz timestamp in human readable terms */
const char *describe_sts(sts_t t)
{
    /* not thread safe */
    static char s[32];

    t /= 180;
    int msec = t % 1000;
    t /= 1000;
    int sec = t % 60;
    t /= 60;
    int min = t % 60;
    t /= 60;
    int hour = t % 60;

    snprintf(s, sizeof(s), "%02d:%02d:%02d.%03d", hour, min, sec, msec);

    return s;
}

const char *describe_filetype(filetype_t f)
{
    static const char *filetype_names[] = {
    "other",
    "yuv",
    "yuv4mpeg",
    "m2v",
    "m4v",
    "avc",
    "hevc",
    "av1"
    "ts",
    "ffmpeg",
    };

    return filetype_names[f];
}

size_t pixelformat_get_size(pixelformat_t pixelformat, int width, int height)
{
    switch (pixelformat) {
        case I420: return width*height*3/2;
        case I422:
        case UYVY: return width*height*2;
        case I444: return width*height*3;
        case YU15: return 2*width*height + 4*width*height/4;
        case YU20: return 2*width*height + 4*width*height/2;
        case UNKNOWN: dlexit("unknown pixelformat: %d", pixelformat);
    }
    return 0;
}

bool pixelformat_is_8bit(pixelformat_t pixelformat)
{
    if (pixelformat==YU15 || pixelformat==YU20)
        return 0;
    return 1;
}
