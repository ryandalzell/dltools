#ifndef DLFORMAT_H
#define DLFORMAT_H

#include "dlutil.h"
#include "dlsource.h"
#include "dlts.h"
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
    virtual int rewind() { return source->rewind(); }
    virtual int attach(dlsource *source);

    /* copy to buffer read */
    virtual size_t read(unsigned char *buf, size_t bytes);
    /* zero copy read (depending on implementation) */
    virtual const unsigned char *read(size_t *bytes);

    /* return most recent timestamp */
    virtual long long get_pts();
    virtual long long get_dts();

    /* true when the data just read does not continue the data before it, so that
       a decoder can discard what it has buffered instead of joining the two */
    virtual bool discontinuity() { return false; }

    /* expose source interfaces */
    //virtual const char *description() { return source->description(); }
    virtual const char *name() { return source->name(); }
    virtual size_t filesize() { return source->size(); }
    virtual off_t pos() { return source->pos(); }
    virtual bool eof() { return source->eof(); }
    virtual bool error() { return source->error(); }

    /* format metadata */
    virtual const char *description() { return "raw"; }

    /* video format, if the container carries it, otherwise -1 */
    virtual int get_video_format(int *width, int *height, bool *interlaced, float *framerate, pixelformat_t *pixelformat) { return -1; }

protected:
    /* this format's own reader on the data source */
    dlsource *source;

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

/* yuv4mpeg2 format decoder class */
class dly4m : public dlformat
{
public:
    dly4m();

    /* format operators */
    virtual int rewind();
    virtual int attach(dlsource *source);

    /* copy to buffer read */
    virtual size_t read(unsigned char *buf, size_t bytes);
    /* zero copy read (depending on implementation) */
    virtual const unsigned char *read(size_t *bytes);

    /* the video format is carried in the stream header */
    virtual int get_video_format(int *width, int *height, bool *interlaced, float *framerate, pixelformat_t *pixelformat);

    /* report size and position of the frame data, excluding the headers */
    virtual size_t filesize() { return numframes*framesize; }
    virtual off_t pos() { return frames_read*framesize; }

    /* format metadata */
    virtual const char *description() { return "yuv4mpeg2"; }

protected:
    /* header parsers */
    size_t read_header(char *header, size_t maxlen);
    int read_stream_header();
    int read_frame_header();

    /* video format from the stream header */
    int width, height;
    bool interlaced;
    float framerate;
    pixelformat_t pixelformat;

    /* stream geometry */
    size_t framesize;
    size_t streamhdrsize, framehdrsize;
    unsigned numframes, frames_read;
};

/* transport stream format decoder class */
class dltstream : public dlformat
{
public:
    dltstream(int pid);
    virtual ~dltstream();

    /* format operators */
    /* attach to a demux shared with the filters for the other pids */
    virtual int attach(dldemux *demux);
    /* attach to a source directly, with a demux of our own for this pid alone */
    virtual int attach(dlsource *source);
    virtual int rewind();

    /* copy to buffer read */
    virtual size_t read(unsigned char *buf, size_t bytes);
    /* zero copy read (depending on implementation) */
    virtual const unsigned char *read(size_t *bytes);

    /* return most recent pts */
    virtual long long get_pts();
    virtual long long get_dts();
    virtual bool discontinuity() { return packet.discontinuity; }

    /* format metadata */
    virtual const char *description() { return "transport stream"; }

protected:
    int pid;

    /* the demux this pid is read from, and whether it is ours to delete */
    dldemux *demux;
    bool own_demux;

    /* the pes packet most recently read, owned by us */
    pespacket_t packet;
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
