/*
 * Description: object interfaces to decoding libraries
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2011 4i2i Communications Ltd.
 */

#include <string.h>
#include <errno.h>
#include <math.h>

#include "dldecode.h"
#include "dlutil.h"
#include "dlconv.h"

dldecode::dldecode()
{
    /* source */
    format = NULL;
    verbose = 0;
    /* timestamp */
    last_sts = 0;
    timestamp = 0;
    frames_since_pts = 0;
    /* debug */
    top_field_first = 1;
    blank_field = 0;
}

dldecode::~dldecode()
{
}

int dldecode::attach(dlformat *f)
{
    /* attach the input format */
    format = f;
    return 0;
}

void dldecode::set_field_order(int tff)
{
    top_field_first = !!tff;
}

void dldecode::set_blank_field(int order)
{
    blank_field = order;
}

dlyuv::dlyuv()
{
    /* buffer */
    size = 0;
    data = NULL;
}

dlyuv::~dlyuv()
{
    if (data)
        free(data);
}

int dlyuv::attach(dlformat *f)
{
    /* attach the input source */
    format = f;

    /* determine the video format */
    if (divine_video_format(imagesize, &width, &height, &interlaced, &framerate)<0)
        if (divine_video_format(format->get_source()->name(), &width, &height, &interlaced, &framerate)<0)
            dlexit("failed to determine output video format: displayformat=%s filename=%s", size, format->get_source()->name());

    /* override the pixelformat if a fourcc is specified */
    pixelformat = I420;
    if (fourcc) {
        if (divine_pixel_format(fourcc, &pixelformat)<0)
            dlexit("failed to determine input pixel format from fourcc: %s", fourcc);
    } else {
        /* not an error if these don't find a match */
        if (divine_pixel_format(imagesize, &pixelformat)<0)
            divine_pixel_format(format->get_source()->name(), &pixelformat);
    }

    /* allocate the read buffer */
    size = pixelformat_get_size(pixelformat, width, height);
    data = (unsigned char *) malloc(size);

    /* calculate the number of frames in the input */
    maxframes = format->get_source()->size() / size;

    return 0;
}

bool dlyuv::atend()
{
    return format->get_source()->pos()/size==maxframes;
}

decode_t dlyuv::decode(unsigned char *uyvy, size_t uyvysize)
{
    decode_t results = {0, 0};

    if (pixelformat==UYVY) {
        /* read directly into frame */
        if (format->read(uyvy, width*height*2)!=(size_t)(width*height*2))
            dlerror("failed to read frame from input stream");
        results.size = width*height*2;
    } else {
        /* read frame from input */
        size_t bytes = size;
        const unsigned char *data = format->read(&bytes);
        if (data==NULL || bytes!=size)
            dlerror("failed to read frame from input stream");
        results.size = bytes;

        /* convert to uyvy */
        if (!lumaonly) {
            const unsigned char *yuv[3] = {data, data+width*height, (pixelformat==I444? data+2*width*height : pixelformat==I422? data+3*width*height/2 : data+5*width*height/4)};
            convert_yuv_uyvy(yuv, uyvy, width, height, pixelformat);
            //convert_i420_uyvy(data, uyvy, width, height, pixelformat);
        } else
            convert_i420_uyvy_lumaonly(data, uyvy, width, height);

    }

    results.timestamp = timestamp;
    timestamp += llround(180000.0/framerate);

    return results;
}

dlmpeg2::dlmpeg2()
{
    /* initialise the mpeg2 video decoder */
    mpeg2dec = mpeg2_init();
    if (mpeg2dec==NULL)
        dlexit("failed to initialise libmpeg2");
    info = mpeg2_info(mpeg2dec);
    mpeg2_accel(MPEG2_ACCEL_DETECT);
    /* don't assume timestamps start from zero */
    last_sts = -1;
}

dlmpeg2::~dlmpeg2()
{
    if (mpeg2dec) {
        mpeg2_close(mpeg2dec);
    }
}

int dlmpeg2::attach(dlformat *f)
{
    /* attach the input source */
    format = f;

    /* find the first sequence header */
    int seq_found = 0;
    const unsigned char *data;
    size_t read = 0;
    do {
        mpeg2_state_t state = mpeg2_parse(mpeg2dec);
        switch (state) {
            case STATE_BUFFER:
                /* read a chunk of data from input */
                data = format->read(&read);
                if (read>0) {
                    /* tag with most recent available timestamp */
                    sts_t sts = format->get_pts();
                    mpeg2_tag_picture(mpeg2dec, (uint32_t)sts, uint32_t(sts>>32));
                    mpeg2_buffer(mpeg2dec, (unsigned char *)data, (unsigned char *)data+read);
                }
                break;

            case STATE_SEQUENCE:
                width = info->sequence->width;
                height = info->sequence->height;
                interlaced = !(info->sequence->flags & SEQ_FLAG_PROGRESSIVE_SEQUENCE);
                framerate = 27000000.0/info->sequence->frame_period;
                pixelformat = info->sequence->height==info->sequence->chroma_height? I422 : I420;
                seq_found = 1;
                break;

            default:
                break;
        }
    } while(read && !seq_found);

    return 0;
}

