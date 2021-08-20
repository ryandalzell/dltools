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
    /* buffer */
    size = 0;
    data = NULL;
    verbose = 0;
    /* timestamp */
    last_pts = 0;
    timestamp = 0;
    frames_since_pts = 0;
    /* debug */
    top_field_first = 1;
    blank_field = 0;
}

dldecode::~dldecode()
{
    if (data)
        free(data);
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
    size = pixelformat==I422? width*height*2 : width*height*3/2;
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
        if (!lumaonly)
            convert_i420_uyvy(data, uyvy, width, height, pixelformat);
        else
            convert_i420_uyvy_lumaonly(data, uyvy, width, height);

    }

    results.timestamp = timestamp;
    timestamp += llround(90000.0/framerate);

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
}

dlmpeg2::~dlmpeg2()
{
    if (mpeg2dec)
        mpeg2_close(mpeg2dec);
}

int dlmpeg2::attach(dlformat *f)
{
    /* attach the input source */
    format = f;

    /* allocate the read buffer */
    size = 32*1024;
    data = (unsigned char *) malloc(size);

    /* find the first sequence header */
    int seq_found = 0;
    int read = 1;
    do {
        mpeg2_state_t state = mpeg2_parse(mpeg2dec);
        switch (state) {
            case STATE_BUFFER:
                /* read a chunk of data from input */
                read = format->read(data, size);
                if (read>0) {
                    mpeg2_buffer(mpeg2dec, data, data+read);
                    /* tag with most recent available timestamp */
                    tstamp_t pts = format->get_timestamp();
                    mpeg2_tag_picture(mpeg2dec, (uint32_t)pts, uint32_t(pts>>32));
                }
                break;

            case STATE_SEQUENCE:
                width = info->sequence->width;
                height = info->sequence->height;
                interlaced = height==720? 0 : 1;
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

    int read;
    do {
        mpeg2_state_t state = mpeg2_parse(mpeg2dec);
        switch (state) {
            case STATE_BUFFER:
                /* read a chunk of data from input */
                read = format->read(data, size);
                if (read==0 || format->get_source()->eof()) {
                    format->get_source()->rewind();
                    read = format->read(data, size);
                }
                if (read>0) {
                    mpeg2_buffer(mpeg2dec, data, data+read);
                    /* tag with most recent available timestamp */
                    tstamp_t pts = format->get_timestamp();
                    mpeg2_tag_picture(mpeg2dec, (uint32_t)pts, uint32_t(pts>>32));
                } else
                    return results;
                break;

            case STATE_SLICE:
            case STATE_END:
                if (info->display_fbuf) {
                    const unsigned char *yuv[3] = {info->display_fbuf->buf[0], info->display_fbuf->buf[1], info->display_fbuf->buf[2]};
                    convert_yuv_uyvy(yuv, uyvy, width, height, pixelformat);
                    results.size = width*height*2;
                    tstamp_t pts = -1;
                    if (info->current_picture)
                        pts = ((tstamp_t)(info->current_picture->tag2)<<32) | (tstamp_t)info->current_picture->tag;
                    if (pts<0 || pts<=last_pts) {
                        /* extrapolate a timestamp if necessary */
                        pts = last_pts + llround(90000.0/framerate);
                        //dlmessage("calc video pts=%s delta=%lld", describe_timestamp(pts), llround(90000.0/framerate));
                    } //else
                        //dlmessage("new video pts=%s", describe_timestamp(pts));
                    results.timestamp = last_pts = pts;

                    return results;
                }
                break;

            default:
                break;
        }
    } while(1);

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

    /* allocate the read buffer */
    size = 184;
    data = (unsigned char *) malloc(size);

    /* find the audio format */
    do {
        size_t bytes;
        int read = format->read(data, size);
        if (read==0) {
            if (format->get_source()->eof() || format->get_source()->error()) {
                dlmessage("failed to sync mpa audio");
                return -1;
            }
        }

        long long pts = format->get_timestamp();
        if (pts>=0) {
            last_pts = pts;
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
        int read = format->read(data, size);
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

        long long pts = format->get_timestamp();
        if (pts>=0 && pts>last_pts) {
            results.timestamp = last_pts = pts;
            frames_since_pts = 0;
            //dlmessage("new audio pts=%s", describe_timestamp(pts));
        }

        ret = mpg123_decode(m, data, read, samples, sampsize, &results.size);
    } while (results.size==0 && ret!=MPG123_ERR);

    /* extrapolate a timestamp if necessary */
    if (results.timestamp<0) {
        frames_since_pts++;
        results.timestamp = last_pts + (tstamp_t)(frames_since_pts*90000ll*1152ll/48000ll);
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
    last_pts = -1;
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

    /* allocate the read buffer */
    size = 184;
    data = (unsigned char *) malloc(size);

    /* find the ac3 audio format */
    int flags = 0, sample_rate = 0, bit_rate = 0;

    /* allocate the audio buffers */
    ac3_size = 3840+188;
    ac3_frame = (unsigned char *)malloc(ac3_size);

    unsigned int sync = 0;
    size_t read;
    int ret = 0;
    do {
        read = format->read(data, size);
        if (read==0) {
            if (format->get_source()->eof() || format->get_source()->error()) {
                dlmessage("failed to sync ac3 audio");
                break;
            }
        }

        /* look for first pts */
        tstamp_t pts = format->get_timestamp();
        if (pts>=0) {
            last_pts = pts;
            frames_since_pts = 0;
        }

        /* look for sync in ac3 stream FIXME this won't work if sync is in last 7 bytes of packet */
        for (sync=0; sync<read-7; sync++) {
            ret = a52_syncinfo(data+sync, &flags, &sample_rate, &bit_rate);
            if (ret)
                break;
        }
    } while (ret==0 || last_pts<0);

    /* queue the synchronised data */
    memcpy(ac3_frame, data+sync, read-sync);
    ac3_length = read-sync;
    dlmessage("found a52 frame of %d bytes, %d in buffer, initial pts=%s", ret, ac3_length, describe_timestamp(last_pts));

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
    int read;

    /* sync to next frame */
    int length = 0;
    int flags, sample_rate, bit_rate;
    do {
        if (ac3_length<7) {
            read = format->read(data, size);
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

            tstamp_t pts = format->get_timestamp();
            if (pts>=0 && pts>last_pts) {
                last_pts = pts;
                frames_since_pts = 0;
                //dlmessage("new audio pts=%s", describe_timestamp(pts));
            }
            memcpy(ac3_frame+ac3_length, data, read);
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
            read = format->read(data, size);
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
            memcpy(ac3_frame+ac3_length, data, read);
            ac3_length += read;
        }
    } while (0);

    /* feed the frame to the audio decoder */
    flags = A52_STEREO | A52_ADJUST_LEVEL;
    sample_t level = 1.0;
    sample_t bias = 384.0;
    if (a52_frame(a52_state, ac3_frame, &flags, &level, bias))
        dlmessage("failed: a52_block");

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
    results.timestamp = last_pts + (tstamp_t)(frames_since_pts*90000ll*6ll*256ll/48000ll);
    frames_since_pts++;

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

    /* allocate the read buffer */
    size = 32*1024;
    data = (unsigned char *) malloc(size);

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
            int n = format->read(data, size);
            if (n) {
                tstamp_t timestamp = format->get_timestamp();
                err = de265_push_data(ctx, data, n, timestamp, NULL);
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
                int read = format->read(data, size);
                if (read==0 || format->get_source()->eof()) {
                    format->get_source()->rewind();
                    read = format->read(data, size);
                }
                if (read>0) {
                    /* use most recent available timestamp */
                    tstamp_t timestamp = format->get_timestamp();
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
            tstamp_t pts = de265_get_image_PTS(image);
            if (pts<0 || pts<=last_pts) {
                /* extrapolate a timestamp if necessary */
                pts = last_pts + llround(90000.0/framerate);
            }
            results.timestamp = last_pts = pts;
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
                    int read = format->read(data, size);
                    if (read==0 || format->get_source()->eof()) {
                        format->get_source()->rewind();
                        read = format->read(data, size);
                    }
                    if (read>0) {
                        /* use most recent available timestamp */
                        tstamp_t timestamp = format->get_timestamp();
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
                    tstamp_t pts = de265_get_image_PTS(image);
                    if (pts<0 || pts<=last_pts) {
                        /* extrapolate a timestamp if necessary */
                        pts = last_pts + llround(90000.0/framerate);
                    }
                    results.timestamp = last_pts = pts;
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

    int stream_index;
    int ret = av_find_best_stream(formatcontext, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ret < 0) {
        dlmessage("failed to find video stream in input file");
        return ret;
    }
    stream_index = ret;

    /* find decoder for the stream */
    AVStream *stream = formatcontext->streams[stream_index];
    codeccontext = stream->codec;
    AVCodec *dec = avcodec_find_decoder(codeccontext->codec_id);
    if (!dec) {
        dlmessage("failed to find video codec");
        return -1;
    }

    /* Init the decoders, with or without reference counting */
    AVDictionary *opts = NULL;
    //if (api_mode == API_MODE_NEW_API_REF_COUNT)
    //    av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(codeccontext, dec, &opts)) < 0) {
        dlmessage("failed to open video codec");
        return ret;
    }

    {
        /* FIXME accessing the codeccontext struct in this way is hacky */
        width = codeccontext->width;
        height = codeccontext->height;
        interlaced = codeccontext->field_order!=AV_FIELD_PROGRESSIVE;
        switch (codeccontext->pix_fmt) {
          //case AV_PIX_FMT_YUV444P  : pixelformat = I444; break;
            case AV_PIX_FMT_YUV422P  :
            case AV_PIX_FMT_YUVJ422P : pixelformat = I422; break;
            case AV_PIX_FMT_YUV420P  :
            case AV_PIX_FMT_YUVJ420P : pixelformat = I420; break;
          //case AV_PIX_FMT_GRAY8    : pixelformat = Y800; break;
            default : dlexit("unknown chroma format: %s", av_get_pix_fmt_name(codeccontext->pix_fmt));
        }
        //framerate = context->framerate.num / context->framerate.den;
        framerate = av_q2d(codeccontext->framerate);

        /* dump input information to stderr */
        if (verbose>=1)
            av_dump_format(formatcontext, 0, format->get_source()->name(), 0);
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
    av_init_packet(&packet);
    packet.data = NULL;
    packet.size = 0;

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
        if (packet.size <= 0) {
            if (av_read_frame(formatcontext, &packet) < 0)
                break;
        }

        // TODO check stream index?

        int len = avcodec_decode_video2(codeccontext, frame, &got_frame, &packet);
        if (len < 0) {
            dlmessage("error while decoding frame: %s", av_make_error_string(errorstring, AV_ERROR_MAX_STRING_SIZE, len));
            break;
        }

        if (got_frame) {
            /* timestamp decode time */
            unsigned long long decode = get_utime();
            results.decode_time = decode - start;

            /* copy frame to uyvy buffer */
            convert_yuv_uyvy((const unsigned char **)frame->data, uyvy, width, height, pixelformat);
            results.size = width*height*2;
            //tstamp_t pts = de265_get_image_PTS(image);
            //if (pts<0 || pts<=last_pts) {
                /* extrapolate a timestamp if necessary */
            tstamp_t pts = last_pts + llround(90000.0/framerate);
            //}
            results.timestamp = last_pts = pts;

            /* measure render time */
            results.render_time = get_utime() - decode;
        }

        packet.size -= len;
        packet.data += len;

        if (got_frame)
            break;
    }

    return results;
}
#endif // HAVE_FFMPEG
