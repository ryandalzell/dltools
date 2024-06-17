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

    /* allocate the buffer */
    size = 64*1024;     /* arbitrary size to start with */
    data = (unsigned char *) malloc(size);

    return 0;
}

size_t dlformat::read(unsigned char *buf, size_t bytes)
{
    size_t size = source->read(buf, bytes, token);
    if (size==0) {
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
    if (data==NULL || *bytes==0) {
        /* no timestamp so simply loop input */
        source->rewind(token);
        *bytes = size; /* discard previous read */
        data = source->read(bytes, token);
    }
    return data;
}

long long dlformat::get_pts()
{
    /* only implemented in sub-classes */
    return -1ll;
}

long long dlformat::get_dts()
{
    /* only implemented in sub-classes */
    return -1ll;
}

/* elementary stream format decoder class */

/* transport stream format decoder class */
dltstream::dltstream(int p)
{
    source = NULL;
    pid = p;
    pts = dts = -1ll;
    packet = NULL;
    packet_valid = false;
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

    /* allocate the packet buffer */
    packet = (unsigned char *) malloc(188);
    packet_valid = false;

    /* allocate the pes buffer */
    size = 64*1024;     /* arbitrary size to start with */
    data = (unsigned char *) malloc(size);

    return 0;
}

size_t dltstream::read(unsigned char *buf, size_t bytes)
{
    dlexit("dltstream::read() into external buffer is not supported");
    return 0;
}

const unsigned char *dltstream::read(size_t *bytes)
{
    size_t packet_size = 0;

    /* default no pts */
    pts = dts = -1ll;

    /* read next whole pes packet with correct pid */
    int start = 1;
    while (1) {
        /* read next packet */
        if (!packet_valid)
            if (next_packet(packet, source, token)<0)
                return 0;

        /* check pid is correct */
        int packet_pid = ((packet[1]<<8) | packet[2]) & 0x1fff;
        if (packet_pid!=pid)
            continue;

        /* check start indicator */
        int payload_unit_start_indicator = packet[1] & 0x40;
        if (start && !payload_unit_start_indicator)
            continue;   /* looking for start of next pes packet */
        else if (start && payload_unit_start_indicator)
            start = 0;  /* found start of next pes packet */
        else if (!start && payload_unit_start_indicator) {
            packet_valid = true;
            break;      /* start of next pes packet i.e. end of this one */
        }

        /* skip transport packet header */
        int ptr = 4;

        /* skip adaption field */
        int adaptation_field_control = (packet[3] >> 4) & 0x3;
        if (adaptation_field_control==2 || adaptation_field_control==3)
            ptr += 1 + packet[4];

        /* skip pes header */
        if (payload_unit_start_indicator) {
            int packet_start_code_prefix = (packet[ptr]<<16) | (packet[ptr+1]<<8) | packet[ptr+2];
            int stream_id = packet[ptr+3];
            if (packet_start_code_prefix!=0x1)
                dlexit("error parsing pes header, start_code=0x%06x stream_id=0x%02x", packet_start_code_prefix, stream_id);

            /* look for pts and dts */
            int pts_dts_flags = packet[ptr+7] >> 6;
            if (pts_dts_flags==2 || pts_dts_flags==3) {
                long long pts3 = (packet[ptr+9] >> 1) & 0x7;
                long long pts2 = (packet[ptr+10] << 7 | (packet[ptr+11] >> 1));
                long long pts1 = (packet[ptr+12] << 7 | (packet[ptr+13] >> 1));
                    pts = (pts3<<30) | (pts2<<15) | pts1;
            }
            if (pts_dts_flags==3) {
                long long dts3 = (packet[ptr+14] >> 1) & 0x7;
                long long dts2 = (packet[ptr+15] << 7 | (packet[ptr+16] >> 1));
                long long dts1 = (packet[ptr+17] << 7 | (packet[ptr+18] >> 1));
                    dts = (dts3<<30) | (dts2<<15) | dts1;
            }

            int pes_header_data_length = packet[ptr+8];

            ptr += 9 + pes_header_data_length;
        }

        /* resize data buffer if necessary */
        if (packet_size+188-ptr>size) {
            size += size;
            data = (unsigned char *) realloc(data, size);
            //dlmessage("reallocing data buffer to %zd bytes", size);
        }

        /* copy data */
        memcpy(data+packet_size, packet+ptr, 188-ptr);
        packet_size += 188-ptr;
        packet_valid = false;
    }

    //dlmessage("return pes packet of %zd bytes", packet_size);
    *bytes = packet_size;
    return data;
}

long long int dltstream::get_pts()
{
    /* return in system time */
    return 2*pts;
}

long long int dltstream::get_dts()
{
    /* return in system time */
    return 2*dts;
}

#ifdef HAVE_FFMPEG

/* ffmpeg (libavformat) decoder class */
dlavformat::dlavformat()
{
    formatcontext = NULL;
    iocontext = NULL;
    errorstring = (char *) malloc(AV_ERROR_MAX_STRING_SIZE);
}

dlavformat::~dlavformat()
{
    avformat_close_input(&formatcontext);
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
#endif // HAVE_FFMPEG