decode_t dlmpeg2::decode(unsigned char *uyvy, size_t uyvysize)
{
    decode_t results = {0, -1ll, 0ll, 0ll};

    const unsigned char *data;
    size_t read;
    do {
        mpeg2_state_t state = mpeg2_parse(mpeg2dec);
        switch (state) {
            case STATE_BUFFER:
                /* read a chunk of data from input */
                data = format->read(&read);
                if (read==0 || format->get_source()->eof()) {
                    results.size = 0;
                    return results;
                }
                if (read>0) {
                    /* tag with most recent available timestamp */
                    sts_t sts = format->get_pts();
                    mpeg2_tag_picture(mpeg2dec, (uint32_t)sts, uint32_t(sts>>32));
                    mpeg2_buffer(mpeg2dec, (unsigned char *)data, (unsigned char *)data+read);
                } else
                    return results;
                break;

            case STATE_SLICE:
            case STATE_END:
                if (info->display_fbuf) {
                    const unsigned char *yuv[3] = {info->display_fbuf->buf[0], info->display_fbuf->buf[1], info->display_fbuf->buf[2]};
                    convert_yuv_uyvy(yuv, uyvy, width, height, pixelformat);
                    results.size = width*height*2;
                    sts_t sts = -1;
                    if (info->current_picture)
                        sts = ((sts_t)(info->current_picture->tag2)<<32) | (sts_t)info->current_picture->tag;
                    if (sts<0 && last_sts<0) {
                        /* first timestamp will be zero if not provided */
                        sts = 0;
                        //dlmessage("init video sts=%s", describe_sts(sts));
                    } else if (sts<0 || sts==last_sts) {
                        /* extrapolate a timestamp if a new one not provided */
                        sts = last_sts + llround(180000.0/framerate);
                        //dlmessage("calc video sts=%s delta=%lld", describe_sts(sts), llround(180000.0/framerate));
                    } else if (sts<last_sts) {
                        /* this should not happen, timestamps should monotonically increase,
                         * also this can be caused by a bug in libmpeg2 overwritting the tag */
                        sts = last_sts + llround(180000.0/framerate);
                        //dlmessage("calc video sts=%s delta=%lld", describe_sts(sts), llround(180000.0/framerate));
                    } else {
                        /* use provided timestamp */
                        ;
                        //dlmessage("new  video sts=%s delta=%lld", describe_sts(sts), sts-last_sts);
                    }
                    results.timestamp = last_sts = sts;

                    return results;
                }
                break;

            default:
                break;
        }
    } while(1);

    return results;
}

const uint8_t ff_reverse[256] = {
    0x00,0x80,0x40,0xC0,0x20,0xA0,0x60,0xE0,0x10,0x90,0x50,0xD0,0x30,0xB0,0x70,0xF0,
    0x08,0x88,0x48,0xC8,0x28,0xA8,0x68,0xE8,0x18,0x98,0x58,0xD8,0x38,0xB8,0x78,0xF8,
    0x04,0x84,0x44,0xC4,0x24,0xA4,0x64,0xE4,0x14,0x94,0x54,0xD4,0x34,0xB4,0x74,0xF4,
    0x0C,0x8C,0x4C,0xCC,0x2C,0xAC,0x6C,0xEC,0x1C,0x9C,0x5C,0xDC,0x3C,0xBC,0x7C,0xFC,
    0x02,0x82,0x42,0xC2,0x22,0xA2,0x62,0xE2,0x12,0x92,0x52,0xD2,0x32,0xB2,0x72,0xF2,
    0x0A,0x8A,0x4A,0xCA,0x2A,0xAA,0x6A,0xEA,0x1A,0x9A,0x5A,0xDA,0x3A,0xBA,0x7A,0xFA,
    0x06,0x86,0x46,0xC6,0x26,0xA6,0x66,0xE6,0x16,0x96,0x56,0xD6,0x36,0xB6,0x76,0xF6,
    0x0E,0x8E,0x4E,0xCE,0x2E,0xAE,0x6E,0xEE,0x1E,0x9E,0x5E,0xDE,0x3E,0xBE,0x7E,0xFE,
    0x01,0x81,0x41,0xC1,0x21,0xA1,0x61,0xE1,0x11,0x91,0x51,0xD1,0x31,0xB1,0x71,0xF1,
    0x09,0x89,0x49,0xC9,0x29,0xA9,0x69,0xE9,0x19,0x99,0x59,0xD9,0x39,0xB9,0x79,0xF9,
    0x05,0x85,0x45,0xC5,0x25,0xA5,0x65,0xE5,0x15,0x95,0x55,0xD5,0x35,0xB5,0x75,0xF5,
    0x0D,0x8D,0x4D,0xCD,0x2D,0xAD,0x6D,0xED,0x1D,0x9D,0x5D,0xDD,0x3D,0xBD,0x7D,0xFD,
    0x03,0x83,0x43,0xC3,0x23,0xA3,0x63,0xE3,0x13,0x93,0x53,0xD3,0x33,0xB3,0x73,0xF3,
    0x0B,0x8B,0x4B,0xCB,0x2B,0xAB,0x6B,0xEB,0x1B,0x9B,0x5B,0xDB,0x3B,0xBB,0x7B,0xFB,
    0x07,0x87,0x47,0xC7,0x27,0xA7,0x67,0xE7,0x17,0x97,0x57,0xD7,0x37,0xB7,0x77,0xF7,
    0x0F,0x8F,0x4F,0xCF,0x2F,0xAF,0x6F,0xEF,0x1F,0x9F,0x5F,0xDF,0x3F,0xBF,0x7F,0xFF,
    };

dlpcm::dlpcm()
{
    audio_packet_size = 0;
    number_channels = 0;
    bits_per_sample = 0;
    pkt = NULL;
    start = end = NULL;
}

dlpcm::~dlpcm()
{
    if (pkt)
        free(pkt);
}

int dlpcm::attach(dlformat *f)
{
    /* attach the input source */
    format = f;

    /* find the audio format */
    size_t read;
    const unsigned char *data = format->read(&read);    /* the first read will sync to the start of a PES packet */
    if (read==0) {
        if (format->get_source()->eof() || format->get_source()->error()) {
            dlmessage("failed to sync s302m audio");
            return -1;
        }
    }

    sts_t sts = format->get_pts();
    if (sts>=0) {
        last_sts = sts;
        frames_since_pts = 0;
    }

    /* decode aes3 header */
    audio_packet_size = data[0]<<8 | data[1];
    number_channels = (data[2]>>6) & 0x3;
    number_channels = 2+2*number_channels;
    //int channel_identification = (data[3] & 0x3)<<6 | data[2]>>2;
    bits_per_sample = (data[3] >> 4) & 0x3;
    bits_per_sample = 16+4*bits_per_sample;
    if (verbose>=1)
        dlmessage("audio format is 48kHz x%d channels of %d-bit (packet size %d)", number_channels, bits_per_sample, audio_packet_size);

    /* allocate the packet buffer */
    pkt = (unsigned char *) malloc(audio_packet_size);

    /* initialise the read buffer pointers */
    start = data;
    end = data + read;

    return 0;
}

