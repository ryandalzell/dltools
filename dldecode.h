#ifndef DLCODEC_H
#define DLCODEC_H

#include <stdio.h>
#include <inttypes.h>

extern "C" {
    #include <mpeg2dec/mpeg2.h>
    #include <a52dec/a52.h>
    #include <a52dec/mm_accel.h>
#ifdef HAVE_LIBDE265
    #include <libde265/de265.h>
#endif
#ifdef HAVE_FFMPEG
    #include <libavutil/imgutils.h>
    #include <libavformat/avformat.h>
#endif
}
#include <mpg123.h>

#include "dlutil.h"
#include "dlformat.h"

/* decoder data types */
typedef struct {
    size_t size;
    tstamp_t timestamp;
    unsigned long long decode_time;
    unsigned long long render_time;
} decode_t;


/* virtual base class for decoders */
class dldecode
{
public:
    dldecode();
    virtual ~dldecode();

    virtual int attach(dlformat *format);
    virtual decode_t decode(unsigned char *buffer, size_t bufsize) = 0;

    /* verbose level */
    void set_verbose(int v) { verbose = v; }

protected:
    /* data source */
    dlformat *format;

    /* buffer variables */
    size_t size;
    unsigned char *data;

    /* timestamp variables */
    tstamp_t timestamp;
    long long last_pts;
    int frames_since_pts;

    /* verbose level */
    int verbose;

public: /* yes public, we're not designing a type library here */
    /* video parameters */
    int width;
    int height;
    bool interlaced;
    float framerate;
    pixelformat_t pixelformat;

    /* decoder debug */
public:
    virtual const char *description() { return "unknown"; }

    /* interactive debug */
    virtual void set_field_order(int top_field_first);
    virtual void set_blank_field(int order);

protected:
    int top_field_first;
    int blank_field;
};

/* yuv class */
class dlyuv : public dldecode
{
public:
    dlyuv() { lumaonly = 0; imagesize = NULL; fourcc = NULL; }

    /* yuv specific options */
    void set_lumaonly(int l) { lumaonly = l; }
    void set_imagesize(const char *s) { imagesize = s; }
    void set_fourcc(const char *f) { fourcc = f; }

    virtual int attach(dlformat *format);
    bool atend();
    virtual decode_t decode(unsigned char *buffer, size_t bufsize);

public:
    virtual const char *description() { return "yuv video"; }

private:
    unsigned maxframes;

    /* display parameters */
    int lumaonly;
    const char *imagesize;
    const char *fourcc;
};

/* libmpeg2 class */
class dlmpeg2 : public dldecode
{
public:
    dlmpeg2();
    ~dlmpeg2();

    virtual int attach(dlformat *format);
    bool atend();
    virtual decode_t decode(unsigned char *buffer, size_t bufsize);

public:
    virtual const char *description() { return "mpeg2 video"; }

protected:
    /* libmpeg2 variables */
    mpeg2dec_t *mpeg2dec;
    const mpeg2_info_t *info;
};

/* mpg123 class */
class dlmpg123 : public dldecode
{
public:
    dlmpg123();
    ~dlmpg123();

    virtual int attach(dlformat *format);
    virtual decode_t decode(unsigned char *frame, size_t size);

public:
    virtual const char *description() { return "mpeg1 audio"; }

private:
    /* mpg123 variables */
    mpg123_handle *m;
    int ret;
};

/* liba52 class */
class dlliba52 : public dldecode
{
public:
    dlliba52();
    ~dlliba52();

    virtual int attach(dlformat *format);
    virtual decode_t decode(unsigned char *frame, size_t size);

public:
    virtual const char *description() { return "mpeg2 ac3 audio"; }

private:
    /* liba52 variables */
    a52_state_t *a52_state;
    sample_t *sample;
    int ac3_length;
    unsigned char *ac3_frame;
    size_t ac3_size;
    int16_t *ac3_block;

    /* transport stream variables */
    int frames_since_pts;

};

/* libde265 class */
#ifdef HAVE_LIBDE265
class dlhevc : public dldecode
{
public:
    dlhevc();
    ~dlhevc();

    virtual int attach(dlformat *format);
    bool atend();
    virtual decode_t decode(unsigned char *buffer, size_t bufsize);

public:
    virtual const char *description() { return "hevc video"; }

protected:
    /* libde265 variables */
    de265_error err;
    de265_decoder_context* ctx;
    const struct de265_image *image;
};
#endif

/* ffmpeg class */
class dlffmpeg : public dldecode
{
public:
    dlffmpeg();
    ~dlffmpeg();

    virtual int attach(dlformat *format);
    bool atend();
    virtual decode_t decode(unsigned char *buffer, size_t bufsize);

public:
    virtual const char *description() { return codeccontext->codec->long_name; }

protected:
    /* ffmpeg variables */
    AVFormatContext *formatcontext;
    AVCodecContext *codeccontext;
    AVFrame *frame;
    AVPacket packet;
    uint8_t *image[4];
    int linesizes[4];

    /* error string */
    char *errorstring;
};

#endif
