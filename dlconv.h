#ifndef DLCONV_H
#define DLCONV_H

#include "dlutil.h"

void convert_i420_uyvy(const unsigned char *i420, unsigned char *uyvy, int width, int height, pixelformat_t pixelformat);
void convert_yuv_uyvy(const unsigned char *yuv[3], unsigned char *uyvy, int width, int height, pixelformat_t pixelformat);
void convert_i420_uyvy_lumaonly(const unsigned char *i420, unsigned char *uyvy, int width, int height);
void convert_field_yuv_uyvy(const unsigned char *top[3], const unsigned char *bot[3], unsigned char *uyvy, int width, int height, pixelformat_t pixelformat);
void convert_top_field_yuv_uyvy(const unsigned char *top[3], unsigned char *uyvy, int width, int height, pixelformat_t pixelformat);
void convert_bot_field_yuv_uyvy(const unsigned char *bot[3], unsigned char *uyvy, int width, int height, pixelformat_t pixelformat);

#endif
