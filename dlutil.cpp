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
