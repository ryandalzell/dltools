/*
 * Description: yuv conversion functions
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2011 4i2i Communications Ltd.
 */

#include "dlutil.h"

#ifdef HAVE_LIBYUV
#include <libyuv.h>
#endif

static void convert_i444_uyvy(const unsigned char *i444, unsigned char *uyvy, int width, int height)
{
    const unsigned char *yuv[3] = {i444};
    for (int y=0; y<height; y++) {
        yuv[1] = i444 + width*height + width*y;
        yuv[2] = i444 + width*height*2 + width*y;
        for (int x=0; x<width/2; x++) {
            *(uyvy++) = *yuv[1];
            *(uyvy++) = *(yuv[0]++);
            *(uyvy++) = *yuv[2];
            *(uyvy++) = *(yuv[0]++);
            yuv[1] += 2;
            yuv[2] += 2;
        }
    }
}

void convert_i420_uyvy(const unsigned char *i420, unsigned char *uyvy, int width, int height, pixelformat_t pixelformat)
{
    const unsigned char *yuv[3] = {i420};

    if (pixelformat==I444)
        return convert_i444_uyvy(i420, uyvy, width, height);

    /* otherwise for 4:2:0 and 4:2:2 */
    for (int y=0; y<height; y++) {
        if (pixelformat==I422) {
            yuv[1] = i420 + width*height + (width/2)*y;
            yuv[2] = i420 + width*height*6/4 + (width/2)*y;
        } else {
            yuv[1] = i420 + width*height + (width/2)*(y/2);
            yuv[2] = i420 + width*height*5/4 + (width/2)*(y/2);
        }
        for (int x=0; x<width/2; x++) {
            *(uyvy++) = *(yuv[1]++);
            *(uyvy++) = *(yuv[0]++);
            *(uyvy++) = *(yuv[2]++);
            *(uyvy++) = *(yuv[0]++);
        }
    }
}

void convert_yuv_uyvy(const unsigned char *yuv[3], unsigned char *uyvy, int width, int height, pixelformat_t pixelformat)
{
#ifndef HAVE_LIBYUV
    const unsigned char *ptr[3] = {yuv[0]};
    for (int y=0; y<height; y++) {
        if (pixelformat==I422) {
            ptr[1] = yuv[1] + (width/2)*y;
            ptr[2] = yuv[2] + (width/2)*y;
        } else {
            ptr[1] = yuv[1] + (width/2)*(y/2);
            ptr[2] = yuv[2] + (width/2)*(y/2);
        }
        for (int x=0; x<width/2; x++) {
            *(uyvy++) = *(ptr[1]++);
            *(uyvy++) = *(ptr[0]++);
            *(uyvy++) = *(ptr[2]++);
            *(uyvy++) = *(ptr[0]++);
        }
    }
#else
    switch (pixelformat) {
        case I420: libyuv::I420ToUYVY(yuv[0], width, yuv[1], width/2, yuv[2], width/2, uyvy, 2*width, width, height); break;
        case I422: libyuv::I422ToUYVY(yuv[0], width, yuv[1], width/2, yuv[2], width/2, uyvy, 2*width, width, height); break;
        case I444: convert_i444_uyvy(yuv[0], uyvy, width, height); break;   /* no suitable accelerated routine in libyuv */
        default  : dlerror("unknown pixel format in conversion: %s", pixelformatname[pixelformat]);
    }
#endif
}

void convert_i420_uyvy_lumaonly(const unsigned char *i420, unsigned char *uyvy, int width, int height)
{
    for (int y=0; y<height; y++) {
        for (int x=0; x<width/2; x++) {
            *(uyvy++) = 0x80;
            *(uyvy++) = *(i420++);
            *(uyvy++) = 0x80;
            *(uyvy++) = *(i420++);
        }
    }
}

