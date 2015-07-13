/*
 * Description: object interfaces to decoding libraries
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2011 4i2i Communications Ltd.
 */

#include <string.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>

#include "dldecode.h"
#include "dlutil.h"
#include "dlconv.h"
#include "dlts.h"

dldecode::dldecode()
{
    file = NULL;
    size = 0;
    data = NULL;
    timestamp = 0;
}

dldecode::~dldecode()
{
    if (data)
        free(data);
    if (file)
        fclose(file);
}

int dldecode::attach(const char *f)
{
    filename = f;
    return 0;
}

int dldecode::rewind(int frame)
{
    if (fseek(file, 0, SEEK_SET)<0)
        dlerror("failed to seek in file \"%s\"", filename);
    return 0;
}

int dlyuv::attach(const char *f)
{
    /* attach the input yuv file */
    filename = f;

    /* open the input file */
    file = fopen(filename, "rb");
    if (!file)
        dlerror("error: failed to open input file \"%s\"", filename);

    /* determine the video format */
    if (divine_video_format(filename, &width, &height, &interlaced, &framerate, &pixelformat)<0)
        dlexit("failed to determine output video format from filename: %s", filename);

    /* allocate the read buffer */
    size = pixelformat==I422? width*height*2 : width*height*3/2;
    data = (unsigned char *) malloc(size);

    /* stat the input file */
    struct stat stat;
    fstat(fileno(file), &stat);

    /* calculate the number of frames in the input file */
    maxframes = stat.st_size / size;

    return 0;
}

bool dlyuv::atend()
{
    return ftello(file)/size==maxframes;
}

int dlyuv::rewind(int frame)
{
    off_t skip = frame * size;
    if (fseeko(file, skip, SEEK_SET)<0)
        dlerror("failed to seek in input file");
    return 0;
}

decode_t dlyuv::decode(unsigned char *uyvy, size_t uyvysize)
{
    decode_t results = {0, 0};

    /* loop input file */
    if (atend()) {
        rewind(0);
    }

    if (pixelformat==UYVY) {
        /* read directly into frame */
        if (fread(uyvy, width*height*2, 1, file)!=1)
            dlerror("failed to read frame from input file");
        results.size = width*height*2;
    } else {
        /* read frame from file */
        if (fread(data, size, 1, file)!=1)
            dlerror("failed to read frame from input file");
        results.size = size;

        /* convert to uyvy */
        if (!lumaonly)
            convert_i420_uyvy(data, uyvy, width, height, pixelformat);
        else
            convert_i420_uyvy_lumaonly(data, uyvy, width, height);

    }

    results.timestamp = timestamp;
    timestamp += lround(90000.0/framerate);

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

int dlmpeg2::attach(const char *f)
{
    /* attach the input m2v file */
    filename = f;

    /* open the input file */
    file = fopen(filename, "rb");
    if (!file)
        dlerror("error: failed to open input file \"%s\"", filename);

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
                /* for elementary stream we can just read a chunk of data from file */
                read = fread(data, 1, size, file);
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
                /* for elementary stream we can just read a chunk of data from file */
                read = fread(data, 1, size, file);
                if (read==0 || feof(file)) {
                    if (fseek(file, 0, SEEK_SET)<0)
                        dlerror("failed to seek in file \"%s\"", filename);
                    read = fread(data, 1, size, file);
                }
                mpeg2_buffer(mpeg2dec, data, data+read);
                break;

            case STATE_SLICE:
            case STATE_END:
                if (info->display_fbuf) {
                    const unsigned char *yuv[3] = {info->display_fbuf->buf[0], info->display_fbuf->buf[1], info->display_fbuf->buf[2]};
                    convert_yuv_uyvy(yuv, uyvy, width, height, pixelformat);
                    results.size = width*height*2;
                    results.timestamp = timestamp;
                    timestamp += lround(90000.0/framerate);
                    return results;
                }
                break;

            default:
                break;
        }
    } while(1);

    return results;
}

dlmpeg2ts::dlmpeg2ts()
{
    dlmpeg2();
    pid = 0;
    first_pts = -1;
    last_pts = -1;
    frames_since_pts = 0;
    offset_pts = 0;
}

