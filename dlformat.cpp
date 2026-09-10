/*
 * Description: container format decoder.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2015 4i2i Communications Ltd.
 */

#include <stdlib.h>
#include <assert.h>
#include <string.h>

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
    if (data)
        free(data);
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
    /* a request of zero bytes means read whatever is available, so only a read
       returning nothing is the end of the input, not one of a different size */
    if (data==NULL || (size? *bytes!=size : *bytes==0)) {
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

/* yuv4mpeg2 format decoder class */
dly4m::dly4m()
{
    width = height = 0;
    interlaced = false;
    framerate = 0.0;
    pixelformat = UNKNOWN;
    framesize = 0;
    streamhdrsize = framehdrsize = 0;
    numframes = frames_read = 0;
}

int dly4m::attach(dlsource *s)
{
    /* attach the input source */
    source = s;
    token = source->attach();

    /* allocate the buffer */
    size = 64*1024;     /* arbitrary size to start with */
    data = (unsigned char *) malloc(size);

    /* parse the stream header */
    if (read_stream_header()<0)
        return -1;

    /* the frame headers are assumed to be the same length throughout the
       stream, so the first one gives the number of frames in the input */
    if (read_frame_header()<0)
        return -1;
    numframes = (source->size() - streamhdrsize) / (framesize + framehdrsize);

    /* rewind to the first frame header */
    return rewind();
}

int dly4m::rewind(dltoken_t t)
{
    /* the frame headers are parsed on our own token, so rewind that one */
    if (source->rewind(token)<0)
        return -1;
    frames_read = 0;

    /* the stream header is only present at the start of the input */
    return read_stream_header();
}

/* read a header, which is a line of text terminated by a newline, from the
   input, and return its length in bytes, including the newline */
size_t dly4m::read_header(char *header, size_t maxlen)
{
    size_t len = 0;

    while (len<maxlen-1) {
        unsigned char c;
        if (source->read(&c, 1, token)!=1)
            return 0;
        if (c=='\n') {
            header[len] = '\0';
            return len+1;
        }
        header[len++] = c;
    }

    /* header is too long to be valid */
    return 0;
}

int dly4m::read_stream_header()
{
    char header[1024];

    streamhdrsize = read_header(header, sizeof(header));
    if (streamhdrsize==0)
        dlexit("failed to read yuv4mpeg2 stream header in \"%s\"", name());
    if (strncmp(header, "YUV4MPEG2", 9)!=0)
        dlexit("not a yuv4mpeg2 stream: \"%s\"", name());

    /* the colourspace parameter is optional, the spec default is 4:2:0 */
    pixelformat = I420;
    interlaced = false;
    framerate = 0.0;
    width = height = 0;

    /* the parameters are single letter tags, separated by whitespace */
    for (char *tag = strtok(header+9, " \t"); tag; tag = strtok(NULL, " \t")) {
        switch (tag[0]) {
            case 'W':
                width = atoi(tag+1);
                break;

            case 'H':
                height = atoi(tag+1);
                break;

            case 'F':
            {
                /* frame rate is a rational number, numerator:denominator */
                int num = 0, den = 0;
                if (sscanf(tag+1, "%d:%d", &num, &den)!=2 || den==0)
                    dlexit("invalid frame rate in yuv4mpeg2 stream header: %s", tag);
                framerate = (float)num / (float)den;
                break;
            }

            case 'I':
                /* p is progressive, t and b are interlaced, m is mixed */
                interlaced = tag[1]=='t' || tag[1]=='b';
                if (tag[1]=='m')
                    dlmessage("warning: mixed interlace mode in yuv4mpeg2 stream, assuming progressive");
                break;

            case 'C':
            {
                static const struct {
                    const char *name;
                    pixelformat_t pixelformat;
                } colourspaces[] = {
                    { "420",      I420 },
                    { "420jpeg",  I420 },
                    { "420paldv", I420 },
                    { "420mpeg2", I420 },
                    { "422",      I422 },
                    { "444",      I444 },
                    { "420p10",   YU15 },
                    { "422p10",   YU20 },
                };

                unsigned i;
                for (i=0; i<sizeof(colourspaces)/sizeof(colourspaces[0]); i++)
                    if (strcmp(tag+1, colourspaces[i].name)==0) {
                        pixelformat = colourspaces[i].pixelformat;
                        break;
                    }
                if (i==sizeof(colourspaces)/sizeof(colourspaces[0]))
                    dlexit("unsupported colourspace in yuv4mpeg2 stream: %s", tag+1);
                break;
            }

            /* aspect ratio and comments are not used */
            case 'A':
            case 'X':
                break;

            default:
                dlmessage("warning: unknown parameter in yuv4mpeg2 stream header: %s", tag);
                break;
        }
    }

    if (width<=0 || height<=0)
        dlexit("invalid image size in yuv4mpeg2 stream header: %dx%d", width, height);
    if (framerate<=0.0)
        dlexit("missing frame rate in yuv4mpeg2 stream header");

    framesize = pixelformat_get_size(pixelformat, width, height);

    return 0;
}

int dly4m::read_frame_header()
{
    char header[1024];

    framehdrsize = read_header(header, sizeof(header));
    if (framehdrsize==0)
        /* end of input */
        return -1;
    if (strncmp(header, "FRAME", 5)!=0)
        dlexit("lost synchronisation with yuv4mpeg2 stream in \"%s\"", name());

    return 0;
}

size_t dly4m::read(unsigned char *buf, size_t bytes)
{
    /* the frame data is preceded by a frame header */
    if (read_frame_header()<0) {
        /* loop the input */
        if (rewind()<0 || read_frame_header()<0)
            return 0;
    }

    size_t read = source->read(buf, bytes, token);
    if (read!=bytes) {
        /* a truncated frame at the end of the input, so loop */
        if (rewind()<0 || read_frame_header()<0)
            return 0;
        read = source->read(buf, bytes, token);
    }
    frames_read++;

    return read;
}

const unsigned char *dly4m::read(size_t *bytes)
{
    /* a zero copy read of a whole frame, the frame header is discarded */
    size_t size = *bytes? *bytes : framesize;

    if (read_frame_header()<0) {
        /* loop the input */
        if (rewind()<0 || read_frame_header()<0) {
            *bytes = 0;
            return NULL;
        }
    }

    *bytes = size;
    const unsigned char *data = source->read(bytes, token);
    if (data==NULL || *bytes!=size) {
        /* a truncated frame at the end of the input, so loop */
        if (rewind()<0 || read_frame_header()<0) {
            *bytes = 0;
            return NULL;
        }
        *bytes = size;
        data = source->read(bytes, token);
    }
    frames_read++;

    return data;
}

int dly4m::get_video_format(int *w, int *h, bool *i, float *f, pixelformat_t *p)
{
    if (w) *w = width;
    if (h) *h = height;
    if (i) *i = interlaced;
    if (f) *f = framerate;
    if (p) *p = pixelformat;

    return 0;
}

/* transport stream format decoder class */
dltstream::dltstream(int p)
{
    pid = p;
    pts = dts = -1ll;
    packet = NULL;
    packet_valid = false;
}

dltstream::~dltstream()
{
    if (packet)
        free(packet);
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
