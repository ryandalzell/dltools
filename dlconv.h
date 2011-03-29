#ifndef DLCONV_H
#define DLCONV_H

#include "dlutil.h"

void convert_i420_uyvy(const unsigned char *i420, unsigned char *uyvy, int width, int height, pixelformat_t pixelformat);
void convert_yuv_uyvy(const unsigned char *yuv[3], unsigned char *uyvy, int width, int height, pixelformat_t pixelformat);
void convert_i420_uyvy_lumaonly(const unsigned char *i420, unsigned char *uyvy, int width, int height);

#endif