// decode one aes3 data packet into the sample buffer
decode_t dlpcm::decode(unsigned char *samples, size_t sampsize) // sampsize is in bytes.
{
    decode_t results = {0, -1ll, 0ll, 0ll};

    size_t numsamps = 0;
    size_t read;
    do {

        if (start==end) {
            /* read from input and check for exceptions */
            const unsigned char *data = format->read(&read);
            if (read==0) {
                if (format->get_source()->error()) {
                    dlmessage("error reading input stream \"%s\": %s", format->get_source()->name(), strerror(errno));
                    results.size = 0;
                    return results;
                }
                if (format->get_source()->eof()) {
                    results.size = 0;
                    return results;
                }
            }

            /* look for a new timestamp, should be one every pes packet */
            sts_t sts = format->get_pts();
            if (sts>=0 && sts>last_sts) {
                results.timestamp = last_sts = sts;
                frames_since_pts = 0;
                //dlmessage("new audio sts=%s", describe_sts(sts));
            }

            /* finally update the read buffer pointers */
            start = data;
            end = data + read;
        } else {
            /* this actually only happens after the attach(),
             * so use the timestamp from there for the first packet */
            results.timestamp = last_sts;
        }

        /* we are pointing at the aes3 data header initially */
        size_t new_aps = start[0]<<8 | start[1];
        if (new_aps > audio_packet_size)
            pkt = (unsigned char *)realloc(pkt, new_aps);
        audio_packet_size = new_aps;
        if (((start[2]>>6) & 0x3) *2+2 != number_channels) {
            dlmessage("aes3 data header changed");
            break;
        }
        //int channel_identification = (start[3] & 0x3)<<6 | start[2]>>2;
        if (((start[3] >> 4) & 0x3) *4+16 != bits_per_sample) {
            dlmessage("aes3 data header changed");
            break;
        }

        start += 4;

        /* fill an aes3 data packet from input */
        //dlmessage("start=%d end=%d", start-data, end-data);
        for (size_t i=0; i<audio_packet_size; ) {
            if (start==end) {
                /* read from input and check for exceptions */
                const unsigned char *data = format->read(&read);
                if (read==0) {
                    if (format->get_source()->error()) {
                        dlmessage("error reading input stream \"%s\": %s", format->get_source()->name(), strerror(errno));
                        results.size = 0;
                        return results;
                    }
                    if (format->get_source()->eof()) {
                        results.size = 0;
                        return results;
                    }
                }

                /* finally update the read buffer pointers */
                start = data;
                end = data + read;
            }

            size_t chunksize = mmin(audio_packet_size-i, (size_t)(end-start));
            memcpy(pkt+i, start, chunksize);
            start += chunksize;
            i += chunksize;
        }

        /* sanity check that audio packet is all of pes packet */
        //if (start!=end)
        //    dlmessage("start=%d end=%d", start-data, end-data);

        /* copy data from aes3 buffer to audio buffer */
        for (unsigned char *ptr=pkt; ptr<pkt+audio_packet_size && numsamps<sampsize; numsamps+=4) {
            switch (bits_per_sample) {
                case 16:
                    samples[numsamps+0] = ff_reverse[ptr[0]];
                    samples[numsamps+1] = ff_reverse[ptr[1]];
                    samples[numsamps+2] = ff_reverse[((ptr[2]&0x0f)<<4) | (ptr[3]>>4)];
                    samples[numsamps+3] = ff_reverse[((ptr[3]&0x0f)<<4) | (ptr[4]>>4)];
                    ptr += 5;
                    break;
                case 24:
                    samples[numsamps+0] = ff_reverse[ptr[1]];
                    samples[numsamps+1] = ff_reverse[ptr[2]];
                    samples[numsamps+2] = ff_reverse[((ptr[4]&0xf)<<4) | (ptr[5]>>4)];
                    samples[numsamps+3] = ff_reverse[((ptr[5]&0xf)<<4) | (ptr[6]>>4)];
                    ptr += 7;
                    break;
                default:
                    dlmessage("unsupported 302m sample size: %d", bits_per_sample);
            }
        }

    } while (0); //(numsamps<sampsize);

    results.size += numsamps / 2; /* number of samples */
    frames_since_pts += numsamps /4; /* number of sample frames */

    /* extrapolate a timestamp if necessary,
     * should not be required according to the spec para 6.10 */
    if (results.timestamp<0) {
        results.timestamp = last_sts + (sts_t)(frames_since_pts*180000ll/48000ll);
        dlmessage("ext audio sts=%s", describe_sts(results.timestamp));
    }

    return results;
}

dlmpg123::dlmpg123()
{
    /* initialise the mpeg1 audio decoder */
    ret = mpg123_init();
    if (ret!=MPG123_OK)
        dlexit("failed to initialise mpg123");
    m = mpg123_new(NULL, &ret);
    if (m==NULL)
        dlexit("failed to create mpg123 handle");
    //mpg123_param(m, MPG123_VERBOSE, 2, 0);
    mpg123_open_feed(m);
}

dlmpg123::~dlmpg123()
{
    mpg123_delete(m);
    mpg123_exit();
}

int dlmpg123::attach(dlformat *f)
{
    /* attach the input source */
    format = f;

    /* find the audio format */
    do {
        size_t bytes, read;
        const unsigned char *data = format->read(&read);
        if (read==0) {
            if (format->get_source()->eof() || format->get_source()->error()) {
                dlmessage("failed to sync mpa audio");
                return -1;
            }
        }

        sts_t sts = format->get_pts();
        if (sts>=0) {
            last_sts = sts;
            frames_since_pts = 0;
        }

        // TODO try mpg123_feed
        ret = mpg123_decode(m, data, read, NULL, 0, &bytes);
        //ret = mpg123_feed(m, data, read);
        if (ret==MPG123_ERR || ret==MPG123_DONE) {
            dlmessage("failed to determine format of audio data: %s", mpg123_strerror(m));
            return -1;
        }
    } while (ret!=MPG123_NEW_FORMAT);

    long rate;
    int channels, enc;
    mpg123_getformat(m, &rate, &channels, &enc);
    dlmessage("audio format is  %ldHz x%d channels", rate, channels);

    return 0;
}

