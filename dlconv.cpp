/*
 * Description: yuv conversion functions
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2011 4i2i Communications Ltd.
 */

#include "dlutil.h"

void convert_i420_uyvy(const unsigned char *i420, unsigned char *uyvy, int width, int height, pixelformat_t pixelformat)
{
    const unsigned char *yuv[3] = {i420};
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
