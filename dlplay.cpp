/*
 * Description: play raw video files.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2010,2011 4i2i Communications Ltd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <semaphore.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <inttypes.h>
#include <errno.h>

extern "C" {
    #include <mpeg2dec/mpeg2.h>
    #include <mpeg2dec/mpeg2convert.h>
    #include <a52dec/a52.h>
    #include <a52dec/mm_accel.h>
#ifdef HAVE_LIBDE265
    #include <libde265/de265.h>
#endif
}
#include <mpg123.h>

#include "DeckLinkAPI.h"

#include "dlutil.h"
#include "dlterm.h"
#include "dldecode.h"
#include "dlalloc.h"
#include "dlts.h"

/* compile options */
#define USE_TERMIOS
//#define USE_MMAP

const char *appname = "dlplay";

/* semaphores for synchronisation */
sem_t sem;
int exit_thread;

/* display statistics */
const int PREROLL_FRAMES = 60;
bool preroll;
unsigned int completed;
unsigned int late, dropped, flushed;
bool pause_mode = 0;

typedef struct TimeCode_ {
    int ff;
    int ss;
    int mm;
    int hh;
} TimeCode;

class callback : public IDeckLinkVideoOutputCallback, public IDeckLinkAudioOutputCallback
{
public:
    callback(IDeckLinkOutput *output);

protected:

public:

    /* implementation of IDeckLinkVideoOutputCallback */
    /* IUnknown needs only a dummy implementation */
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) {return E_NOINTERFACE;}
    virtual ULONG STDMETHODCALLTYPE   AddRef()  {return 1;}
    virtual ULONG STDMETHODCALLTYPE   Release() {return 1;}

    virtual HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame* frame, BMDOutputFrameCompletionResult result);
    virtual HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped();
    virtual HRESULT STDMETHODCALLTYPE RenderAudioSamples(bool preroll);
};

callback::callback(IDeckLinkOutput *output)
{
    if (output->SetScheduledFrameCompletionCallback(this)!=S_OK)
        dlexit("%s: error: could not set video callback object");
    //if (output->SetAudioCallback(this)!=S_OK)
    //    dlexit("%s: error: could not set audio callback object");
}

#if 0
bool test()
{
    /* allocate a frame */
    IDeckLinkMutableVideoFrame *frame = newframe();

    /* draw colour bars */
    const unsigned int bars[8] = {0xEA80EA80, 0xD292D210, 0xA910A9A5, 0x90229035, 0x6ADD6ACA, 0x51EF515A, 0x286D28EF, 0x10801080};
    unsigned int *data;
    frame->GetBytes((void**)&data);
    for (int y=0; y<height; y++) {
        for (int x=0; x<width; x+=2) {
            //*(data++) = 0x10801080;
            *(data++) = bars[(x * 8) / width];
        }
    }

    /* begin video preroll by scheduling a second of frames in hardware */
    int frameno = 0;
    for (; frameno<framerate; frameno++)
        enqueue(frame);

    return true;
}
#endif

/************************* DeckLink API Delegate Methods *****************************/

HRESULT callback::ScheduledFrameCompleted(IDeckLinkVideoFrame* frame, BMDOutputFrameCompletionResult result)
{
    switch (result) {
        case bmdOutputFrameDisplayedLate: late++; break;
        case bmdOutputFrameDropped: dropped++; break;
        case bmdOutputFrameFlushed: flushed++; break;
    }

    /* when a video frame has been completed, post a semaphore */
    if (result!=bmdOutputFrameFlushed) {
        sem_post(&sem);
        completed++;
    }

    return S_OK;
}

HRESULT callback::ScheduledPlaybackHasStopped ()
{
    return S_OK;
}

HRESULT callback::RenderAudioSamples(bool preroll)
{
    return S_OK;
}

static void set_timecode(IDeckLinkMutableVideoFrame *frame, BMDTimeScale framerate_scale, BMDTimeValue framerate_duration, bool progressive, TimeCode *timecode, bool reset)
{
    const int framerate = (int)((framerate_scale + (framerate_duration - 1)) / framerate_duration);
    const bool fractional = framerate_duration % 10 ? true : false;

    BMDTimecodeFlags timeCodeFlag = 0;
    if (reset) {
        timecode->ff = timecode->ss = timecode->mm = timecode->hh = 0;
        timeCodeFlag = bmdTimecodeFlagDefault;
    } else {
        if (timecode->ff >= framerate - 1) {
            timecode->ff = 0;
            timecode->ss++;
        }
        else
            timecode->ff++;

        if (timecode->ss >= 60) {
            timecode->ss = 0;
            timecode->mm++;
        }

        if (timecode->mm >= 60) {
            timecode->mm = 0;
            timecode->hh++;
        }

        if (timecode->hh >= 24)
            timecode->hh = 0;

        timeCodeFlag = bmdTimecodeFlagDefault;
        if (!(framerate % 30) && fractional && !timecode->ff && !timecode->ss && timecode->mm % 10) {
            timeCodeFlag = bmdTimecodeIsDropFrame;
            timecode->ff = framerate > 30 ? 4 : 2;
        }
    }
    int ff = framerate > 30 ? timecode->ff >> 1 : timecode->ff;
    bool oddflag = timecode->ff & 1;

    /* set the timecode in the frame */
    bool setVITC1Timecode = false;
    bool setVITC2Timecode = false;

    if (!progressive) {
        // An interlaced or PsF frame has both VITC1 and VITC2 set with the same timecode value (SMPTE ST 12-2:2014 7.2)
        setVITC1Timecode = true;
        setVITC2Timecode = true;
    }
    else if (framerate_scale / framerate_duration <= 30) {
        // If this isn't a High-P mode, then just use VITC1 (SMPTE ST 12-2:2014 7.2)
        setVITC1Timecode = true;
    }
    else {
        // If this is a High-P mode then use VITC1 on even frames and VITC2 on odd frames. This is done because the
        // frames field of the RP188 VITC timecode cannot hold values greater than 30 (SMPTE ST 12-2:2014 7.2, 9.2)
        if (oddflag)
            setVITC2Timecode = true;
        else
            setVITC1Timecode = true;
    }

    //printf("frame %d, timecode[%.2d:%.2d:%.2d.%.2d]%s\n", timecode->ff, timecode->hh, timecode->mm, timecode->ss, ff,
    //       timeCodeFlag & bmdTimecodeIsDropFrame? " (drop)" : "");
    if (setVITC1Timecode)
        frame->SetTimecodeFromComponents(bmdTimecodeRP188VITC1, timecode->hh, timecode->mm, timecode->ss, ff, timeCodeFlag);

    if (setVITC2Timecode)
        frame->SetTimecodeFromComponents(bmdTimecodeRP188VITC2, timecode->hh, timecode->mm, timecode->ss, ff, timeCodeFlag | bmdTimecodeFieldMark);

    return;
};