decode_t dlmpg123::decode(unsigned char *samples, size_t sampsize)
{
    decode_t results = {0, -1, 0ll, 0ll};

    /* feed the audio decoder */
    do {
        size_t read;
        const unsigned char *data = format->read(&read);
        if (read==0) {
            if (format->get_source()->error()) {
                dlmessage("error reading input stream \"%s\": %s", format->get_source()->name(), strerror(errno));
                results.size = 0;
                return results;
            }
            if (format->get_source()->eof()) {
                off_t offset = 0;
                ret = mpg123_feedseek(m, 0, SEEK_SET, &offset);
                if (ret != MPG123_OK)
                    dlerror("failed to seek in audio stream: %d", mpg123_strerror(m));
                format->get_source()->rewind();
                continue;
            }
        }

        sts_t sts = format->get_pts();
        if (sts>=0 && sts>last_sts) {
            results.timestamp = last_sts = sts;
            frames_since_pts = 0;
            //dlmessage("new audio pts=%s", describe_timestamp(pts));
        }

        ret = mpg123_decode(m, data, read, samples, sampsize, &results.size);
    } while (results.size==0 && ret!=MPG123_ERR);

    /* extrapolate a timestamp if necessary */
    if (results.timestamp<0) {
        frames_since_pts++;
        results.timestamp = last_sts + (sts_t)(frames_since_pts*180000ll*1152ll/48000ll);
    }

    return results;
}

dlliba52::dlliba52()
{
    /* initialise the ac3 audio decoder */
    //a52_state = a52_init(mm_accel());
    a52_state = a52_init(MM_ACCEL_X86_MMXEXT);
    sample = a52_samples(a52_state);
    if (a52_state==NULL || sample==NULL)
        dlexit("failed to initialise liba52");

    /* initialise the buffers */
    ac3_length = 0;
    ac3_size = 0;
    ac3_frame = NULL;

    /* initialise the transport stream parser */
    last_sts = -1;
}

dlliba52::~dlliba52()
{
    if (a52_state)
        a52_free(a52_state);
}

int dlliba52::attach(dlformat *f)
{
    /* attach the input source */
    format = f;

    /* find the ac3 audio format */
    int flags = 0, sample_rate = 0, bit_rate = 0;

    /* allocate the audio buffers */
    ac3_size = 6*256*5*sizeof(uint16_t); //3840+188;
    ac3_frame = (unsigned char *)malloc(ac3_size);

    unsigned int sync = 0;
    const unsigned char *buf;
    size_t read;
    int ret = 0;
    do {
        buf = format->read(&read);
        if (read==0) {
            if (format->get_source()->eof() || format->get_source()->error()) {
                dlmessage("failed to sync ac3 audio");
                break;
            }
        }

        /* look for first pts */
        sts_t sts = format->get_pts();
        if (sts>=0) {
            last_sts = sts;
            frames_since_pts = 0;
        }

        /* look for sync in ac3 stream FIXME this won't work if sync is in last 7 bytes of packet */
        for (sync=0; sync<read-7; sync++) {
            ret = a52_syncinfo((unsigned char *)buf+sync, &flags, &sample_rate, &bit_rate);
            if (ret)
                break;
        }
    } while (ret==0 || last_sts<0);

    /* queue the synchronised buf */
    memcpy(ac3_frame, buf+sync, read-sync);
    ac3_length = read-sync;
    dlmessage("found a52 frame of %d bytes, %d in buffer, sync=%d, initial pts=%s", ret, ac3_length, sync, describe_sts(last_sts));

    /* report the format parameters */
    int channels = 0;
    switch (flags & A52_CHANNEL_MASK) {
        case  0: channels = 2; break;
        case  1: channels = 1; break;
        case  2: channels = 2; break;
        case  3: channels = 3; break;
        case  4: channels = 3; break;
        case  5: channels = 4; break;
        case  6: channels = 4; break;
        case  7: channels = 5; break;
        case  8: channels = 1; break;
        case  9: channels = 1; break;
        case 10: channels = 2; break;
    }
    int lfe_channel = flags&A52_LFE? 1 : 0;
    dlmessage("audio format is %.1fkHz %d.%d channels @%dbps", sample_rate/1000.0, channels, lfe_channel, bit_rate);

    return 0;
}

/*
 * this conversion from 32-bit floating point (single precision)
 * to 32-bit int relies on some specific behaviour of the IEEE
 * floating point standard, the input needs to be denormalised
 * to 384+/-1 to this to work
 */
static inline int float32_to_int32_hack(int32_t i)
{
    if (i > 0x43c07fff)
        return 32767;
    else if (i < 0x43bf8000)
        return -32768;
    return i - 0x43c00000;
}