void convert_field_yuv_uyvy(const unsigned char *top[3], const unsigned char *bot[3], unsigned char *uyvy, int width, int height, pixelformat_t pixelformat)
{
    const unsigned char *ptr[2][3] = {{top[0]}, {bot[0]}};
    for (int y=0; y<height/2; y++) {
        if (pixelformat==I422) {
            ptr[0][1] = top[1] + (width/2)*y;
            ptr[0][2] = top[2] + (width/2)*y;
            ptr[1][1] = bot[1] + (width/2)*y;
            ptr[1][2] = bot[2] + (width/2)*y;
        } else {
            ptr[0][1] = top[1] + (width/2)*(y/2);
            ptr[0][2] = top[2] + (width/2)*(y/2);
            ptr[1][1] = bot[1] + (width/2)*(y/2);
            ptr[1][2] = bot[2] + (width/2)*(y/2);
        }
        /* first field */
        for (int x=0; x<width/2; x++) {
            *(uyvy++) = *(ptr[0][1]++);
            *(uyvy++) = *(ptr[0][0]++);
            *(uyvy++) = *(ptr[0][2]++);
            *(uyvy++) = *(ptr[0][0]++);
        }
        /* second field */
        for (int x=0; x<width/2; x++) {
            *(uyvy++) = *(ptr[1][1]++);
            *(uyvy++) = *(ptr[1][0]++);
            *(uyvy++) = *(ptr[1][2]++);
            *(uyvy++) = *(ptr[1][0]++);
        }
    }
}

void convert_top_field_yuv_uyvy(const unsigned char *top[3], unsigned char *uyvy, int width, int height, pixelformat_t pixelformat)
{
#ifndef HAVE_LIBYUV
    const unsigned char *ptr[3] = {top[0]};
    for (int y=0; y<height/2; y++) {
        if (pixelformat==I422) {
            ptr[1] = top[1] + (width/2)*y;
            ptr[2] = top[2] + (width/2)*y;
        } else {
            ptr[1] = top[1] + (width/2)*(y/2);
            ptr[2] = top[2] + (width/2)*(y/2);
        }
        /* first field */
        for (int x=0; x<width/2; x++) {
            *(uyvy++) = *(ptr[1]++);
            *(uyvy++) = *(ptr[0]++);
            *(uyvy++) = *(ptr[2]++);
            *(uyvy++) = *(ptr[0]++);
        }
        /* second field */
        uyvy += 2*width;
    }
#else
    switch (pixelformat) {
        case I420: libyuv::I420ToUYVY(top[0], width, top[1], width/2, top[2], width/2, uyvy, 4*width, width, height/2); break;
        case I422: libyuv::I422ToUYVY(top[0], width, top[1], width/2, top[2], width/2, uyvy, 4*width, width, height/2); break;
        default  : dlerror("unknown pixel format in conversion: %s", pixelformatname[pixelformat]);
    }
#endif
}

void convert_bot_field_yuv_uyvy(const unsigned char *bot[3], unsigned char *uyvy, int width, int height, pixelformat_t pixelformat)
{
#ifndef HAVE_LIBYUV
    const unsigned char *ptr[3] = {bot[0]};
    for (int y=0; y<height/2; y++) {
        if (pixelformat==I422) {
            ptr[1] = bot[1] + (width/2)*y;
            ptr[2] = bot[2] + (width/2)*y;
        } else {
            ptr[1] = bot[1] + (width/2)*(y/2);
            ptr[2] = bot[2] + (width/2)*(y/2);
        }
        /* first field */
        uyvy += 2*width;
        /* second field */
        for (int x=0; x<width/2; x++) {
            *(uyvy++) = *(ptr[1]++);
            *(uyvy++) = *(ptr[0]++);
            *(uyvy++) = *(ptr[2]++);
            *(uyvy++) = *(ptr[0]++);
        }
    }
#else
    switch (pixelformat) {
        case I420: libyuv::I420ToUYVY(bot[0], width, bot[1], width/2, bot[2], width/2, uyvy+2*width, 4*width, width, height/2); break;
        case I422: libyuv::I422ToUYVY(bot[0], width, bot[1], width/2, bot[2], width/2, uyvy+2*width, 4*width, width, height/2); break;
        default  : dlerror("unknown pixel format in conversion: %s", pixelformatname[pixelformat]);
    }
#endif
}