/*****************************************/

void usage(int exitcode)
{
    fprintf(stderr, "%s: play raw video files\n", appname);
    fprintf(stderr, "usage: %s [options] <file/url>\n", appname);
    fprintf(stderr, "  -s, --sizeformat    : specify display size format: 480i,480p,576i,720p,1080i,1080p [optional +framerate] (default: autodetect)\n");
    fprintf(stderr, "  -f, --fourcc        : specify pixel fourcc format: i420,i422,uyvy (default: i420)\n");
    fprintf(stderr, "  -I, --interface     : address of interface to listen for multicast data (default: first network interface)\n");
    fprintf(stderr, "  -r, --resettime     : reset timecode to zero when input yuv file wraps around (default: off)\n");
    fprintf(stderr, "  -a, --firstframe    : index of first frame in input to display (default: 0)\n");
    fprintf(stderr, "  -n, --numframes     : total number of frames to display (default: no limit)\n");
    fprintf(stderr, "  -2, --halfrate      : allow using half frame rate, e.g. 30fps when 60fps is not supported (default: off)\n");
    fprintf(stderr, "  -l, --luma          : display luma plane only (default: luma and chroma)\n");
    fprintf(stderr, "  -=, --videoonly     : play video only (default: video and audio if possible)\n");
    fprintf(stderr, "  -~, --audiooonly    : play audio only (default: video and audio if possible)\n");
    fprintf(stderr, "  -p, --video-pid     : decode specific video pid from transport stream (default: first program)\n");
    fprintf(stderr, "  -o, --audio-pid     : decode specific audio pid from transport stream (default: first program)\n");
    fprintf(stderr, "  -i, --index         : index of decklink card to use (default: 0)\n");
    fprintf(stderr, "  -q, --quiet         : decrease verbosity, can be used multiple times\n");
    fprintf(stderr, "  -v, --verbose       : increase verbosity, can be used multiple times\n");
    fprintf(stderr, "  --                  : disable argument processing\n");
    fprintf(stderr, "  -h, --help          : print this usage message\n");
    exit(exitcode);
}

void *display_status(void *arg)
{
    IDeckLinkOutput *output = (IDeckLinkOutput *)arg;

    sleep(1);

    unsigned int framerate = completed;
    do {
        if (!pause_mode && !preroll) {
            char string[256];
            int len = 0;

            /* get buffer depth */
            uint32_t video_buffer, audio_buffer = 0;
            output->GetBufferedVideoFrameCount(&video_buffer);
            output->GetBufferedAudioSampleFrameCount(&audio_buffer);

            /* get scheduled time */
            BMDTimeValue time;
            double speed;
            output->GetScheduledStreamTime(180000, &time, &speed);

            /* display status output */
            len += snprintf(string+len, sizeof(string)-len, "%dfps video buffer %d audio buffer %d time %s speed %.1f", completed-framerate, video_buffer, audio_buffer, describe_sts(time), speed);
            framerate = completed;
            if (late)
                len += snprintf(string+len, sizeof(string)-len, " late %d frame%s", late, late>1? "s" : "");
            if (dropped)
                len += snprintf(string+len, sizeof(string)-len, " dropped %d frame%s", dropped, dropped>1? "s" : "");
            dlstatus("performance: %s", string);
        }

        /* wait for stats to accumulate */
        sleep(1);
    } while (!exit_thread);

    pthread_exit(0);
}

