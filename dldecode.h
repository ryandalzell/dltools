#ifndef DLCODEC_H
#define DLCODEC_H

#include <stdio.h>
#include <inttypes.h>

extern "C" {
    #include <mpeg2dec/mpeg2.h>
    #include <mpeg2dec/mpeg2convert.h>
    #include <a52dec/a52.h>
    #include <a52dec/mm_accel.h>
}
#include <mpg123.h>

#include "dlutil.h"

/* virtual base class for decoders */
class dldecode
{
public:
    dldecode();
    virtual ~dldecode();

    virtual int attach(const char *filename);
    virtual int rewind(int frame);
    virtual size_t decode(unsigned char *uyvy, size_t size) = 0;

protected:
    /* file and buffer variables */
    const char *filename;
    FILE *file;
    size_t size;
    unsigned char *data;

public: /* yes public, we're not designing a type library here */
    /* video parameters */
    int width;
    int height;
    bool interlaced;
    float framerate;
    pixelformat_t pixelformat;
};

/* yuv class */
class dlyuv : public dldecode
{
public:
    dlyuv() { lumaonly = 0; }

    virtual int attach(const char *filename);
    bool atend();
    virtual int rewind(int frame);
    virtual size_t decode(unsigned char *uyvy, size_t size);

public:

private:
    int maxframes;

    /* display parameters */
    int lumaonly;
};

/* libmpeg2 class */
class dlmpeg2 : public dldecode
{
public:
    dlmpeg2();
    ~dlmpeg2();

    virtual int attach(const char *filename);
    bool atend();
    virtual size_t decode(unsigned char *uyvy, size_t size);

public:

protected:
    virtual int readdata();

    /* libmpeg2 variables */
    mpeg2dec_t *mpeg2dec;
    const mpeg2_info_t *info;

};

/* libmpeg2 class for transport streams */
class dlmpeg2ts : public dlmpeg2
{
public:
    dlmpeg2ts();

    virtual int attach(const char *filename);

public:
    int pid;

protected:
    virtual int readdata();

private:
    int findpid();
};

/* mpg123 class */
class dlmpg123 : public dldecode
{
public:
    dlmpg123();
    ~dlmpg123();

    virtual int attach(const char *filename);
    virtual size_t decode(unsigned char *frame, size_t size);

public:
    int pid;

private:
    /* mpg123 variables */
    mpg123_handle *m;
    int ret;
};

/* libmpeg2 class */
class dlliba52 : public dldecode
{
public:
    dlliba52();
    ~dlliba52();

    virtual int attach(const char *filename);
    virtual size_t decode(unsigned char *frame, size_t size);

public:
    int pid;

private:
    /* liba52 variables */
    a52_state_t *a52_state;
    sample_t *sample;
    int ac3_length;
    unsigned char *ac3_frame;
    size_t ac3_size;
    int16_t *ac3_block;

};

#endif
