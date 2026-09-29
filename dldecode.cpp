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

bool dldecode::atend()
{
    /* default don't know if at end of input */
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
    /* display parameters */
    lumaonly = 0;
    imagesize = NULL;
    fourcc = NULL;
    /* frame range */
    firstframe = 0;
    loopframes = 0;
    loopframe = 0;
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

    /* the container may carry the video format, otherwise divine it */
    if (format->get_video_format(&width, &height, &interlaced, &framerate, &pixelformat)==0) {
        /* the container is authoritative, so any command line format is ignored */
        if ((imagesize || fourcc) && verbose>=0)
            dlmessage("warning: using video format from %s container, not the command line", format->description());
    } else {
        /* determine the video format */
        if (divine_video_format(imagesize, &width, &height, &interlaced, &framerate)<0)
            if (divine_video_format(format->name(), &width, &height, &interlaced, &framerate)<0)
                dlexit("failed to determine output video format: displayformat=%s filename=%s", imagesize, format->name());

        /* override the pixelformat if a fourcc is specified */
        pixelformat = I420;
        if (fourcc) {
            if (divine_pixel_format(fourcc, &pixelformat)<0)
                dlexit("failed to determine input pixel format from fourcc: %s", fourcc);
        } else {
            /* not an error if these don't find a match */
            if (divine_pixel_format(imagesize, &pixelformat)<0)
                divine_pixel_format(format->name(), &pixelformat);
        }
    }

    /* allocate the read buffer */
    size = pixelformat_get_size(pixelformat, width, height);
    data = (unsigned char *) malloc(size);

    /* calculate the number of frames in the input */
    maxframes = format->filesize() / size;

    /* play a range of frames, looping from the last back to the first */
    if (firstframe || loopframes) {
        if (firstframe>=maxframes)
            dlexit("first frame %u is past the end of \"%s\", which has %u frames", firstframe, format->name(), maxframes);
        if (loopframes==0 || loopframes>maxframes-firstframe)
            loopframes = maxframes-firstframe;
        if (format->seek((off_t)firstframe*size)<0)
            dlexit("failed to seek to frame %u in \"%s\"", firstframe, format->name());
    }

    return 0;
}

bool dlyuv::atend()
{
    /* this only works for yuv data */
    if (loopframes)
        return loopframe==loopframes;
    return format->pos()/size==maxframes;
}