decode_t dlliba52::decode(unsigned char *frame, size_t framesize)
{
    decode_t results = {0, -1ll, 0ll, 0ll};
    size_t read;

    /* sync to next frame */
    int length = 0;
    int flags, sample_rate, bit_rate;
    do {
        if (ac3_length<7) {
            const unsigned char *buf = format->read(&read);
            if (read==0) {
                if (format->get_source()->error()) {
                    dlmessage("error reading input stream \"%s\": %s", format->get_source()->name(), strerror(errno));
                    results.size = 0;
                    return results;
                }
                if (format->get_source()->eof()) {
                    format->get_source()->rewind();
                    continue;
                }
            }

            sts_t sts = format->get_pts();
            if (sts>=0 && sts>last_sts) {
                last_sts = sts;
                frames_since_pts = 0;
                //dlmessage("new audio pts=%s", describe_sts(sts));
            }
            memcpy(ac3_frame+ac3_length, buf, read);
            ac3_length += read;
        }

        /* look for sync in ac3 stream */
        int sync;
        for (sync=0; sync<ac3_length-7; sync++) {
            length = a52_syncinfo(ac3_frame+sync, &flags, &sample_rate, &bit_rate);
            if (length)
                break;
            //else
            //    dlmessage("ac_length=%d ac3_frame=%02x %02x %02x %02x", ac3_length, *(ac3_frame+sync+0), *(ac3_frame+sync+1), *(ac3_frame+sync+2), *(ac3_frame+sync+3));
        }

        /* if no luck */
        if (length==0) {
            /* reset buffer for next loop */
            memmove(ac3_frame, ac3_frame+sync, ac3_length-sync);
            ac3_length = ac3_length-sync;
        }

    } while (length==0);

    /* prepare the next frame for decoding */
    do {
        /* read data from transport stream to complete frame */
        while (ac3_length < length) {
            const unsigned char *buf = format->read(&read);
            if (read<=0) {
                if (format->get_source()->error()) {
                    dlmessage("error reading input stream \"%s\": %s", format->get_source()->name(), strerror(errno));
                    break;
                }
                if (format->get_source()->eof()) {
                    format->get_source()->rewind();
                    continue;
                }
            }
            memcpy(ac3_frame+ac3_length, buf, read);
            ac3_length += read;
        }
    } while (0);

    /* feed the frame to the audio decoder */
    flags = A52_STEREO | A52_ADJUST_LEVEL;
    sample_t level = 1.0;
    sample_t bias = 384.0;
    if (a52_frame(a52_state, ac3_frame, &flags, &level, bias))
        dlmessage("failed: a52_frame");

    /* decode audio frame */
    int i, j;
    for (i=0; i<6; i++) {
        int32_t *f = (int32_t *)sample;
        int16_t *s = (int16_t *)frame;

        if (a52_block(a52_state))
            dlmessage("failed: a52_block");

        /* convert decoded samples to integer and interleave */
        for (j=0; j<256; j++) {
            s[i*512+j*2  ] = (int16_t) float32_to_int32_hack(f[j    ]);
            s[i*512+j*2+1] = (int16_t) float32_to_int32_hack(f[j+256]);
        }
    }
    results.size = 6*256*2; /* in samples */

    /* keep leftover data for next frame */
    if (ac3_length-length)
        memmove(ac3_frame, ac3_frame+length, ac3_length-length);
    ac3_length = ac3_length-length;

    /* extrapolate a timestamp if necessary */
    //static sts_t prev_sts;
    results.timestamp = last_sts + (sts_t)(frames_since_pts*180000ll*6ll*256ll/48000ll);
    frames_since_pts++;
    //dlmessage("    audio pts=%s diff=%d", describe_sts(results.timestamp), results.timestamp-prev_sts);
    //prev_sts = results.timestamp;

    return results;
}

#ifdef HAVE_LIBDE265
dlhevc::dlhevc()
{
    /* initialise the hevc video decoder */
    err = DE265_OK;
    ctx = de265_new_decoder();
    if (ctx==NULL)
        dlexit("failed to initialise libde265");

    /* configure hevc decoder */
    de265_set_parameter_bool(ctx, DE265_DECODER_PARAM_BOOL_SEI_CHECK_HASH, 1);
    de265_set_parameter_bool(ctx, DE265_DECODER_PARAM_SUPPRESS_FAULTY_PICTURES, false);
    if (0) {
        //de265_set_limit_TID(ctx, highestTID);
    }
    err = de265_start_worker_threads(ctx, 4);
    if (!de265_isOK(err))
        dlerror("failed to start decoder worker threads: %s", de265_get_error_text(err));
}

dlhevc::~dlhevc()
{
    if (ctx)
        de265_free_decoder(ctx);
}

int dlhevc::attach(dlformat *f)
{
    /* attach the input source */
    format = f;

    /* decode the first hevc frame */
    image = de265_peek_next_picture(ctx);
    while (!image) {

        /* decode some more */
        int more = 1;
        de265_error err = de265_decode(ctx, &more);
        if (more && de265_isOK(err))
            image = de265_peek_next_picture(ctx);
        else if (more && err==DE265_ERROR_IMAGE_BUFFER_FULL)
            image = de265_peek_next_picture(ctx);
        else if (more && err==DE265_ERROR_WAITING_FOR_INPUT_DATA) {
            /* read a chunk of input data */
            size_t read;
            const unsigned char *data = format->read(&read);
            if (n) {
                sts_t timestamp = format->get_pts();
                err = de265_push_data(ctx, data, read, timestamp, NULL);
                if (!de265_isOK(err)) {
                    dlerror("failed to push hevc data to decoder: %s", de265_get_error_text(err));
                }
            }

            if (format->get_source()->eof()) {
                err = de265_flush_data(ctx); // indicate end of stream
            }
        } else if (!more) {
            /* decoding finished */
            if (!de265_isOK(err))
                dlmessage("error decoding frame: %s", de265_get_error_text(err));
            break;
        }
    }

    /* retrieve parameters from first frame */
    if (image) {
        width  = de265_get_image_width(image,0);
        height = de265_get_image_height(image,0);
        interlaced = 0;
        framerate = 60000.0/1001.0; // FIXME not sure how to extract this.
        switch (de265_get_chroma_format(image)) {
          //case de265_chroma_444  : pixelformat = I444; break;
            case de265_chroma_422  : pixelformat = I422; break;
            case de265_chroma_420  : pixelformat = I420; break;
          //case de265_chroma_mono : pixelformat = Y800; break;
            default : dlexit("unknown chroma format");
        }

        /* heuristics for determining interlaced */
        if (width==1920 && (height==540 || height==558 || height==576)) {
            interlaced = 1;
            height *= 2;
            framerate /= 2.0;
            dlmessage("interlaced: height=%d", height);
        }
    }

    return 0;
}