int main(int argc, char *argv[])
{
    char *filename = NULL;
    filetype_t filetype = OTHER;

    /* picture size variables */
    int pic_width = 0;
    int pic_height = 0;
    int dis_width = 0;
    int dis_height = 0;
    bool interlaced = 0;
    float framerate = 0.0;
    bool halfframerate = false;
    pixelformat_t pixelformat = I420;

    /* command line defaults */
    char *sizeformat = NULL;
    char *fourcc = NULL;
    const char *interface = NULL;
    int firstframe = 0;
    unsigned numframes = 0;
    bool allowhalfrate = false; /* use e.g. 30fps when 60fps not supported */
    int lumaonly = 0;
    int topfieldfirst = 1;
    int videoonly = 0;
    int audioonly = 0;
    int index = 0;
    int verbose = 0;
    bool resettime = false;

    /* decoders */
    class dldecode *video = NULL;
    class dldecode *audio = NULL;

    /* decoded audio buffer */
    size_t aud_size = 0;
    unsigned char *aud_data = NULL;
    uint32_t aud_rem = 0;              /* audio samples remaining to enqueue */

    /* transport stream variables */
    int vid_pid = 0;
    int aud_pid = 0;

    /* status thread variables */
    pthread_t status_thread;

    /* custom memory allocator */
    class dlalloc alloc;

    /* parse command line for options */
    while (1) {
        static struct option long_options[] = {
            {"sizeformat",1, NULL, 's'},
            {"format",    1, NULL, 'f'},
            {"ts",        0, NULL, 't'},
            {"transportstream", 0, NULL, 't'},
            {"interface", 1, NULL, 'I'},
            {"resettime", 0, NULL, 'r'},
            {"firstframe",1, NULL, 'a'},
            {"numframes", 1, NULL, 'n'},
            {"halfrate",  0, NULL, '2'},
            {"halfframerate",  0, NULL, '2'},
            {"luma",      0, NULL, 'l'},
            {"videoonly", 0, NULL, '='},
            {"noaudio",   0, NULL, '='},
            {"audioonly", 0, NULL, '~'},
            {"novideo",   0, NULL, '~'},
            {"video-pid", 1, NULL, 'p'},
            {"audio-pid", 1, NULL, 'o'},
            {"index",     1, NULL, 'i'},
            {"quiet",     0, NULL, 'q'},
            {"verbose",   0, NULL, 'v'},
            {"help",      0, NULL, 'h'},
            {NULL,        0, NULL,  0 }
        };

        int optchar = getopt_long(argc, argv, "s:f:tI:ra:n:2l=~p:o:i:qvh", long_options, NULL);
        if (optchar==-1)
            break;

        switch (optchar) {
            case 's':
                sizeformat = optarg;
                break;

            case 'f':
                fourcc = optarg;
                break;

            case 't':
                /* left for command line compatibility, but not required */
                break;

            case 'I':
                interface = optarg;
                dlmessage("interface=%s", interface);
                break;

            case 'r':
                resettime = true;
                break;

            case 'a':
                firstframe = atoi(optarg);
                if (firstframe<0)
                    dlexit("invalid value for index of first frame: %d", firstframe);
                break;

            case 'n':
                numframes = atoi(optarg);
                if (numframes<1)
                    dlexit("invalid value for number of frames: %d", numframes);
                break;

            case '2':
                allowhalfrate = true;
                break;

            case 'l':
                lumaonly = 1;
                break;

            case '=':
                videoonly = 1;
                break;

            case '~':
                audioonly = 1;
                break;

            case 'p':
                vid_pid = atoi(optarg);
                if (vid_pid<=1 || vid_pid>8191)
                    dlexit("invalid value for video pid: %d", vid_pid);
                break;

            case 'o':
                aud_pid = atoi(optarg);
                if (aud_pid<=1 || aud_pid>8191)
                    dlexit("invalid value for audio pid: %d", aud_pid);
                break;

            case 'i':
                index = atoi(optarg);
                if (index<0)
                    dlexit("invalid value for card index: %d", index);
                break;

            case 'q':
                verbose--;
                break;

            case 'v':
                verbose++;
                break;

            case 'h':
                usage(0);
                break;

            case '?':
                exit(1);
                break;
        }
    }

    /* all non-options are input filenames or urls */
    while (optind<argc) {
        if (filename==NULL)
            filename = argv[optind++];
        else {
            dlexit("only one input file supported: %s", argv[optind++]);
        }
    }

    /* sanity check the command line */
    if (!filename)
        usage(1);

    /* initialise the DeckLink API */
    IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
    if (iterator==NULL)
        dlexit("error: could not initialise, the DeckLink driver may not be installed");

    /* connect to the first card in the system */
    IDeckLink *card;
    int i;
    for (i=0; i<=index; i++)
        if (iterator->Next(&card)!=S_OK)
            dlexit("error: failed to find DeckLink card with index %d", i);

    /* obtain the audio/video output interface */
    void *voidptr;
    if (card->QueryInterface(IID_IDeckLinkOutput, &voidptr)!=S_OK)
        dlexit("%s: error: could not obtain the video output interface");
    IDeckLinkOutput *output = (IDeckLinkOutput *)voidptr;

    /* create callback object */
    class callback the_callback(output);

    /* assign the custom memory allocator */
    if (output->SetVideoOutputFrameMemoryAllocator(&alloc)!=S_OK)
        dlexit("%s: error: could not set a custom memory allocator");

    /* configure the decklink card to output on SDI single-link only
     * and smpte level A for 3G, and PsF set to off,
     * this is a necessary precaution as it appears that these configurations
     * essentially have random default values on 4K Extreme 12G cards */
    if (card->QueryInterface(IID_IDeckLinkConfiguration, &voidptr)!=S_OK)
        dlexit("error: could not obtain the configuration interface");
    IDeckLinkConfiguration *config = (IDeckLinkConfiguration *)voidptr;
    if (config->SetInt(bmdDeckLinkConfigVideoOutputConnection, bmdVideoConnectionSDI)!=S_OK)
        dlmessage("warning: failed to set card configuration to output SDI");
    if (config->SetInt(bmdDeckLinkConfigSDIOutputLinkConfiguration, bmdLinkConfigurationSingleLink)!=S_OK)
        dlmessage("warning: failed to set card configuration to single link SDI");
    if (config->SetFlag(bmdDeckLinkConfigSMPTELevelAOutput, true)!=S_OK)
        dlmessage("warning: failed to set card configuration to SMPTE A");
    if (config->SetFlag(bmdDeckLinkConfigOutput1080pAsPsF, false))
        dlmessage("warning: failed to set card configuration to not use PsF");

    /* initialise timecode */
    TimeCode timecode = {0};
    bool reset_timecode = true;

    /* play input files sequentially */
    unsigned int restart = 1, exit = 0;
    while (restart && !exit) {

        /* initialise the semaphore */
        sem_init(&sem, 0, 0);

        /* create the input data source */
        dlsource *source = NULL;
        if (strncmp(filename, "udp://", 6)==0) {
            /* determine address, if given */
            char address[32] = {0};
            strncpy(address, filename + 6, sizeof(address)-1);

            /* determine port number, if given */
            const char *port = "1234";
            char *colon = strchr(address, ':');
            if (colon) {
                port = colon+1;
                *colon = '\0'; /* mark end of address */
            }

            /* open network socket */
            if (strtol(address, NULL, 10)>=224 && strtol(address, NULL, 10)<=239)
                /* multicast */
                source = new dlsock(address, interface);
            else
                /* unicast */
                source = new dlsock();
            source->open(port);

            filetype = source->autodetect();
        } else if (strncmp(filename, "tcp://", 6)==0) {
            source = new dltcpsock();
            const char *port = strchr(filename+6, ':');
            if (port)
                source->open(port+1);
            else
                source->open("1234");

            filetype = source->autodetect();
        } else if (strncmp(filename, "file://", 7)==0) {
#ifdef USE_MMAP
            source = new dlmmap();
#else
            source = new dlfile();
#endif
            source->open(filename+7);
            filetype = source->autodetect();
        } else if (strstr(filename, "://")==NULL) {
#ifdef USE_MMAP
            source = new dlmmap();
#else
            source = new dlfile();
#endif
            source->open(filename);
            filetype = source->autodetect();
        } else
            dlexit("could not open url \"%s\"", filename);

        /* create the video and audio format filters and decoders */
        dlformat *vid_fmt = NULL, *aud_fmt = NULL;
        switch (filetype) {
            case TS :
            {
                /* look for a video pid */
                int stream_type = 0;
                if (!audioonly) {
                    int video_stream_types[] = { 0x02, 0x80, 0x1B, 0x24 };
                    vid_pid = find_pid_for_stream_type(video_stream_types, sizeof(video_stream_types)/sizeof(int), &stream_type, source, 0);
                    source->rewind();
                }

                if (vid_pid) {
                    /* create a format filter for transport stream */
                    dltstream *ts = new dltstream(vid_pid);
                    ts->attach(source);

                    /* create a video decoder */
                    switch (stream_type) {
                        case 0x02:
                        case 0x80:
//#ifdef HAVE_FFMPEG
//                            video = new dlffvideo(AV_CODEC_ID_MPEG2VIDEO);
//#else
                            video = new dlmpeg2;
//#endif
                            break;

                        case 0x24:
#ifdef HAVE_FFMPEG
                            video = new dlffvideo(AV_CODEC_ID_HEVC);
#elif HAVE_LIBDE265
                            video = new dlhevc;
#else
                            dlexit("error: no support for hevc decoder in this build");
#endif
                            break;

                        case 0x1b:
#ifdef HAVE_FFMPEG
                            video = new dlffvideo(AV_CODEC_ID_H264);
#else
                            dlexit("error: no support for ffmpeg decoder in this build");
#endif
                            break;
                    }

                    /* cast down to format pointer */
                    vid_fmt = (dlformat *)ts;
                } else
                    audioonly = 1;

                /* look for an audio pid */
                if (!videoonly) {
                    int audio_stream_types[] = { 0x03, 0x04, 0x81, 0x1C, 0x06 };
                    //int audio_stream_types[] = { 0x03, 0x04 };
                    aud_pid = find_pid_for_stream_type(audio_stream_types, sizeof(audio_stream_types)/sizeof(int), &stream_type, source, 0);
                    source->rewind();
                }

                if (aud_pid) {
                    /* create a format filter for transport stream */
                    dltstream *ts = new dltstream(aud_pid);
                    ts->attach(source);

                    /* create an audio decoder */
                    switch (stream_type) {
                        case 0x03:
                        case 0x04:
                            audio = new dlmpg123;
                            aud_size = 32768;
                            break;

                        case 0x1c:
                        case 0x81:
                            audio = new dlliba52;
                            aud_size = 6*256*2*sizeof(uint16_t);
                            break;

                        case 0x06:
                            /* assume this is smpte302m */
                            audio = new dlpcm;
                            aud_size = 32768; //192*2*2*sizeof(uint16_t);
                            break;

                        default:
                            dlmessage("unknown audio stream type: %d", stream_type);
                            aud_size = 0;
                            break;
                    }

                    /* cast down to format pointer */
                    aud_fmt = (dlformat *)ts;
                } else
                    videoonly = 1;

                if (verbose>=0)
                    dlmessage("video pid is %d and audio pid is %d", vid_pid, aud_pid);

                break;
            }

            case YUV:
            {
                vid_fmt = new dlformat;
                vid_fmt->attach(source);
                dlyuv *yuv = new dlyuv();

                /* yuv specific options */
                if (lumaonly)
                    yuv->set_lumaonly(lumaonly);
                if (sizeformat)
                    yuv->set_imagesize(sizeformat);
                if (fourcc)
                    yuv->set_fourcc(fourcc);

                /* cast down to decoder pointer */
                video = (dldecode *)yuv;
                videoonly = 1;
                break;
            }

            case M2V :
                vid_fmt = new dlestream;
                vid_fmt->attach(source);
                video = new dlmpeg2;
                // TODO does this need dims in advance.
                //video = new dlffvideo(AV_CODEC_ID_MPEG2VIDEO);
                videoonly = 1;
                break;

            case M4V:
#ifdef HAVE_FFMPEG
                vid_fmt = new dlestream;
                vid_fmt->attach(source);
                video = new dlffvideo(AV_CODEC_ID_MPEG4);
                videoonly = 1;
#else
                dlexit("error: no support for mpeg-4 decoder in this build");
#endif
                break;

            case AVC:
#ifdef HAVE_FFMPEG
                vid_fmt = new dlestream;
                vid_fmt->attach(source);
                video = new dlffvideo(AV_CODEC_ID_H264);
                videoonly = 1;
#else
                dlexit("error: no support for avc decoder in this build");
#endif
                break;

            case HEVC:
#ifdef HAVE_FFMPEG
                vid_fmt = new dlestream;
                vid_fmt->attach(source);
                video = new dlffvideo(AV_CODEC_ID_H265);
                videoonly = 1;
#elif HAVE_LIBDE265
                vid_fmt = new dlestream;
                vid_fmt->attach(source);
                video = new dlhevc;
                videoonly = 1;
#else
                dlexit("error: no support for hevc decoder in this build");
#endif
                break;

            case AV1:
#ifdef HAVE_FFMPEG
                vid_fmt = new dlestream;
                vid_fmt->attach(source);
                video = new dlffvideo(AV_CODEC_ID_AV1);
                videoonly = 1;
#else
                dlexit("error: no support for hevc decoder in this build");
#endif
                break;

            case FFMPEG:
#ifdef HAVE_FFMPEG
                vid_fmt = new dlavformat;
                if (vid_fmt->attach(source)<0)
                    dlexit("failed to attach the ffmpeg format decoder to the source");
                video = new dlffmpeg;
                videoonly = 1;
#else
                dlexit("error: no support for ffmpeg decoder in this build");
#endif
                break;

            default: dlexit("unknown input file type: %s", describe_filetype(filetype));
        }

        /* initialise the video decoder */
        if (!audioonly) {
            /* set the verbosity */
            video->set_verbose(verbose);

            /* figure out the mode to set */
            if (video->attach(vid_fmt)<0)
                dlexit("failed to initialise the video decoder");
            pic_width = video->width;
            pic_height = video->height;
            interlaced = video->interlaced;
            framerate = video->framerate;
            pixelformat = video->pixelformat;
        }

        /* initialise the audio encoder */
        if (!videoonly && aud_pid) {
            /* set the verbosity */
            audio->set_verbose(verbose);

            if (audio->attach(aud_fmt)<0) {
                dlmessage("failed to initialise the audio decoder");
                delete audio;
                audio = NULL;
            }
            /* note there may not be an audio stream in the file
             * in which case the audio decoder will be null */
            if (audio)
                aud_data = (unsigned char *) malloc(aud_size);
        }

        /* sanity check */
        if (!video && !audio)
            dlexit("error: neither video nor audio to play in file \"%s\"", filename);

        dlmessage("info: found %s video and %s audio in %s from %s source", video? video->description() : "no video", audio? audio->description() : "no audio", vid_fmt->description(), source->description());

        if (video && verbose>=1)
            dlmessage("info: video format is %dx%d%c%.2f %s", pic_width, pic_height, interlaced? 'i' : 'p', framerate, pixelformatname[pixelformat]);

        /* determine display dimensions */
        if (sizeformat) {
            /* lookup specified format */
            if (divine_video_format(sizeformat, &dis_width, &dis_height, &interlaced, &framerate)<0)
                dlexit("failed to determine output video format from filename: %s", sizeformat);
        } else {
            /* determine from picture parameters */
            dis_width = pic_width;
            dis_height = pic_height;
            switch (pic_width) {
                case 704 : dis_width = 720; break;
                case 1440: dis_width = 1920; break;
            }
            switch (pic_height) {
                case 480 : dis_height = 486; break;
                case 744 : dis_height = 720; break; /* work around a bug in libde265 in 4:2:2 mode */
                case 1088: dis_height = 1080; break;
                case 1116: dis_height = 1080; break; /* work around a bug in libde265 in 4:2:2 mode */
            }
            if (pic_width<704 && pic_height<480) {
                if (framerate<29.0) {
                    /* display pal */
                    dis_width = 720;
                    dis_height = 576;
                } else {
                    /* display ntsc */
                    dis_width = 720;
                    dis_height = 486;
                }
            }
        }

        /* get display mode iterator */
        IDeckLinkDisplayMode *mode;
        BMDTimeValue framerate_duration;
        BMDTimeScale framerate_scale;
        if (video) {
            IDeckLinkDisplayModeIterator *iterator;
            if (output->GetDisplayModeIterator(&iterator) != S_OK)
                dlerror("failed to get display mode iterator");

            /* find mode for given width and height */
            while (iterator->Next(&mode) == S_OK) {
                mode->GetFrameRate(&framerate_duration, &framerate_scale);
                if (verbose>=2) {
                    const char *name = NULL;
                    mode->GetName(&name);
                    //bool interlaced = mode->GetFieldDominance()!=bmdProgressiveFrame;
                    //dlmessage("mode available: %ldx%ld%c%.2f (%s)", mode->GetWidth(), mode->GetHeight(), interlaced? 'i' : 'p', (double)framerate_scale/framerate_duration*(interlaced?2.0:1.0), name);
                    dlmessage("mode available: %s", name);
                    if (name)
                        delete name;
                }
                if (mode->GetWidth()==dis_width && mode->GetHeight()==dis_height) {
                    if ((mode->GetFieldDominance()==bmdProgressiveFrame) ^ interlaced) {
                        mode->GetFrameRate(&framerate_duration, &framerate_scale);
                        /* look for an integer frame rate match */
                        if ((framerate_scale / framerate_duration)==(int)floor(framerate))
                            break;
                        if (allowhalfrate) {
                            /* also look for a half-rate match, to support 1080p60 in a hacky way */
                            if ((framerate_scale / framerate_duration)==(int)floor(framerate/2)) {
                                halfframerate = true;
                                framerate /= 2;
                                video->framerate /= 2;
                                break;
                            }
                        }
                    }
                }
            }
            iterator->Release();

            if (mode==NULL)
                dlexit("error: failed to find mode for %dx%d%c%.2f", pic_width, pic_height, interlaced? 'i' : 'p', framerate);

            /* display mode name */
            const char *name = NULL;
            if (mode->GetName(&name)==S_OK)
                dlmessage("info: video mode %s %s", name, halfframerate?"(half rate)":"");
            if (name)
                delete name;
        }

        /* vanc timecode enabled */
        BMDVideoOutputFlags videoOutputFlags = bmdVideoOutputFlagDefault | bmdVideoOutputRP188;

        /* set the video output mode */
        if (video) {
            HRESULT result = output->EnableVideoOutput(mode->GetDisplayMode(), videoOutputFlags);
            if (result!=S_OK)
                dlapierror(result, "failed to enable video output");
        }

        /* set the audio output mode */
        if (audio) {
            HRESULT result = output->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, 2, bmdAudioOutputStreamTimestamped);
            if (result != S_OK) {
                switch (result) {
                    case E_ACCESSDENIED : fprintf(stderr, "%s: error: access denied when enabling audio output\n", appname); break;
                    case E_OUTOFMEMORY  : fprintf(stderr, "%s: error: out of memory when enabling audio output\n", appname); break;
                    case E_INVALIDARG   : fprintf(stderr, "%s: error: invalid number of channels when enabling audio output\n", appname); break;
                    default             : fprintf(stderr, "%s: error: failed to enable audio output\n", appname); break;
                }
                return 2;
            }

            /* being audio preroll */
            if (output->BeginAudioPreroll()!=S_OK) {
                dlmessage("error: failed to begin audio preroll");
                return 2;
            }
        }

        /* preroll as many video frames as possible */
        preroll = 1;
        IDeckLinkMutableVideoFrame *frame = NULL;
        decode_t vid, aud;
        vid.timestamp = aud.timestamp = 0;      /* fixes warning */

        /* playback timestamp boundaries */
        sts_t video_start_time = 1ll<<35;
        sts_t video_end_time = 0ll;
        sts_t audio_start_time = 1ll<<35;
        sts_t audio_end_time = 0ll;

        /* timestamp sanity checking */
        sts_t last_vid = 0;
        sts_t last_aud = 0;

        /* video frame history buffer */
        int num_history_frames = 0;
        const int MAX_HISTORY_FRAMES = 30;
        typedef struct {
            IDeckLinkVideoFrame *frame;
            sts_t timestamp;
        } history_frame_t;
        history_frame_t history_buffer[MAX_HISTORY_FRAMES];

        /* start the status thread */
        exit_thread = 0;
        if (pthread_create(&status_thread, NULL, display_status, output)<0)
            dlerror("failed to create status thread");

        dlmessage("press q to exit, p to pause, s to swap fields...");

        /* initialise terminal for user input */
#ifdef USE_TERMIOS
        class dlterm term;
#endif

        /* set a timeout to catch encoder restarts */
        source->set_timeout(500000); // timeout of 0.5s

        /* main loop */
        int queuenum = 0;
        int framenum = 0;
        int blocknum = 0;
        unsigned long long queuetime = 0;
        unsigned long long decodetime = 0;
        while (true) {

            /* check for user input */
#ifdef USE_TERMIOS
            if (term.kbhit()) {
                int c = term.readchar();

                /* pause */
                if (c=='p' && video) {
                    /* enter pause mode */
                    pause_mode = 1;

                    /* find the index in the history buffer of the current frame */
                    BMDTimeValue time;
                    double speed;
                    output->GetScheduledStreamTime(180000, &time, &speed);
                    int pause_index = 0;
                    if (time < history_buffer[0].timestamp)
                        pause_index = 0;
                    else if (time >= history_buffer[num_history_frames-1].timestamp)
                        pause_index = num_history_frames-1;
                    else
                        for (int i=0; i<num_history_frames-1; i++) {
                            if (history_buffer[i].timestamp <= time && time < history_buffer[i+1].timestamp) {
                                pause_index = i+1;
                                break;
                            }
                        }

                    if (verbose>=1)
                        dlmessage("current time is %s, pause timestamp %s", describe_sts(time), describe_sts(history_buffer[pause_index].timestamp));

                    /* stop the playback */
                    if (output->StopScheduledPlayback(0, NULL, 0) != S_OK)
                        dlexit("%s: error: failed to pause video playback");
                    if (audio)
                        output->DisableAudioOutput();

                    /* display the current frame in the history buffer */
                    output->DisplayVideoFrameSync(history_buffer[pause_index].frame);
                    do {
                        /* blocking terminal read */
                        c = term.readchar();

                        if (c=='j')
                            if (pause_index>0)
                                output->DisplayVideoFrameSync(history_buffer[--pause_index].frame);

                        if (c=='k')
                            if (pause_index<num_history_frames-1)
                                output->DisplayVideoFrameSync(history_buffer[++pause_index].frame);

                    } while (c!='p' && c!='q');

                    /* preroll from end of history buffer to current pause index */
                    for (int i=num_history_frames-1; i>pause_index; i--)
                        if (output->ScheduleVideoFrame(history_buffer[i].frame, history_buffer[i].timestamp, lround(180000.0/framerate), 180000)==S_OK)
                            video_start_time = history_buffer[i].timestamp;
                        else
                            break;

                    /* resume the audio output */
                    if (audio) {
                        HRESULT result = output->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, 2, bmdAudioOutputStreamTimestamped);
                        if (result != S_OK) {
                            dlmessage("error: failed to resume audio output\n");
                        }
                    }

                    /* the semaphone has to be re-initialised */
                    sem_destroy(&sem);
                    sem_init(&sem, 0, 0);

                    /* resume scheduled playback */
                    preroll = 1;
                    queuenum = 0;

                    /* exit pause */
                    pause_mode = 0;
                }

                if (c=='s' && video) {
                    topfieldfirst = !topfieldfirst;
                    video->set_field_order(topfieldfirst);
                    dlmessage("setting deinterlace field order to %s field first", topfieldfirst? "top" : "bottom");
                }

                /* verbose */
                if (c=='v') {
                    verbose++;
                    break;
                }

                /* quit */
                if (c=='q' || c=='\n') {
                    exit = 1;
                    break;
                }
            }
#endif

            /* wait for callback after a frame is finished */
            if (video && !preroll)
                /* use video callback to wait */
                sem_wait(&sem);
            else if (audio && !video)
                /* sleep wait */
                usleep(250000);
            /* else don't wait */

            /* enqueue previous frame */
            if (frame && video) {
                /* set timecode for the current frame */
                set_timecode(frame,
                             framerate_scale,
                             framerate_duration,
                             mode->GetFieldDominance() == bmdProgressiveFrame,
                             &timecode,
                             reset_timecode);
                reset_timecode = false;

                unsigned long long start = get_utime();
                HRESULT result = output->ScheduleVideoFrame(frame, vid.timestamp, lround(180000.0/framerate), 180000);
                queuetime += get_utime() - start;
                if (result != S_OK) {
                    switch (result) {
                        case E_ACCESSDENIED : fprintf(stderr, "%s: error: frame %d: video output disabled when queueing video frame\n", appname, queuenum); break;
                        case E_OUTOFMEMORY  : fprintf(stderr, "%s: error: frame %d: too many frames are scheduled when queueing video frame\n", appname, queuenum);
                        case E_INVALIDARG   : fprintf(stderr, "%s: error: frame %d: frame attributes are invalid when queueing video frame\n", appname, queuenum); break;
                        default             : fprintf(stderr, "%s: error: frame %d: failed to schedule video frame, timestamp %s\n", appname, queuenum, describe_sts(vid.timestamp)); break;
                    }
                    break;
                }
                queuenum++;
            }

            /* end pre-roll after a certain number of frames */
            if (preroll && queuenum>=PREROLL_FRAMES) {
                /* preroll complete */
                preroll = 0;

                /* end audio preroll */
                if (audio && output->EndAudioPreroll()!=S_OK) {
                    dlmessage("error: failed to end audio preroll");
                    return 2;
                }

                /* find best start time from video and audio */
                sts_t start_time = 0;
                /* audio only handled below */
                if (videoonly)
                    start_time = video_start_time;
                //else if (llabs(video_start_time-audio_start_time)>180000)
                //    start_time = mmax(video_start_time, audio_start_time);
                else if (video_start_time==0 && audio_start_time>180000)
                    start_time = audio_start_time;
                else
                    start_time = mmin(video_start_time, audio_start_time);

                /* start the playback */
                if (output->StartScheduledPlayback(start_time, 180000, 1.0) != S_OK)
                    dlexit("error: failed to start video playback");

                if (verbose>=1)
                    dlmessage("info: pre-rolled %d frames", queuenum);
                if (verbose>=1)
                    dlmessage("info: start time of video is %lld, %s", start_time, describe_sts(start_time));
            }

            /* decode the next frame */
            if (video) {
                unsigned long long start = get_utime();

                /* allocate a new frame object */
                HRESULT result = output->CreateVideoFrame(pic_width, pic_height, pic_width*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame);
                if (result!=S_OK)
                    dlapierror(result, "error: failed to create video frame");

                /* extract the frame buffer pointer without type punning */
                result = frame->GetBytes(&voidptr);
                if (result!=S_OK)
                    dlapierror(result, "error: failed to get pointer to data in video frame");
                unsigned char *uyvy = (unsigned char *)voidptr;

                /* read the next frame */
                vid = video->decode(uyvy, frame->GetRowBytes()*frame->GetHeight());
                if (vid.size==0) {
                    dlmessage("error: failed to decode video frame %d in file \"%s\"", framenum, filename);
                    if (source->timeout())
                        restart = 1; /* wait for next sequence */
                    break;
                }
                decodetime += get_utime() - start;
                video_start_time = mmin(vid.timestamp, video_start_time);
                video_end_time = mmax(vid.timestamp, video_end_time);

                /* reset timecode at end of file, only supported with yuv files */
                if (resettime && video->atend())
                    reset_timecode = true;

                if (verbose>=3)
                    dlmessage("info: frame %d timestamp %s, decode %.1fms render %.1fms", framenum, describe_sts(vid.timestamp), vid.decode_time/1000.0, vid.render_time/1000.0);
                framenum++;

                /* store the frame in the history buffer */
                if (num_history_frames<MAX_HISTORY_FRAMES) {
                    history_buffer[num_history_frames].frame = frame;
                    history_buffer[num_history_frames].timestamp = vid.timestamp;
                    num_history_frames++;
                } else {
                    history_buffer[0].frame->Release();
                    memmove(&history_buffer[0], &history_buffer[1], (MAX_HISTORY_FRAMES-1)*sizeof(history_frame_t));
                    history_buffer[MAX_HISTORY_FRAMES-1].frame = frame;
                    history_buffer[MAX_HISTORY_FRAMES-1].timestamp = vid.timestamp;
                }
            }

            /* maintain audio buffer level */
            if (audio /*&& !preroll*/) {

                /* reschedule audio that wasn't queued last time */
                if (aud_rem>0) {
                    uint32_t scheduled;
                    HRESULT result = output->ScheduleAudioSamples(aud_data+aud_size-aud_rem*4, aud_rem, aud.timestamp, 180000, &scheduled); // FIXME timestamp
                    if (result != S_OK) {
                        dlmessage("error: block %d: failed to re-schedule audio data", blocknum);
                        delete audio;
                        audio = NULL;
                    } else if (scheduled!=aud_rem) {
                        dlmessage("failed to re-schedule all the audio data: %d/%d samples", scheduled, aud_rem);
                        aud_rem -= scheduled;
                    } else
                        aud_rem = 0;
                }

                while (aud_rem==0) {
                    unsigned int buffered;
                    if (output->GetBufferedAudioSampleFrameCount(&buffered) != S_OK)
                        dlexit("failed to get audio buffer level");

                    /* maintain audio buffer at >=0.5 secs */
                    if (buffered >= 24000)
                        break;

                    /* decode audio */
                    aud = audio->decode(aud_data, aud_size);
                    if (blocknum==0) {
                        audio_start_time = aud.timestamp;
                        if (verbose>=1)
                            dlmessage("info: start time of audio is %lld, %s", audio_start_time, describe_sts(audio_start_time));
                    }
                    audio_end_time = mmax(aud.timestamp, audio_end_time);

                    /* buffer decoded audio */
                    uint32_t scheduled, num_sample_frames = aud.size/2;
                    HRESULT result = output->ScheduleAudioSamples(aud_data, num_sample_frames, aud.timestamp, 180000, &scheduled);
                    //dlmessage("buffer level %d: decoded %d bytes at timestamp %s and scheduled %d samples", buffered, aud.size, describe_sts(aud.timestamp), scheduled);
                    if (result != S_OK) {
                        dlmessage("error: block %d: failed to schedule audio data", blocknum);
                        delete audio;
                        audio = NULL;
                        break;
                    }
                    if (scheduled!=num_sample_frames) {
                        dlmessage("did not schedule all the audio data: %d/%d samples", scheduled, num_sample_frames);
                        /* exit the loop with a note of how many sample frames remain */
                        aud_rem = num_sample_frames - scheduled;
                    }
                    blocknum++;
                }
            }

            /* start the playback in audio only mode */
            if (audio && !video) {
                /* preroll complete */
                preroll = 0;

                /* end audio preroll */
                if (output->EndAudioPreroll()!=S_OK) {
                    dlmessage("error: failed to end audio preroll");
                    return 2;
                }

                /* start the playback */
                if (output->StartScheduledPlayback(audio_start_time, 180000, 1.0) != S_OK)
                    dlexit("error: failed to start audio playback");

                audio_start_time = aud.timestamp;
                if (verbose>=1) {
                    dlmessage("info: start time of audio is %lld, %s", audio_start_time, describe_sts(audio_start_time));
                }

            }

            /* loop debugging */
            if (1) {
                if (last_vid && last_aud) {
                    sts_t vdiff = vid.timestamp - last_vid;
                    sts_t adiff = aud.timestamp - last_aud;
                    if (vdiff<0  || adiff<0) {
                        char v[32] = {0}, a[32] = {0};
                        strncpy(v, describe_sts(vdiff), sizeof(v)-1);
                        strncpy(a, describe_sts(adiff), sizeof(a)-1);
                        if (verbose>=3)
                            dlmessage("loop: vid=%s aud=%s", v, a);
                        if (vdiff<0)
                            dlmessage("video went back in time: %s", v);
                        if (adiff<0)
                            dlmessage("audio went back in time: %s", a);
                        if (1) {
                            exit = 1;
                            break;
                        }
                    }
                }
                else if (last_vid) {
                    sts_t vdiff = vid.timestamp - last_vid;
                    if (verbose>=3)
                        dlmessage("loop: vid=%s", describe_sts(vdiff));
                    if (vdiff<0) {
                        dlmessage("video went back in time: %s", describe_sts(vdiff));
                        exit = 1;
                        break;
                    }
                }
                last_vid = vid.timestamp;
                last_aud = aud.timestamp;
            }

            /* limit output to specific number of frames */
            if (numframes && completed>=numframes) {
                exit = 1;
                break;
            }
        }

        /* stop the status display */
        exit_thread = 1;

        /* stop the video output */
        output->StopScheduledPlayback(0, NULL, 0);
        output->DisableVideoOutput();
        if (audio)
            output->DisableAudioOutput();

        /* release all frames in the history buffer */
        for (i=0; i<num_history_frames; i++)
            if (history_buffer[i].frame)
                history_buffer[i].frame->Release();

        /* the semaphone has to be re-initialised */
        sem_destroy(&sem);

        /* report timing statistics */
        if (verbose>=1)
            dlmessage("\nmean decode time=%.2fms, mean render time=%.2fms (frame period %.2fms)", (decodetime/framenum)/1000.0, (queuetime/queuenum)/1000.0, 1000.0/framerate);

        /* tidy up */
        delete source;
        delete vid_fmt;
        delete aud_fmt;
        delete video;
        delete audio;
        pthread_join(status_thread, NULL);
    }

    /* tidy up */
    config->Release();
    card->Release();
    iterator->Release();
    if (aud_data)
        free(aud_data);

    /* report statistics */
    if (verbose>=0)
        dlmessage("%d frames: %d late, %d dropped, %d flushed", completed, late, dropped, flushed);

    return 0;
}