int dlmpeg2ts::attach(const char *f)
{
    /* attach the input m2v file */
    filename = f;

    /* open the input file */
    file = fopen(filename, "rb");
    if (!file)
        dlerror("error: failed to open input file \"%s\"", filename);

    /* allocate the read buffer */
    size = 184;
    data = (unsigned char *) malloc(size);

    /* skip psi parsing if video pid specified */
    if (pid==0) {
        int stream_types[] = { 0x02, 0x80 };
        pid = find_pid_for_stream_type(stream_types, sizeof(stream_types)/sizeof(int), filename, file);
    }

    if (pid==0) {
        dlmessage("could not find video pid in file \"%s\"", filename);
        return -1;
    }
    dlmessage("mpeg video pid is %d", pid);

    /* find the first sequence header */
    int read = 1, done = 0;
    do {
        mpeg2_state_t state = mpeg2_parse(mpeg2dec);
        switch (state) {
            case STATE_BUFFER:
            {
                long long pts;
                /* for transport streams we need to extract data from pes packets,
                * and pes packets from transport stream packets */
                read = next_pes_packet_data(data, &pts, pid, 0, file);
                if (pts>=0) {
                    /* tag the picture with the pts so it can be retrieved when the picture is decoded */
                    mpeg2_tag_picture(mpeg2dec, pts, pts);

                    /* make a note of the pts */
                    last_pts = pts;
                    frames_since_pts = 0;
                }
                mpeg2_buffer(mpeg2dec, data, data+read);
                break;
            }

            case STATE_SEQUENCE:
                width = info->sequence->width;
                height = info->sequence->height;
                interlaced = height==720? 0 : 1;
                framerate = 27000000.0/info->sequence->frame_period;
                pixelformat = info->sequence->height==info->sequence->chroma_height? I422 : I420;
                done = 1;
                break;

            default:
                break;
        }

        if (done && last_pts>=0)
            break;
    } while (read);

    if (read==0) {
        /* failed to attach */
        dlmessage("could not find an mpeg2 sequence header in transport stream \"%s\"", filename);
        return -1;
    }

    return 0;
}

decode_t dlmpeg2ts::decode(unsigned char *uyvy, size_t uyvysize)
{
    decode_t results = {0, -1};

    /* tag the picture with a marker that can be overwritten with a pts */
    //mpeg2_tag_picture(mpeg2dec, -1, -1);

    int read, done = 0;
    do {
        mpeg2_state_t state = mpeg2_parse(mpeg2dec);
        switch (state) {
            case STATE_BUFFER:
            {
                long long pts;
                /* for transport streams we need to extract data from pes packets,
                * and pes packets from transport stream packets */
                read = next_pes_packet_data(data, &pts, pid, 0, file);
                if (read==0 || feof(file)) {
                    if (fseek(file, 0, SEEK_SET)<0)
                        dlerror("failed to seek in file \"%s\"", filename);
                    read = next_pes_packet_data(data, &pts, pid, 0, file);

                    /* offset the timestamp for looped decoding */
                    offset_pts += last_pts + (tstamp_t)(frames_since_pts*90000ll/framerate) - first_pts;
                }
                if (pts>=0) {
                    /* tag the picture with the pts so it can be retrieved when the picture is decoded */
                    mpeg2_tag_picture(mpeg2dec, pts, pts);

                    /* make a note of the pts */
                    last_pts = pts;
                    frames_since_pts = 0;
                }
                mpeg2_buffer(mpeg2dec, data, data+read);
                break;
            }

            case STATE_SLICE:
            case STATE_END:
                if (info->display_fbuf) {
                    const unsigned char *yuv[3] = {info->display_fbuf->buf[0], info->display_fbuf->buf[1], info->display_fbuf->buf[2]};
                    convert_yuv_uyvy(yuv, uyvy, width, height, pixelformat);
                    results.size = width*height*2;
                    results.timestamp = info->display_picture->tag;
                    done = 1;
                }
                break;

            default:
                break;
        }
    } while (!done);

    /* extrapolate a timestamp if necessary */
    if (results.timestamp<=0) {
        frames_since_pts++;
        results.timestamp = last_pts + (tstamp_t)(frames_since_pts*90000ll/framerate);
    }
    results.timestamp += offset_pts;

    /* record the timestamp of the first frame */
    if (first_pts<0)
        first_pts = results.timestamp;

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
    last_pts = -1;
}

dlmpg123::~dlmpg123()
{
    mpg123_delete(m);
    mpg123_exit();
}