decode_t dlhevc::decode(unsigned char *uyvy, size_t uyvysize)
{
    decode_t results = {0, 0, 0, 0};

    /* start timer */
    unsigned long long start = get_utime();

    if (!interlaced) {
        /* decode the next hevc frame */
        image = de265_get_next_picture(ctx);
        while (!image) {

            /* decode some more */
            int more = 1;
            de265_error err = de265_decode(ctx, &more);
            if (more && de265_isOK(err))
                image = de265_get_next_picture(ctx);
            else if (more && err==DE265_ERROR_WAITING_FOR_INPUT_DATA) {
                /* read a chunk of input data */
                size_t read;
                const unsigned char *data = format->read(&read);
                if (read==0 || format->get_source()->eof()) {
                    format->get_source()->rewind();
                    data = format->read(&read);
                }
                if (read>0) {
                    /* use most recent available timestamp */
                    sts_t timestamp = format->get_pts();
                    err = de265_push_data(ctx, data, read, timestamp, NULL);
                    if (!de265_isOK(err)) {
                        dlerror("failed to push hevc data to decoder: %s", de265_get_error_text(err));
                    }
                } else
                    return results;
            } else if (!more) {
                /* decoding finished */
                if (!de265_isOK(err))
                    dlmessage("error decoding frame: %s", de265_get_error_text(err));
                break;
            }
        }

        /* timestamp decode time */
        unsigned long long decode = get_utime();
        results.decode_time = decode - start;

        /* extract data from available frame */
        if (image) {
            int stride;

            /* copy frame to history buffer FIXME 4:2:0 only */
            const unsigned char *yuv[3] = {NULL};
            yuv[0] = de265_get_image_plane(image, 0, &stride);
            yuv[1] = de265_get_image_plane(image, 1, &stride);
            yuv[2] = de265_get_image_plane(image, 2, &stride);
            convert_yuv_uyvy(yuv, uyvy, width, height, pixelformat);
            results.size = width*height*2;
            sts_t sts = 2*de265_get_image_PTS(image);
            if (sts<0 || sts<=last_sts) {
                /* extrapolate a timestamp if necessary */
                sts = last_sts + llround(180000.0/framerate);
            }
            results.timestamp = last_sts = sts;
        }

        /* measure render time */
        results.render_time = get_utime() - decode;
    } else {
        /* decode until a top field and bottom field have been deinterlaced */
        for (int field=0; field<2; ) {
            /* (re-)start timer */
            start = get_utime();

            image = de265_get_next_picture(ctx);
            while (!image) {
                /* decode some more */
                int more = 1;
                de265_error err = de265_decode(ctx, &more);
                if (more && de265_isOK(err)) {
                    image = de265_get_next_picture(ctx);
                } else if (more && err==DE265_ERROR_WAITING_FOR_INPUT_DATA) {
                    /* read a chunk of input data */
                    size_t read;
                    const unsigned char *data = format->read(&read);
                    if (read==0 || format->get_source()->eof()) {
                        format->get_source()->rewind();
                        data = format->read(&read);
                    }
                    if (read>0) {
                        /* use most recent available timestamp */
                        sts_t timestamp = format->get_pts();
                        err = de265_push_data(ctx, data, read, timestamp, NULL);
                        if (!de265_isOK(err)) {
                            dlerror("failed to push hevc data to decoder: %s", de265_get_error_text(err));
                        }
                    } else
                        return results;
                } else if (!more) {
                    /* decoding finished */
                    if (!de265_isOK(err))
                        dlmessage("error decoding frame: %s", de265_get_error_text(err));
                    break;
                }
            }

            /* sum decode time */
            unsigned long long decode = get_utime();
            results.decode_time += decode - start;

            /* extract data from available frame */
            if (image) {
                int stride;

                /* copy frame to history buffer FIXME 4:2:0 only */
                const unsigned char *yuv[3] = {NULL};
                yuv[0] = de265_get_image_plane(image, 0, &stride);
                yuv[1] = de265_get_image_plane(image, 1, &stride);
                yuv[2] = de265_get_image_plane(image, 2, &stride);

#ifdef HAVE_LIBDE265_CUSTOM
                /* get poc and field order info from decoder */
                int poc = de265_get_image_picture_order_count(image);
                enum de265_field_order field_order = de265_get_image_field_order(image);

                /* determine field order of current picture */
                int top_field;
                switch (field_order) {
                    case de265_top_field:
                    case de265_bottom_field:
                        top_field = field_order==de265_top_field;
                        break;

                    case de265_top_field_prev_bottom_field:
                    case de265_bottom_field_prev_top_field:
                        top_field = field_order==de265_top_field_prev_bottom_field;
                        break;

                    case de265_top_field_next_bottom_field:
                    case de265_bottom_field_next_top_field:
                        top_field = field_order==de265_top_field_next_bottom_field;
                        break;

                    default:
                        /* guess using poc */
                        top_field = (poc&1)==0;
                        break;
                }
#else
                int top_field = 1;
#endif

                /* deinterlace */
                if (top_field==top_field_first) {
                    convert_top_field_yuv_uyvy(yuv, uyvy, width, height, pixelformat);
                    /* move field on if it is the correct order */
                    field += (top_field_first ^ field);
                } else if (top_field==!top_field_first) {
                    convert_bot_field_yuv_uyvy(yuv, uyvy, width, height, pixelformat);
                    field += !(top_field_first ^ field);
                }

                /* use timestamp from first field for display of deinterlaced frame */
                if (top_field==top_field_first) {
                    results.size = width*height*2;
                    sts_t sts = 2*de265_get_image_PTS(image);
                    if (sts<0 || sts<=last_sts) {
                        /* extrapolate a timestamp if necessary */
                        sts = last_sts + llround(180000.0/framerate);
                    }
                    results.timestamp = last_sts = sts;
                }

                /* sum render time */
                results.render_time += get_utime() - decode;
            }
        }
    }

    /* show warnings in decode */
    while (1) {
        de265_error warning = de265_get_warning(ctx);
        if (de265_isOK(warning))
            break;

        dlmessage("warning after decoding: %s", de265_get_error_text(warning));
    }

    return results;
}
#endif

#ifdef HAVE_FFMPEG
dlffvideo::dlffvideo()
{
    init();
}

dlffvideo::dlffvideo(enum AVCodecID id)
{
    init();
    codecid = id;
}

dlffvideo::~dlffvideo()
{
    av_frame_free(&frame);
    avcodec_free_context(&codeccontext);
    free(errorstring);
}