decode_t dlyuv::decode(unsigned char *uyvy, size_t uyvysize)
{
    decode_t results = {0, 0};

    /* go back to the first frame at the end of the loop */
    if (loopframes) {
        if (loopframe==loopframes) {
            if (format->seek((off_t)firstframe*size)<0)
                dlerror("failed to seek to frame %u in \"%s\"", firstframe, format->name());
            loopframe = 0;
        }
        loopframe++;
    }

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
    size_t read = 0;
    do {
        mpeg2_state_t state = mpeg2_parse(mpeg2dec);
        switch (state) {
            case STATE_BUFFER:
                /* read a chunk of data from input */
                data = format->read(&read);
                if (read==0 || format->eof()) {
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
        if (format->eof() || format->error()) {
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

    /* the fourth sample size is reserved in smpte 302m, and a stream which is
       not 302m at all is quite likely to land on it */
    if (bits_per_sample>24) {
        dlmessage("unsupported 302m sample size: %d", bits_per_sample);
        return -1;
    }

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
    bool discontinuity;
    do {
        discontinuity = false;

        if (start==end) {
            /* read from input and check for exceptions */
            const unsigned char *data = format->read(&read);
            if (read==0) {
                if (format->error()) {
                    dlmessage("error reading input stream \"%s\": %s", format->name(), strerror(errno));
                    results.size = 0;
                    return results;
                }
                if (format->eof()) {
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
                    if (format->error()) {
                        dlmessage("error reading input stream \"%s\": %s", format->name(), strerror(errno));
                        results.size = 0;
                        return results;
                    }
                    if (format->eof()) {
                        results.size = 0;
                        return results;
                    }
                }

                /* the rest of this audio packet is not in the input any more, so
                   discard it and start another packet with the data just read */
                if (format->discontinuity()) {
                    if (verbose>=2)
                        dlmessage("discarding %zd bytes of a 302m audio packet at a discontinuity in the input", i);

                    /* the new packet carries its own timestamp */
                    sts_t sts = format->get_pts();
                    if (sts>=0 && sts>last_sts) {
                        results.timestamp = last_sts = sts;
                        frames_since_pts = 0;
                    }

                    start = data;
                    end = data + read;
                    discontinuity = true;
                    break;
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

        /* the packet was abandoned, so start the next one */
        if (discontinuity)
            continue;

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
                case 20:
                    samples[numsamps+0] = ff_reverse[((ptr[0]&0x0f)<<4) | (ptr[1]>>4)];
                    samples[numsamps+1] = ff_reverse[((ptr[1]&0x0f)<<4) | (ptr[2]>>4)];
                    samples[numsamps+2] = ff_reverse[((ptr[3]&0x0f)<<4) | (ptr[4]>>4)];
                    samples[numsamps+3] = ff_reverse[((ptr[4]&0x0f)<<4) | (ptr[5]>>4)];
                    ptr += 6;
                    break;
                case 24:
                    samples[numsamps+0] = ff_reverse[ptr[1]];
                    samples[numsamps+1] = ff_reverse[ptr[2]];
                    samples[numsamps+2] = ff_reverse[((ptr[4]&0xf)<<4) | (ptr[5]>>4)];
                    samples[numsamps+3] = ff_reverse[((ptr[5]&0xf)<<4) | (ptr[6]>>4)];
                    ptr += 7;
                    break;
                default:
                    /* the sample size is checked when the decoder is attached */
                    dlexit("unsupported 302m sample size: %d", bits_per_sample);
            }
        }

    } while (discontinuity); //(numsamps<sampsize);

    /* numsamps counts bytes in the sample buffer, four per sample frame */
    results.size += numsamps;
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
            if (format->eof() || format->error()) {
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
            if (format->error()) {
                dlmessage("error reading input stream \"%s\": %s", format->name(), strerror(errno));
                results.size = 0;
                return results;
            }
            if (format->eof()) {
                off_t offset = 0;
                ret = mpg123_feedseek(m, 0, SEEK_SET, &offset);
                if (ret != MPG123_OK)
                    dlerror("failed to seek in audio stream: %d", mpg123_strerror(m));
                format->rewind();
                continue;
            }
        }

        /* the input has jumped, so throw away the part of a frame the decoder is
           holding rather than joining it to data which does not follow it */
        if (format->discontinuity())
            mpg123_open_feed(m);

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
    if (ac3_frame)
        free(ac3_frame);
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
            if (format->eof() || format->error()) {
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

    /* sync to the next frame and read the whole of it, starting again if the input
       jumps part way through, as the rest of that frame is not in the input */
    int length = 0;
    int flags, sample_rate, bit_rate;
    do {
        /* sync to next frame */
        do {
            /* look for sync in the buffered ac3 data, a sync word is seven bytes
               so the last offset worth testing is seven bytes from the end */
            int sync;
            for (sync=0; sync+7<=ac3_length; sync++) {
                length = a52_syncinfo(ac3_frame+sync, &flags, &sample_rate, &bit_rate);
                if (length)
                    break;
                //else
                //    dlmessage("ac_length=%d ac3_frame=%02x %02x %02x %02x", ac3_length, *(ac3_frame+sync+0), *(ac3_frame+sync+1), *(ac3_frame+sync+2), *(ac3_frame+sync+3));
            }

            /* discard the bytes before the sync word, so that the frame starts at
               the front of the buffer, or all but the last six bytes if there was
               no sync word, as a sync word can straddle the next read */
            if (sync) {
                if (verbose>=2 && length)
                    dlmessage("discarding %d bytes before the sync word of an ac3 frame", sync);
                memmove(ac3_frame, ac3_frame+sync, ac3_length-sync);
                ac3_length -= sync;
            }

            /* read more input if there is no whole sync word in the buffer */
            if (length==0) {
                const unsigned char *buf = format->read(&read);
                if (read==0) {
                    if (format->error()) {
                        dlmessage("error reading input stream \"%s\": %s", format->name(), strerror(errno));
                        results.size = 0;
                        return results;
                    }
                    if (format->eof()) {
                        format->rewind();
                        continue;
                    }

                    /* the input has no more audio to give, so there is no frame */
                    results.size = 0;
                    return results;
                }

                sts_t sts = format->get_pts();
                if (sts>=0 && sts>last_sts) {
                    last_sts = sts;
                    frames_since_pts = 0;
                    //dlmessage("new audio pts=%s", describe_sts(sts));
                }

                /* the input has jumped, so throw away the part of a frame in the
                   buffer rather than joining it to data which does not follow it */
                if (format->discontinuity()) {
                    if (verbose>=2 && ac3_length)
                        dlmessage("discarding %d bytes of ac3 audio at a discontinuity in the input", ac3_length);
                    ac3_length = 0;
                }

                memcpy(ac3_frame+ac3_length, buf, read);
                ac3_length += read;
            }

        } while (length==0);

        /* read data from transport stream to complete frame */
        while (ac3_length < length) {
            const unsigned char *buf = format->read(&read);
            if (read<=0) {
                if (format->error()) {
                    dlmessage("error reading input stream \"%s\": %s", format->name(), strerror(errno));
                    break;
                }
                if (format->eof()) {
                    format->rewind();
                    continue;
                }
            }

            /* the rest of this frame is not in the input, so start another one
               from the data just read */
            if (format->discontinuity()) {
                if (verbose>=2)
                    dlmessage("discarding %d bytes of an incomplete ac3 frame at a discontinuity in the input", ac3_length);
                memcpy(ac3_frame, buf, read);
                ac3_length = read;
                length = 0;
                break;
            }

            memcpy(ac3_frame+ac3_length, buf, read);
            ac3_length += read;
        }

    } while (length==0);

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
    results.size = 6*256*2*sizeof(int16_t); /* in bytes */

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
    fields = false;
    first_field = -1;
    pending = false;
    pending_pts = -1ll;
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

    /* decode with as many threads as the host will support */
    codeccontext->thread_count = 0;

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

    /* hevc codes interlaced video as separate fields, and ffmpeg flags only such a picture as interlaced */
    fields = codecid==AV_CODEC_ID_HEVC && got_frame && (frame->flags & AV_FRAME_FLAG_INTERLACED);
    if (fields) {
        height *= 2;
        interlaced = true;
    }
    switch (codeccontext->pix_fmt) {
        //case AV_PIX_FMT_YUV444P  : pixelformat = I444; break;
        case AV_PIX_FMT_YUV422P  :
        case AV_PIX_FMT_YUVJ422P : pixelformat = I422; break;
        case AV_PIX_FMT_YUV420P  :
        case AV_PIX_FMT_YUVJ420P : pixelformat = I420; break;
        case AV_PIX_FMT_YUV422P10LE : pixelformat = YU20; break;
        case AV_PIX_FMT_YUV420P10LE : pixelformat = YU15; break;
        //case AV_PIX_FMT_GRAY8    : pixelformat = Y800; break;
        default : dlexit("unknown chroma format: %s", av_get_pix_fmt_name(codeccontext->pix_fmt));
    }
    framerate = av_q2d(codeccontext->framerate);
    if (fields)
        framerate /= 2.0;
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

/* weave the field just decoded into the frame, returning true when it completes the frame */
bool dlffvideo::weave_field(unsigned char *buffer)
{
    const bool is8bit = pixelformat_is_8bit(pixelformat);
    const int rowbytes = is8bit? width*2 : ((width+47)/48)*128;
    const int top = (frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)? 1 : 0;

    /* each frame starts with a field of the parity decoded first, a lone field is skipped */
    if (first_field<0)
        first_field = top;
    if (!pending && top!=first_field) {
        if (verbose>=1)
            dlmessage("warning: skipped a %s field which has no first field", top? "top" : "bottom");
        return false;
    }
    if (pending && top==first_field) {
        if (verbose>=1)
            dlmessage("warning: skipped a %s field which has no second field", top? "top" : "bottom");
        pending = false;
    }

    /* the field goes into every other row, the top field into the first */
    unsigned char *out = buffer + (top? 0 : rowbytes);
    if (is8bit)
        convert_yuv_uyvy_rows((const unsigned char **)frame->data, frame->linesize, out, rowbytes*2, width, height/2, pixelformat);
    else
        convert_yuv10_v210_rows((const unsigned char **)frame->data, frame->linesize, out, rowbytes*2, width, height/2, pixelformat);

    if (!pending) {
        pending = true;
        pending_pts = frame->pts;
        return false;
    }
    pending = false;
    return true;
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

            if (format->discontinuity()) {
                av_parser_close(parser);
                parser = av_parser_init(codecid);
                if (parser==NULL)
                    dlexit("failed to re-initialise the %s parser", avcodec_get_name(codecid));
                avcodec_flush_buffers(codeccontext);
                got_frame = 0;
                last_sts = -1ll;
                frames_since_pts = 0;
                first_field = -1;
                pending = false;
            }
        }

        /* take a picture the decoder already holds before sending it more, as a packet can give two */
        if (!got_frame && avcodec_receive_frame(codeccontext, frame)==0)
            got_frame = 1;

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
                    dlexit("failed to send a packet for decoding: %s", av_make_error_string(errorstring, AV_ERROR_MAX_STRING_SIZE, ret));

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

            /* copy frame to the output buffer, 10-bit formats are packed as v210 */
            sts_t sts = frame->pts;
            if (fields) {
                /* weave a field into every other row, and wait for the second field of the frame */
                if (!weave_field(uyvy)) {
                    got_frame = 0;
                    continue;
                }
                sts = pending_pts;
                results.size = pixelformat_is_8bit(pixelformat)? width*height*2 : ((width+47)/48)*128 * height;
            } else if (pixelformat_is_8bit(pixelformat)) {
                convert_yuv_uyvy_rows((const unsigned char **)frame->data, frame->linesize, uyvy, width*2, width, height, pixelformat);
                results.size = width*height*2;
            } else {
                convert_yuv10_v210((const unsigned char **)frame->data, frame->linesize, uyvy, width, height, pixelformat);
                results.size = ((width+47)/48)*128 * height;
            }

            /* get timestamp from decoder */
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

/* the audio decoders all give stereo, which dlplay repeats in each pair of the output */
static const int FFAUDIO_CHANNELS = 2;

dlffaudio::dlffaudio(enum AVCodecID id)
{
    codecid = id;
    parser = NULL;
    codeccontext = NULL;
    frame = NULL;
    packet = NULL;
    got_frame = 0;
    swr = NULL;
    memset(&swr_layout, 0, sizeof(swr_layout));
    swr_rate = 0;
    swr_format = AV_SAMPLE_FMT_NONE;
    size = 0;
    ptr = NULL;
    buf_pts = AV_NOPTS_VALUE;
    last_sts = -1ll;
    samples_since_pts = 0;
    errorstring = (char *) malloc(AV_ERROR_MAX_STRING_SIZE);
}

dlffaudio::~dlffaudio()
{
    swr_free(&swr);
    av_channel_layout_uninit(&swr_layout);
    if (parser)
        av_parser_close(parser);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&codeccontext);
    free(errorstring);
}

int dlffaudio::attach(dlformat* f)
{
    int ret;

    /* attach the input source */
    format = f;

    /* read the first packet */
    do {
        const unsigned char *buf = format->read(&size);
        if (size==0) {
            dlmessage("failed to read any %s audio", avcodec_get_name(codecid));
            return -1;
        }
        ptr = buf;
    } while (size<2);
    sts_t sts = format->get_pts();
    buf_pts = sts>=0? sts : AV_NOPTS_VALUE;

    /* a pes packet of aac starts with a frame, so its sync word says whether the
       stream is adts or latm, whatever the stream type said */
    if (codecid==AV_CODEC_ID_AAC || codecid==AV_CODEC_ID_AAC_LATM) {
        if (ptr[0]==0xff && (ptr[1]&0xf6)==0xf0)
            codecid = AV_CODEC_ID_AAC;
        else if (ptr[0]==0x56 && (ptr[1]&0xe0)==0xe0)
            codecid = AV_CODEC_ID_AAC_LATM;
        else if (verbose>=1)
            dlmessage("no adts or latm sync word at the start of the aac audio, assuming %s", avcodec_get_name(codecid));
    }

    /* find required decoder */
    const AVCodec *codec = avcodec_find_decoder(codecid);
    if (!codec)
        dlexit("failed to find %s audio decoder", avcodec_get_name(codecid));

    /* initialise the parser */
    parser = av_parser_init(codec->id);
    if (!parser)
        dlexit("failed to initialise the %s parser", avcodec_get_name(codecid));

    /* initialise the codec context, timestamps are passed through in system time */
    codeccontext = avcodec_alloc_context3(codec);
    if (!codeccontext)
        dlexit("failed to initialise codec context");
    codeccontext->pkt_timebase = av_make_q(1, 180000);

    /* initialise the frame and packet */
    frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!frame || !packet) {
        dlmessage("failed to allocate audio frame");
        return -1;
    }

    /* the ac3 and e-ac3 decoders downmix with the levels the stream carries, which
       is better than the resampler's defaults, other decoders ignore the option */
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "downmix", "stereo", 0);
    ret = avcodec_open2(codeccontext, codec, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        dlmessage("failed to open audio codec: %s", av_make_error_string(errorstring, AV_ERROR_MAX_STRING_SIZE, ret));
        return ret;
    }

    /* decode up to the first frame with a timestamp, to find the audio format */
    int skipped = 0;
    while (1) {
        if (receive_frame()<=0) {
            dlmessage("failed to decode any %s audio", avcodec_get_name(codecid));
            return -1;
        }
        if (frame->pts!=AV_NOPTS_VALUE)
            break;
        skipped++;
    }
    got_frame = 1;
    if (verbose>=2 && skipped)
        dlmessage("discarded %d %s frames before the first timestamp", skipped, avcodec_get_name(codecid));

    /* report the format parameters */
    char layout[64];
    av_channel_layout_describe(&frame->ch_layout, layout, sizeof(layout));
    const char *profile = av_get_profile_name(codec, codeccontext->profile);
    if (verbose>=0)
        dlmessage("audio format is %s%s%s %.1fkHz %s, %d samples a frame", avcodec_get_name(codecid), profile? " " : "", profile? profile : "", frame->sample_rate/1000.0, layout, frame->nb_samples);

    return setup_resampler();
}

/* receive the next decoded frame, reading and parsing as much input as that takes,
   returns 1 for a frame and 0 at the end of the input or on an error */
int dlffaudio::receive_frame()
{
    while (1) {
        /* take a frame the decoder already has */
        int ret = avcodec_receive_frame(codeccontext, frame);
        if (ret==0)
            return 1;
        if (ret!=AVERROR(EAGAIN)) {
            dlmessage("error during decoding audio: %s", av_make_error_string(errorstring, AV_ERROR_MAX_STRING_SIZE, ret));
            return 0;
        }

        /* read more input */
        if (size==0) {
            const unsigned char *buf = format->read(&size);
            if (size==0)
                return 0;
            ptr = buf;

            if (format->discontinuity())
                reset();

            sts_t sts = format->get_pts();
            buf_pts = sts>=0? sts : AV_NOPTS_VALUE;
        }

        /* use the parser to split the data into frames, the pts goes with the first
           call only, as the parser gives it to every frame starting after that call */
        ret = av_parser_parse2(parser, codeccontext, &packet->data, &packet->size, ptr, size, buf_pts, AV_NOPTS_VALUE, 0);
        if (ret < 0)
            dlexit("failed to parse %s data", avcodec_get_name(codecid));
        ptr += ret;
        size -= ret;
        buf_pts = AV_NOPTS_VALUE;

        if (packet->size) {
            packet->pts = parser->pts;
            packet->dts = parser->dts;
            /* a corrupt frame is dropped by the decoder, which is not fatal in a broadcast */
            ret = avcodec_send_packet(codeccontext, packet);
            if (ret<0 && verbose>=1)
                dlmessage("failed to decode a frame of %s audio: %s", avcodec_get_name(codecid), av_make_error_string(errorstring, AV_ERROR_MAX_STRING_SIZE, ret));
        }
    }
}

/* the input has jumped, so the parser is holding the last frame before the jump,
   which it only gives up when no data follows it: send that frame to the decoder,
   then start the parser again rather than join the next frame to data which does
   not follow it, the decoder and resampler carry on as there is nothing to reference */
void dlffaudio::reset()
{
    uint8_t *data;
    int bytes;
    av_parser_parse2(parser, codeccontext, &data, &bytes, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (bytes) {
        packet->data = data;
        packet->size = bytes;
        packet->pts = parser->pts;
        packet->dts = parser->dts;
        /* the decoder takes a copy of the data, which belongs to the parser */
        int ret = avcodec_send_packet(codeccontext, packet);
        if (ret<0 && verbose>=1)
            dlmessage("failed to decode the last frame of %s audio before a discontinuity: %s", avcodec_get_name(codecid), av_make_error_string(errorstring, AV_ERROR_MAX_STRING_SIZE, ret));
    }

    av_parser_close(parser);
    parser = av_parser_init(codecid);
    if (parser==NULL)
        dlexit("failed to re-initialise the %s parser", avcodec_get_name(codecid));
}

/* convert the format of the current frame to 48kHz 16-bit interleaved stereo,
   downmixing or upmixing as need be */
int dlffaudio::setup_resampler()
{
    AVChannelLayout out;
    av_channel_layout_default(&out, FFAUDIO_CHANNELS);

    swr_free(&swr);
    int ret = swr_alloc_set_opts2(&swr, &out, AV_SAMPLE_FMT_S16, 48000, &frame->ch_layout, (enum AVSampleFormat)frame->format, frame->sample_rate, 0, NULL);
    av_channel_layout_uninit(&out);
    if (ret<0 || (ret = swr_init(swr))<0) {
        dlmessage("failed to initialise the audio resampler: %s", av_make_error_string(errorstring, AV_ERROR_MAX_STRING_SIZE, ret));
        return -1;
    }

    /* remember the input so that a change of format can be seen */
    av_channel_layout_uninit(&swr_layout);
    av_channel_layout_copy(&swr_layout, &frame->ch_layout);
    swr_rate = frame->sample_rate;
    swr_format = frame->format;

    return 0;
}

decode_t dlffaudio::decode(unsigned char *samples, size_t sampsize) // sampsize is in bytes.
{
    decode_t results = {0, -1ll, 0ll, 0ll};

    /* start timer */
    unsigned long long start = get_utime();

    /* the resampler can take a frame without giving any samples back yet */
    int num_samples = 0;
    while (num_samples==0) {
        if (!got_frame && !receive_frame()) {
            if (format->error())
                dlmessage("error reading input stream \"%s\": %s", format->name(), strerror(errno));
            return results;
        }
        got_frame = 0;

        /* the channels or rate can change, at a programme boundary for instance */
        if (frame->sample_rate!=swr_rate || frame->format!=swr_format || av_channel_layout_compare(&frame->ch_layout, &swr_layout)) {
            if (verbose>=1) {
                char layout[64];
                av_channel_layout_describe(&frame->ch_layout, layout, sizeof(layout));
                dlmessage("audio format changed to %.1fkHz %s", frame->sample_rate/1000.0, layout);
            }
            if (setup_resampler()<0)
                return results;
        }

        /* the first sample out is behind the first sample of this frame by the
           samples the resampler is holding */
        if (frame->pts!=AV_NOPTS_VALUE) {
            last_sts = frame->pts - swr_get_delay(swr, 180000);
            samples_since_pts = 0;
        }
        results.timestamp = last_sts + samples_since_pts*180000ll/48000ll;

        /* convert as much as fits, the resampler keeps the rest for the next call */
        num_samples = swr_convert(swr, &samples, sampsize/(FFAUDIO_CHANNELS*2), (const uint8_t **)frame->extended_data, frame->nb_samples);
        if (num_samples<0) {
            dlmessage("failed to convert audio samples: %s", av_make_error_string(errorstring, AV_ERROR_MAX_STRING_SIZE, num_samples));
            num_samples = 0;
        }
        samples_since_pts += num_samples;
    }

    results.size = num_samples*FFAUDIO_CHANNELS*2;
    results.decode_time = get_utime() - start;

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

    const AVCodec *codec;
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

    /* decode with as many threads as the host will support */
    codeccontext->thread_count = 0;

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
        case AV_PIX_FMT_YUV422P10LE : pixelformat = YU20; break;
        case AV_PIX_FMT_YUV420P10LE : pixelformat = YU15; break;
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
        av_dump_format(formatcontext, stream_index, format->name(), 0);

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

            /* copy frame to the output buffer, 10-bit formats are packed as v210 */
            if (pixelformat_is_8bit(pixelformat)) {
                convert_yuv_uyvy_rows((const unsigned char **)frame->data, frame->linesize, uyvy, width*2, width, height, pixelformat);
                results.size = width*height*2;
            } else {
                convert_yuv10_v210((const unsigned char **)frame->data, frame->linesize, uyvy, width, height, pixelformat);
                results.size = ((width+47)/48)*128 * height;
            }
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
