/*
 * Description: container format decoder.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2015 4i2i Communications Ltd.
 */

#include <stdlib.h>
#include <assert.h>

#include "dlformat.h"
#include "dlts.h"

/* virtual base class for data format decoders */
dlformat::dlformat()
{
    source = NULL;
    data = NULL;
    size = 0;
}

dlformat::~dlformat()
{
}

int dlformat::attach(dlsource* s)
{
    /* attach the input source */
    source = s;
    token = source->attach();
    return 0;
}

size_t dlformat::read(unsigned char *buf, size_t bytes)
{
    size_t size = source->read(buf, bytes, token);
    if (size!=bytes) {
        /* no timestamp so simply loop input */
        source->rewind(token);
        size = source->read(buf, bytes, token);
    }
    return size;
}

const unsigned char *dlformat::read(size_t *bytes)
{
    size_t size = *bytes;
    const unsigned char *data = source->read(bytes, token);
    if (data==NULL || size!=*bytes) {
        /* no timestamp so simply loop input */
        source->rewind(token);
        *bytes = size; /* discard previous read */
        data = source->read(bytes, token);
    }
    return data;
}

long long dlformat::get_timestamp()
{
    /* only implemented in sub-classes */
    return -1ll;
}

dlsource* dlformat::get_source()
{
    return source;
}

/* elementary stream format decoder class */

/* transport stream format decoder class */
dltstream::dltstream(int p)
{
    source = NULL;
    pid = p;
    pts = -1ll;
    data = NULL;
    size = 0;
}

dltstream::~dltstream()
{
    if (data)
        free(data);
}

int dltstream::attach(dlsource *s)
{
    /* attach the input source */
    source = s;
    token = source->attach();

    /* allocate the read buffer */
    size = 184;
    data = (unsigned char *) malloc(size);

    return 0;
}

size_t dltstream::read(unsigned char *buf, size_t bytes)
{
    size_t size = 0;

    /* on first read of the data stream skip until the pts is initialised */
    do {
        long long new_pts;
        size = next_pes_packet_data(buf, &new_pts, pid, 0, source, token);
        if (new_pts>=0)
            pts = new_pts;
    } while (pts<0);
    return size;
}

const unsigned char *dltstream::read(size_t *bytes)
{
    do {
        long long new_pts;
        *bytes = next_pes_packet_data(data, &new_pts, pid, 0, source, token);
        if (new_pts>=0)
            pts = new_pts;
    } while (pts<0);
    return data;
}

long long int dltstream::get_timestamp()
{
    return pts;
}

/* ffmpeg (libavformat) decoder class */
dlavformat::dlavformat()
{
    formatcontext = NULL;
    iocontext = NULL;
    /* register all the codecs and formats */
    av_register_all();
    errorstring = (char *) malloc(AV_ERROR_MAX_STRING_SIZE);
}

dlavformat::~dlavformat()
{
    // the following line triggers a double-free on my system,
    // but valgrind tells me it should be here
    //avformat_close_input(&formatcontext);
    if (iocontext) {
        av_freep(&iocontext->buffer);
        av_freep(&iocontext);
    }
    free(errorstring);
}

/* av io context callbacks */
int read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    class dlavformat *p = (class dlavformat *)opaque;
    return (int) p->read(buf, (size_t)buf_size);
}

int dlavformat::attach(dlsource *s)
{
    /* attach the input source */
    source = s;
    token = source->attach();

    /* allocate the read buffer */
    size = 4096; //188;
    data = (unsigned char *) av_malloc(size);

    /* allocate format context */
    formatcontext = avformat_alloc_context();
    if ( !formatcontext ) {
        dlmessage("failed to allocate format context");
        return -1;
    }

    /* allocate the io context */
    iocontext = avio_alloc_context(data, size, 0, this, &read_packet, NULL, NULL);
    if (!iocontext) {
        dlmessage("failed to allocate io context");
        return -1;
    }
    formatcontext->pb = iocontext;

    /* open the format context with custom io */
    int ret = avformat_open_input(&formatcontext, NULL, NULL, NULL);
    if (ret < 0) {
        av_strerror(ret, errorstring, AV_ERROR_MAX_STRING_SIZE);
        dlmessage("failed to open format context with custom io: %s", errorstring);
        return -1;
    }

    return 0;
}

size_t dlavformat::read(unsigned char *buf, size_t bytes)
{
    size_t size = source->read(buf, bytes, token);
    if (size==0) {
        /* no timestamp so simply loop input */
        source->rewind(token);
        size = source->read(buf, bytes, token);
    }
    return size;
}

