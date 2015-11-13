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
    size = 0;
    data = NULL;
    verbose = 0;
    /* timestamp */
    last_pts = 0;
    timestamp = 0;
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

int dldecode::rewind(int frame)
{
    return format->get_source()->rewind();
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
    if (divine_video_format(format->get_source()->name(), &width, &height, &interlaced, &framerate, &pixelformat)<0)
        dlexit("failed to determine output video format from filename: %s", format->get_source()->name());

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

    /* loop input stream */
    if (atend()) {
        rewind(0);
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
                /* for elementary stream we can just read a chunk of data from input */
                read = format->read(data, size);
                mpeg2_buffer(mpeg2dec, data, data+read);
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
    decode_t results = {0, 0};

    int read;
    do {
        mpeg2_state_t state = mpeg2_parse(mpeg2dec);
        switch (state) {
            case STATE_BUFFER:
                /* for elementary stream we can just read a chunk of data from input */
                read = format->read(data, size);
                if (read==0 || format->get_source()->eof()) {
                    format->get_source()->rewind();
                    read = format->read(data, size);
                }
                if (read>0)
                    mpeg2_buffer(mpeg2dec, data, data+read);
                else
                    return results;
                break;

            case STATE_SLICE:
            case STATE_END:
                if (info->display_fbuf) {
                    const unsigned char *yuv[3] = {info->display_fbuf->buf[0], info->display_fbuf->buf[1], info->display_fbuf->buf[2]};
                    convert_yuv_uyvy(yuv, uyvy, width, height, pixelformat);
                    results.size = width*height*2;
                    results.timestamp = timestamp;
                    timestamp += llround(90000.0/framerate);
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

    /* initialise the transport stream parser */
    pid = 0;
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
#if 0
    /* skip psi parsing if audio pid specified */
    if (pid==0) {
        int stream_types[] = { 0x03, 0x04 };
        pid = find_pid_for_stream_type(stream_types, sizeof(stream_types)/sizeof(int), source);
    }

    if (pid==0)
        /* no mpeg audio pid found */
        return -1;
    dlmessage("mpeg audio pid is %d", pid);

    /* find the first pes with a pts */
    do {
        int read = next_pes_packet_data(data, &pts, pid, 1, source);
        if (read==0) {
            if (format->get_source()->eof() || format->get_source()->error()) {
                dlmessage("failed to sync mpa audio");
                pid = 0;
                return -1;
            }
        }

        /* look for first pts */
        if (pts>=0) {
            last_pts = pts;
            frames_since_pts = 0;
        }
    }
    while (last_pts<0);

    /* find the audio format */
    do {
        size_t bytes;
        int read = next_pes_packet_data(data, &pts, pid, 0, source);
        if (read==0) {
            if (format->get_source()->eof() || format->get_source()->error()) {
                dlmessage("failed to sync mpa audio");
                pid = 0;
                return -1;
            }
        }

        if (pts>=0) {
            last_pts = pts;
            frames_since_pts = 0;
        }

        // TODO try mpg123_feed
        ret = mpg123_decode(m, data, read, NULL, 0, &bytes);
        //ret = mpg123_feed(m, data, read);
        if (ret==MPG123_ERR || ret==MPG123_DONE) {
            dlmessage("failed to determine format of audio data: %s", mpg123_strerror(m));
            pid = 0;
            return -1;
        }
    } while (ret!=MPG123_NEW_FORMAT);

    long rate;
    int channels, enc;
    mpg123_getformat(m, &rate, &channels, &enc);
    dlmessage("audio format is  %ldHz x%d channels", rate, channels);
#endif
    return 0;
}

decode_t dlmpg123::decode(unsigned char *samples, size_t sampsize)
{
    decode_t results = {0, -1};

    /* feed the audio decoder */
    do {
        int read = format->read(data, size);
        if (read==0) {
            if (format->get_source()->error()) {
                dlmessage("error reading input stream \"%s\": %s", format->get_source()->name(), strerror(errno));
                pid = 0;
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

        if (pts>=0) {
            last_pts = pts;
            frames_since_pts = 0;
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
    ac3_frame = NULL;

    /* initialise the transport stream parser */
    pid = 0;
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
#if 0
    /* skip psi parsing if audio pid specified */
    if (pid==0) {
        int stream_types[] = { 0x81 };
        pid = find_pid_for_stream_type(stream_types, sizeof(stream_types)/sizeof(int), source);
    }

    if (pid==0)
        /* no ac3 audio pid found */
        return -1;
    dlmessage("ac3 audio pid is %d", pid);

    /* find the ac3 audio format */
    int flags = 0, sample_rate = 0, bit_rate = 0;

    /* allocate the audio buffers */
    ac3_frame = (unsigned char *)malloc(3840+188);

    unsigned int sync = 0;
    size_t read;
    int ret = 0;
    do {
        long long pts;
        read = next_pes_packet_data(data, &pts, pid, 1, source);
        if (read==0) {
            if (format->get_source()->eof() || format->get_source()->error()) {
                dlmessage("failed to sync ac3 audio");
                pid = 0;
                break;
            }
        }

        /* look for first pts */
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
    } while (ret==0 && last_pts<0);

    /* queue the synchronised data */
    memcpy(ac3_frame, data+sync, read-sync);
    ac3_length = read-sync;

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
#endif
    return 0;
}

decode_t dlliba52::decode(unsigned char *frame, size_t framesize)
{
    decode_t results = {0, -1};
    int read;
    long long pts;

    /* sync to next frame */
    int length = 0;
    int flags, sample_rate, bit_rate;
    do {
        if (ac3_length<7) {
            read = format->read(data, size);
            if (read==0) {
                if (format->get_source()->error()) {
                    dlmessage("error reading input stream \"%s\": %s", format->get_source()->name(), strerror(errno));
                    pid = 0;
                    results.size = 0;
                    return results;
                }
                if (format->get_source()->eof()) {
                    format->get_source()->rewind();
                    continue;
                }
            }
            if (pts>=0) {
                last_pts = pts;
                frames_since_pts = 0;
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
        }

        //length = a52_syncinfo(ac3_frame, &flags, &sample_rate, &bit_rate);
        //if (length==0)
        //    fprintf(stderr, "length=%d ac_length=%d ac3_frame=%02x %02x %02x %02x\n", length, ac3_length, ac3_frame[0], ac3_frame[1], ac3_frame[2], ac3_frame[3]);
    } while (0);

    /* prepare the next frame for decoding */
    do {
        /* read data from transport stream to complete frame */
        while (ac3_length < length) {
            read = format->read(data, size);
            if (read==0) {
                if (format->get_source()->error()) {
                    dlmessage("error reading input stream \"%s\": %s", format->get_source()->name(), strerror(errno));
                    pid = 0;
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
    flags = A52_STEREO;
    sample_t level = 32767.0;
    sample_t bias = 0.0;
    a52_frame(a52_state, ac3_frame, &flags, &level, bias);

    /* decode audio frame */
    int i, j;
    for (i=0; i<6; i++) {
        uint16_t *ac3_frame = (uint16_t *) frame;

        a52_block(a52_state);

        /* convert decoded samples to integer and interleave */
        for (j=0; j<256; j++) {
            ac3_frame[i*512+j*2  ] = (int16_t) lround(sample[j    ]);
            ac3_frame[i*512+j*2+1] = (int16_t) lround(sample[j+256]);
        }
    }
    results.size = 6*256*2;

    /* keep leftover data for next frame */
    if (ac3_length-length)
        memcpy(ac3_frame, ac3_frame+length, ac3_length-length);
    ac3_length = ac3_length-length;

    /* extrapolate a timestamp if necessary */
    if (results.timestamp<0) {
        frames_since_pts++;
        results.timestamp = last_pts + (tstamp_t)(frames_since_pts*90000ll/48000ll*6ll*256ll);
    }

    return results;
}

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
            default : dlerror("unknown chroma format");
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
            results.timestamp = pts;
            last_pts = pts;

            timestamp += llround(90000.0/framerate);
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

                /* use poc to decide field order, this works around a potential bug in libde265 */
                int poc = de265_get_image_picture_order_count(image);

                /* deinterlace */
                if ((poc&1)==!top_field_first) { /* works for negative poc too */
                    convert_top_field_yuv_uyvy(yuv, uyvy, width, height, pixelformat);
                    /* move field on if it is the correct order */
                    field += (top_field_first ^ field);
                } else if ((poc&1)==top_field_first) {
                    convert_bot_field_yuv_uyvy(yuv, uyvy, width, height, pixelformat);
                    field += !(top_field_first ^ field);
                }

                /* use timestamp from first field for display of deinterlaced frame */
                if ((poc&1)==!top_field_first) {
                    results.size = width*height*2;
                    tstamp_t pts = de265_get_image_PTS(image);
                    if (pts<0 || pts<=last_pts) {
                        /* extrapolate a timestamp if necessary */
                        pts = last_pts + llround(90000.0/framerate);
                    }
                    results.timestamp = pts;
                    last_pts = pts;
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