int dlmpg123::attach(const char *f)
{
    /* attach the input m2v file */
    filename = f;

    /* open the input file */
    file = fopen(filename, "rb");
    if (!file)
        dlerror("error: failed to open input file \"%s\"", filename);

    /* allocate the read buffer */
    size = 184;
    data = (unsigned char *) malloc(size);

    /* skip psi parsing if audio pid specified */
    if (pid==0) {
        int stream_types[] = { 0x03, 0x04 };
        pid = find_pid_for_stream_type(stream_types, sizeof(stream_types)/sizeof(int), filename, file);
    }

    if (pid==0)
        /* no mpeg audio pid found */
        return -1;
    dlmessage("mpeg audio pid is %d", pid);

    /* find the first pes with a pts */
    do {
        int read = next_pes_packet_data(data, &pts, pid, 1, file);
        if (read==0) {
            if (feof(file) || ferror(file)) {
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
        int read = next_pes_packet_data(data, &pts, pid, 0, file);
        if (read==0) {
            if (feof(file) || ferror(file)) {
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

    return 0;
}

decode_t dlmpg123::decode(unsigned char *samples, size_t sampsize)
{
    decode_t results = {0, -1};

    /* feed the audio decoder */
    do {
        int read = next_pes_packet_data(data, &pts, pid, 0, file);
        if (read==0) {
            if (ferror(file)) {
                dlmessage("error reading file \"%s\": %s", filename, strerror(errno));
                pid = 0;
                results.size = 0;
                return results;
            }
            if (feof(file)) {
                off_t offset = 0;
                ret = mpg123_feedseek(m, 0, SEEK_SET, &offset);
                if (ret != MPG123_OK)
                    dlerror("failed to seek in audio stream: %d", mpg123_strerror(m));
                if (fseek(file, offset, SEEK_SET)<0)
                    dlerror("failed to seek in file \"%s\"", filename);
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
    last_pts = -1;
}

dlliba52::~dlliba52()
{
    if (a52_state)
        a52_free(a52_state);
}

int dlliba52::attach(const char *f)
{
    /* attach the input m2v file */
    filename = f;

    /* open the input file */
    file = fopen(filename, "rb");
    if (!file)
        dlerror("error: failed to open input file \"%s\"", filename);

    /* allocate the read buffer */
    size = 184;
    data = (unsigned char *) malloc(size);

    /* skip psi parsing if audio pid specified */
    if (pid==0) {
        int stream_types[] = { 0x81 };
        pid = find_pid_for_stream_type(stream_types, sizeof(stream_types)/sizeof(int), filename, file);
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
        read = next_pes_packet_data(data, &pts, pid, 1, file);
        if (read==0) {
            if (feof(file) || ferror(file)) {
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
            read = next_pes_packet_data(data, &pts, pid, 0, file);
            if (read==0) {
                if (ferror(file)) {
                    dlmessage("error reading file \"%s\": %s", filename, strerror(errno));
                    pid = 0;
                    results.size = 0;
                    return results;
                }
                if (feof(file)) {
                    off_t offset = 0;
                    if (fseek(file, offset, SEEK_SET)<0)
                        dlerror("failed to seek in file \"%s\"", filename);
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
            read = next_pes_packet_data(data, &pts, pid, 0, file);
            if (read==0) {
                if (ferror(file)) {
                    dlmessage("error reading file \"%s\": %s", filename, strerror(errno));
                    pid = 0;
                    break;
                }
                if (feof(file)) {
                    off_t offset = 0;
                    if (fseek(file, offset, SEEK_SET)<0)
                        dlerror("failed to seek in file \"%s\"", filename);
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
}

dlhevc::~dlhevc()
{
    if (ctx)
        de265_free_decoder(ctx);
}

int dlhevc::attach(const char *f)
{
    /* attach the input hevc file */
    filename = f;

    /* open the input file */
    file = fopen(filename, "rb");
    if (!file)
        dlerror("error: failed to open input file \"%s\"", filename);

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
            int n = fread(data, 1, size, file);
            if (n) {
                err = de265_push_data(ctx, data, n, 0, NULL);
                if (!de265_isOK(err)) {
                    dlerror("failed to push hevc data to decoder: %s", de265_get_error_text(err));
                }
            }

            if (feof(file)) {
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
        framerate = 60; // FIXME not sure how to extract this.
        switch (de265_get_chroma_format(image)) {
          //case de265_chroma_444  : pixelformat = I444; break;
            case de265_chroma_422  : pixelformat = I422; break;
            case de265_chroma_420  : pixelformat = I420; break;
          //case de265_chroma_mono : pixelformat = Y800; break;
            default : dlerror("unknown chroma format");
        }
    }

    return 0;
}

decode_t dlhevc::decode(unsigned char *uyvy, size_t uyvysize)
{
    decode_t results = {0, 0};

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
            int read = fread(data, 1, size, file);
            if (read==0 || feof(file)) {
                if (fseek(file, 0, SEEK_SET)<0)
                    dlerror("failed to seek in file \"%s\"", filename);
                read = fread(data, 1, size, file);
            }
            if (read) {
                err = de265_push_data(ctx, data, read, 0, NULL);
                if (!de265_isOK(err)) {
                    dlerror("failed to push hevc data to decoder: %s", de265_get_error_text(err));
                }
            }
        } else if (!more) {
            /* decoding finished */
            if (!de265_isOK(err))
                dlmessage("error decoding frame: %s", de265_get_error_text(err));
            break;
        }
    }

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
        results.timestamp = timestamp;
        timestamp += lround(90000.0/framerate);
        return results;
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

