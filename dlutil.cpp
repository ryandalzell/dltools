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

#include "dlutil.h"

extern const char *appname;

void dlerror(const char *format, ...)
{
    va_list ap;
    char message[256];
    int exitcode = errno;

    /* start output with application name */
    int len = snprintf(message, sizeof(message), "%s: ", appname);

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

    /* start output with application name */
    int len = snprintf(message, sizeof(message), "%s: ", appname);

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

    /* start output with application name */
    int len = snprintf(message, sizeof(message), "%s: ", appname);

    /* print message */
    va_start(ap, format);
    vsnprintf(message+len, sizeof(message)-len, format, ap);
    fprintf(stderr, "%s\n", message);
    va_end(ap);
}

void dlabort(const char *format, ...)
{
    va_list ap;
    char message[256];

    /* start output with application name */
    int len = snprintf(message, sizeof(message), "%s: ", appname);

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
    "UYVY"
};

int divine_video_format(const char *filename, int *width, int *height, bool *interlaced, float *framerate, pixelformat_t *pixelformat)
{
    struct format_t {
        const char *name;
        int width;
        int height;
        bool interlaced;
        float framerate;
    };

    const struct format_t formats[] = {
        { "1080p24",    1920, 1080, false, 24.0 },
        { "1080p25",    1920, 1080, false, 25.0 },
        { "1080p30",    1920, 1080, false, 30.0 },
        { "1080p",      1920, 1080, false, 30.0 },
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

    if (strstr(filename, "uyvy")!=NULL || strstr(filename, "UYVY")!=NULL)
        *pixelformat = UYVY;
    else if (strstr(filename, "422")!=NULL)
        *pixelformat = I422;
    else
        *pixelformat = I420;

    for (unsigned i=0; i<sizeof(formats)/sizeof(struct format_t); i++) {
        struct format_t f = formats[i];

        if (strstr(filename, f.name)!=NULL) {
            *width = f.width;
            *height = f.height;
            *interlaced = f.interlaced;
            *framerate = f.framerate;
            return 0;
        }
    }
    return -1;
}