void dlffvideo::init()
{
    codeccontext = NULL;
    frame = NULL;
    size = 0;
    ptr = NULL;
    got_frame = 0;
    errorstring = (char *) malloc(AV_ERROR_MAX_STRING_SIZE);
    codecid = AV_CODEC_ID_H264; /* default codec is h.264 */
}

int dlffvideo::attach(dlformat* f)
{
    int ret;

    /* attach the input source */
    format = f;

    /* find required decoder */
    const AVCodec *codec = avcodec_find_decoder(codecid);
    if (!codec)
        dlexit("failed to find %s video decoder", avcodec_get_name(codecid));

    /* initialise the parser */
    parser = av_parser_init(codec->id);
    if (!parser)
        dlexit("failed to initialise codec parser");

    /* initialise the codec context */
    codeccontext = avcodec_alloc_context3(codec);
    if (!codeccontext)
        dlexit("failed to initialise codec context");

    /* initialise the frame */
    frame = av_frame_alloc();
    if (!frame) {
        dlmessage("failed to allocate video frame");
        return -1;
    }

    /* initialise the packet */
    packet = av_packet_alloc();
    if (!packet) {
        dlmessage("failed to allocate packet");
        return -1;
    }

    /* initialise a data buffer */
    //buf = (unsigned char *) malloc(bufsize + AV_INPUT_BUFFER_PADDING_SIZE);
    //memset(buf + bufsize, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    /* init the decoder, with or without reference counting */
    AVDictionary *opts = NULL;
    //if (api_mode == API_MODE_NEW_API_REF_COUNT)
    //    av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(codeccontext, codec, &opts)) < 0) {
        dlmessage("failed to open video codec");
        return ret;
    }

    /* decode the first frame to get the image parameters */
    got_frame = 0;
    while (!got_frame) {
        if (size==0) {
            const unsigned char *buf = format->read(&size);
            if (size==0)
                break;
            ptr = buf;
        }

        /* use the parser to split the data into frames */
        ret = av_parser_parse2(parser, codeccontext, &packet->data, &packet->size, ptr, size, format->get_pts(), format->get_dts(), 0);
        if (ret < 0)
            dlexit("failed to parse %s data", avcodec_get_name(codecid));
        ptr += ret;
        size -= ret;

        if (packet->size) {
            packet->pts = parser->pts;
            packet->dts = parser->dts;
            packet->pos = parser->pos;
            ret = avcodec_send_packet(codeccontext, packet);
            /* errors here are not critical until the first frame is decoded
            if (ret < 0)
                dlmessage("failed to send a packet for decoding: %s", av_make_error_string(errorstring, AV_ERROR_MAX_STRING_SIZE, ret)); */

            while (ret >= 0) {
                ret = avcodec_receive_frame(codeccontext, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                else if (ret < 0)
                    dlexit("error during decoding frame");

                got_frame = 1;
                break;
            }
        }
    }

    /* read the image parameters from the codeccontext */
    width = codeccontext->width;
    height = codeccontext->height;
    interlaced = codeccontext->field_order!=AV_FIELD_PROGRESSIVE && codeccontext->field_order!=AV_FIELD_UNKNOWN;
    switch (codeccontext->pix_fmt) {
        //case AV_PIX_FMT_YUV444P  : pixelformat = I444; break;
        case AV_PIX_FMT_YUV422P  :
        case AV_PIX_FMT_YUVJ422P : pixelformat = I422; break;
        case AV_PIX_FMT_YUV420P  :
        case AV_PIX_FMT_YUVJ420P : pixelformat = I420; break;
        //case AV_PIX_FMT_GRAY8    : pixelformat = Y800; break;
        default : dlexit("unknown chroma format: %s", av_get_pix_fmt_name(codeccontext->pix_fmt));
    }
    framerate = av_q2d(codeccontext->framerate);
    /* h.264 doesn't require timing info in elementary stream */
    if (framerate<0.1) {
        framerate = 30000.0/1001.0;
        dlmessage("framerate is zero, using a default framerate of %.2f", framerate);
    }

    /* dump input information to stderr */
    if (verbose>=1)
        dlmessage("video format is %dx%d%c%.2f", width, height, interlaced? 'i' : 'p', framerate);

    return 0;
}

