#ifndef DLFORMAT_H
#define DLFORMAT_H

#include "dlutil.h"
#include "dlsource.h"
extern "C" {
#ifdef HAVE_FFMPEG
        #include <libavformat/avformat.h>
#endif
}

/* virtual base class for data format decoders */
class dlformat
{
public:
    dlformat();
    virtual ~dlformat();

    /* format operators */
    virtual int attach(dlsource *source);

    /* copy to buffer read */
    virtual size_t read(unsigned char *buf, size_t bytes);
    /* zero copy read (depending on implementation) */
    virtual const unsigned char *read(size_t *bytes);

    /* return most recent timestamp */
    virtual long long get_pts();
    virtual long long get_dts();

    /* expose source interfaces */
    dlsource *get_source();

    /* format metadata */
    virtual const char *description() { return "raw"; }

protected:
    /* data source */
    dlsource *source;
    dltoken_t token;

    /* buffer variables */
    size_t size;
    unsigned char *data;

};

/* elementary stream format decoder class */
class dlestream : public dlformat
{
public:
    /* format metadata */
    virtual const char *description() { return "elementary stream"; }
};

/* transport stream format decoder class */
class dltstream : public dlformat
{
public:
    dltstream(int pid);
    virtual ~dltstream();

    /* format operators */
    virtual int attach(dlsource *source);

    /* copy to buffer read */
    virtual size_t read(unsigned char *buf, size_t bytes);
    /* zero copy read (depending on implementation) */
    virtual const unsigned char *read(size_t *bytes);

    /* return most recent pts */
    virtual long long get_pts();
    virtual long long get_dts();

    /* format metadata */
    virtual const char *description() { return "transport stream"; }

protected:
    int pid;
    long long pts, dts;
};

#ifdef HAVE_FFMPEG
/* ffmpeg (libavformat) format decoder class */
class dlavformat : public dlformat
{
public:
    dlavformat();
    ~dlavformat();

    /* format operators */
    virtual int attach(dlsource *source);

    /* copy to buffer read */
    virtual size_t read(unsigned char *buf, size_t bytes);

    /* format metadata */
    virtual const char *description() { return formatcontext->iformat->long_name; }

public:
    AVFormatContext *formatcontext;
    AVIOContext *iocontext;

protected:
    /* error string */
    char *errorstring;
};
#endif

#endif