decode_t dlffvideo::decode(unsigned char *uyvy, size_t uyvysize)
{
    decode_t results = {0, -1ll, 0ll, 0ll};
    int ret;

    /* start timer */
    unsigned long long start = get_utime();

    /* decode the next avc frame */
    do {
        if (size==0) {
            const unsigned char *buf = format->read(&size);
            if (size==0)
                break;
            ptr = buf;
        }

        /* use the parser to split the data into frames */
        if (!got_frame) {
            ret = av_parser_parse2(parser, codeccontext, &packet->data, &packet->size, ptr, size, format->get_pts(), format->get_dts(), 0);
            if (ret < 0)
                dlexit("failed to parse %s data", avcodec_get_name(codecid));
            ptr += ret;
            size -= ret;

            if (packet->size) {
                packet->pts = parser->pts;
                packet->dts = parser->dts;
                packet->pos = parser->pos;
                ret = avcodec_send_packet(codeccontext, packet);
                if (ret < 0)
                    dlexit("failed to send a packet for decoding");

                while (ret >= 0) {
                    ret = avcodec_receive_frame(codeccontext, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    else if (ret < 0)
                        dlexit("error during decoding frame");

                    got_frame = 1;
                    break;
                }
            }
        }

        if (got_frame) {
            /* timestamp decode time */
            unsigned long long decode = get_utime();
            results.decode_time = decode - start;

            /* copy frame to uyvy buffer */
            convert_yuv_uyvy((const unsigned char **)frame->data, uyvy, width, height, pixelformat);
            results.size = width*height*2;

            /* get timestamp from decoder */
            sts_t sts = frame->pts;
            if (sts<0 || sts==last_sts) {
                /* extrapolate a timestamp if necessary */
                frames_since_pts++;
                sts = last_sts + frames_since_pts * llround(180000.0/framerate); /* this is the more correct version */
                //sts = last_sts = last_sts + llround(180000.0/framerate); /* this one covers up a bug in the passing in of timestamps */
                //dlmessage("ext video sts=%s", describe_sts(sts));
            } else {
                last_sts = sts;
                frames_since_pts = 0;
                //dlmessage("new video sts=%s", describe_sts(sts));
            }

            results.timestamp = sts;

            /* measure render time */
            results.render_time = get_utime() - decode;
        }

        //av_packet_unref(packet);
    } while (!got_frame);

    got_frame = 0;

    return results;
}

dlffmpeg::dlffmpeg()
{
    formatcontext = NULL;
    codeccontext = NULL;
    frame = NULL;
    image[0] = NULL;
    errorstring = (char *) malloc(AV_ERROR_MAX_STRING_SIZE);
}

dlffmpeg::~dlffmpeg()
{
    if (image[0])
        av_freep(&image[0]);
    av_frame_free(&frame);
    avcodec_free_context(&codeccontext);
    free(errorstring);
}

int dlffmpeg::attach(dlformat* f)
{
    /* attach the input source */
    format = f;

    /* extract the pointer to the format context */
    formatcontext = ((dlavformat *)f)->formatcontext;

    /* get stream information */
    if (avformat_find_stream_info(formatcontext, NULL) < 0) {
        dlmessage("failed to find stream information");
        return -1;
    }

    AVCodec *codec;
    int ret = av_find_best_stream(formatcontext, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (ret < 0) {
        dlmessage("failed to find video stream in input file");
        return ret;
    }
    stream_index = ret;

    /* find decoder for the stream */
    AVStream *stream = formatcontext->streams[stream_index];
    codeccontext = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codeccontext, stream->codecpar);
    //av_codec_set_packet_timebase(codeccontext, stream->time_base);

    /* init the decoders, with or without reference counting */
    AVDictionary *opts = NULL;
    //if (api_mode == API_MODE_NEW_API_REF_COUNT)
    //    av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(codeccontext, codec, &opts)) < 0) {
        dlmessage("failed to open video codec");
        return ret;
    }

    /* read the image parameters from the codeccontext */
    width = codeccontext->width;
    height = codeccontext->height;
    interlaced = codeccontext->field_order!=AV_FIELD_PROGRESSIVE && codeccontext->field_order!=AV_FIELD_UNKNOWN;
    switch (codeccontext->pix_fmt) {
        //case AV_PIX_FMT_YUV444P  : pixelformat = I444; break;
        case AV_PIX_FMT_YUV422P  :
        case AV_PIX_FMT_YUVJ422P : pixelformat = I422; break;
        case AV_PIX_FMT_YUV420P  :
        case AV_PIX_FMT_YUVJ420P : pixelformat = I420; break;
        //case AV_PIX_FMT_GRAY8    : pixelformat = Y800; break;
        default : dlexit("unknown chroma format: %s", av_get_pix_fmt_name(codeccontext->pix_fmt));
    }
    /* need to hunt a bit for the framerate */
    if (codeccontext->framerate.num!=0) {
        //dlmessage("framerate: %d/%d", codeccontext->framerate.num, codeccontext->framerate.den);
        framerate = av_q2d(codeccontext->framerate);
    } else {
        //dlmessage("framerate: %d/%d", stream->avg_frame_rate.num, stream->avg_frame_rate.den);
        framerate = av_q2d(stream->avg_frame_rate);
    }

    /* allocate the decoded image */
    int size = av_image_alloc(image, linesizes, width, height, codeccontext->pix_fmt, 1);
    if (size < 0) {
        dlmessage("failed to allocate image");
        return -1;
    }

    /* initialise the frame */
    frame = av_frame_alloc();
    if (!frame) {
        dlmessage("failed to allocate video frame");
        return -1;
    }

    /* initialise the packet */
    packet = av_packet_alloc();
    if (!packet) {
        dlmessage("failed to allocate packet");
        return -1;
    }

    /* dump input information to stderr */
    if (verbose>=1)
        av_dump_format(formatcontext, stream_index, format->get_source()->name(), 0);

    return 0;
}

decode_t dlffmpeg::decode(unsigned char *uyvy, size_t uyvysize)
{
    decode_t results = {0, 0, 0, 0};

    /* start timer */
    unsigned long long start = get_utime();

    /* decode the next avc frame */
    int got_frame = 0;
    while (!got_frame) {
        if (av_read_frame(formatcontext, packet) < 0)
            /* end of file */
            break;

        // check stream index for frames we are interested in.
        if (packet->stream_index != stream_index)
            continue;

        //if (codeccontext->codec_type == AVMEDIA_TYPE_VIDEO || codeccontext->codec_type == AVMEDIA_TYPE_AUDIO) {
        int ret = avcodec_send_packet(codeccontext, packet);
        if (ret < 0) {
            dlmessage("error while sending pcket: %s", av_make_error_string(errorstring, AV_ERROR_MAX_STRING_SIZE, ret));
            break;
        } else {

            // FIXME sample code loops until no more frames.
            ret = avcodec_receive_frame(codeccontext, frame);
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
                /* there is no output frame available, but there were no errors during decoding */
                continue;
            } else if (ret < 0) {
                dlmessage("error decoding frame: %s", av_make_error_string(errorstring, AV_ERROR_MAX_STRING_SIZE, ret));
                return results;
            } else
                got_frame = 1;
        }

        if (got_frame) {
            /* timestamp decode time */
            unsigned long long decode = get_utime();
            results.decode_time = decode - start;

            /* copy frame to uyvy buffer */
            convert_yuv_uyvy((const unsigned char **)frame->data, uyvy, width, height, pixelformat);
            results.size = width*height*2;
            /* get pts from decoder */
            sts_t sts = 2*frame->pts;
            if (sts<0 || sts<=last_sts) {
                /* extrapolate a timestamp if necessary */
                sts = last_sts + llround(180000.0/framerate);
            }
            results.timestamp = last_sts = sts;

            /* measure render time */
            results.render_time = get_utime() - decode;

            av_frame_unref(frame);
        }

        av_packet_unref(packet);
    }

    return results;
}
#endif // HAVE_FFMPEG
