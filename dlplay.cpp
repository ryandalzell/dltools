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
    #include <libde265/de265.h>
}
#include <mpg123.h>

#include "DeckLinkAPI.h"

#include "dlutil.h"
#include "dlterm.h"
#include "dldecode.h"
#include "dlalloc.h"

/* compile options */
#define USE_TERMIOS
#define USE_MMAP

const char *appname = "dlplay";

/* semaphores for synchronisation */
sem_t sem;
int exit_thread;

/* display statistics */
const int PREROLL_FRAMES = 20;
bool preroll;
unsigned int completed;
unsigned int late, dropped, flushed;
bool pause_mode = 0;

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
    sem_post(&sem);
    completed++;

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

/*****************************************/

const char *describe_time(long long t)
{
    static char s[32];

    t /= 90;
    int msec = t % 1000;
    t /= 1000;
    int sec = t % 60;
    t /= 60;
    int min = t % 60;
    t /= 60;
    int hour = t % 60;

    snprintf(s, sizeof(s), "%02d:%02d:%02d.%03d", hour, min, sec, msec);

    return s;
}

void usage(int exitcode)
{
    fprintf(stderr, "%s: play raw video files\n", appname);
    fprintf(stderr, "usage: %s [options] <file/url> [<file/url>...]\n", appname);
    fprintf(stderr, "  -f, --format        : specify display format: 480i,480p,576i,720p,1080i,1080p [optional +framerate] (default: auto)\n");
    fprintf(stderr, "  -t, --ts            : specify network streaming will be transport stream data (default: elementary stream)\n");
    fprintf(stderr, "  -I, --interface     : address of interface to listen for multicast data (default: first network interface)\n");
    fprintf(stderr, "  -a, --firstframe    : index of first frame in input to display (default: 0)\n");
    fprintf(stderr, "  -n, --numframes     : number of frames in input to display (default: all)\n");
    fprintf(stderr, "  -m, --ntscmode      : use proper ntsc framerate, e.g. 29.97 instead of 30fps (default: on)\n");
    fprintf(stderr, "  -l, --luma          : display luma plane only (default: luma and chroma)\n");
    fprintf(stderr, "  -=, --videoonly     : play video only (default: video and audio if possible)\n");
    fprintf(stderr, "  -~, --audiooonly    : play audio only (default: video and audio if possible)\n");
    fprintf(stderr, "  -p, --video-pid     : decode specific video pid from transport stream (default: first program)\n");
    fprintf(stderr, "  -o, --audio-pid     : decode specific audio pid from transport stream (default: first program)\n");
    fprintf(stderr, "  -i, --index         : index of decklink card to use (default: 0)\n");
    fprintf(stderr, "  -q, --quiet         : decrease verbosity, can be used multiple times\n");
    fprintf(stderr, "  -v, --verbose       : increase verbosity, can be used multiple times\n");
    fprintf(stderr, "  --                  : disable argument processing\n");
    fprintf(stderr, "  -u, --help, --usage : print this usage message\n");
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
            uint32_t video_buffer, audio_buffer;
            output->GetBufferedVideoFrameCount(&video_buffer);
            output->GetBufferedAudioSampleFrameCount(&audio_buffer);

            /* get scheduled time */
            BMDTimeValue time;
            double speed;
            output->GetScheduledStreamTime(90000, &time, &speed);

            /* display status output */
            len += snprintf(string+len, sizeof(string)-len, "%dfps video buffer %d audio buffer %d time %s speed %.1f", completed-framerate, video_buffer, audio_buffer, describe_time(time), speed);
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
    pixelformat_t pixelformat;

    /* command line defaults */
    char *displayformat = NULL;
    int ts = 0;
    const char *interface = NULL;
    int firstframe = 0;
    int numframes = -1;
    int ntscmode = 1;   /* use e.g. 29.97 instead of 30fps */
    int lumaonly = 0;
    int topfieldfirst = 1;
    int videoonly = 0;
    int audioonly = 0;
    int index = 0;
    int verbose = 0;

    /* decoders */
    class dldecode *video = NULL;
    class dldecode *audio = NULL;

    /* decoded audio buffer */
    size_t aud_size = 0;
    unsigned char *aud_data = NULL;

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
            {"format",    1, NULL, 'f'},
            {"ts",        0, NULL, 't'},
            {"transportstream", 0, NULL, 't'},
            {"interface", 1, NULL, 'I'},
            {"firstframe",1, NULL, 'a'},
            {"numframes", 1, NULL, 'n'},
            {"ntscmode",  1, NULL, 'm'},
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
            {"usage",     0, NULL, 'u'},
            {"help",      0, NULL, 'u'},
            {NULL,        0, NULL,  0 }
        };

        int optchar = getopt_long(argc, argv, "f:tI:a:n:m:l=~p:o:i:qvu", long_options, NULL);
        if (optchar==-1)
            break;

        switch (optchar) {
            case 'f':
                displayformat = optarg;
                break;

            case 't':
                ts = 1;
                break;

            case 'I':
                interface = optarg;
                dlmessage("interface=%s", interface);
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

            case 'm':
                ntscmode = atoi(optarg);
                if (ntscmode<0 || ntscmode>1)
                    dlexit("invalid value for ntsc mode: %d", ntscmode);
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

            case 'u':
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

    /* play input files sequentially */
    unsigned int restart = 1, exit = 0;
    while (restart && !exit) {

        /* initialise the semaphore */
        sem_init(&sem, 0, 0);

        /* determine the file type */
        if (strstr(filename, ".m2v")!=NULL || strstr(filename, ".M2V")!=NULL)
            filetype = M2V;
        else if (strstr(filename, ".ts")!=NULL || strstr(filename, ".trp")!=NULL || strstr(filename, ".mpg")!=NULL)
            filetype = HEVCTS;
        else if (strstr(filename, ".265")!=NULL || strstr(filename, ".h265")!=NULL || strstr(filename, ".hevc")!=NULL)
            filetype = HEVC;
        else
            filetype = YUV;

        /* create the input data source */
        dlsource *source = NULL;
        if (strncmp(filename, "udp://", 6)==0) {
            /* determine address, if given */
            const char *address = filename + 6;

            /* determine port number, if given */
            const char *port = "1234";
            char *colon = strchr(filename+6, ':');
            if (colon) {
                port = colon+1;
                *colon = '\0'; /* mark end of address */
            }

            /* open network socket */
            if (strtol(address, NULL, 10)>=224 && strtol(address, NULL, 10)<=239) {
                /* multicast */
                source = new dlsock(address, interface);
                source->open(port);
            } else {
                /* unicast */
                source = new dlsock();
                source->open(port);
            }

            /* FIXME need filetype detection or signalling */
            filetype = ts? HEVCTS : HEVC;
            dlmessage("expecting data over udp as hevc %s stream", ts? "transport" : "elementary");
        } else if (strncmp(filename, "tcp://", 6)==0) {
            source = new dltcpsock();
            const char *port = strchr(filename+6, ':');
            if (port)
                source->open(port+1);
            else
                source->open("1234");
            /* FIXME need filetype detection or signalling */
            filetype = ts? HEVCTS : HEVC;
            dlmessage("expecting data over tcp as hevc %s stream", ts? "transport" : "elementary");
        } else if (strncmp(filename, "file://", 7)==0) {
#ifdef USE_MMAP
            source = new dlmmap();
#else
            source = new dlfile();
#endif
            source->open(filename+7);
        } else if (strstr(filename, "://")==NULL) {
#ifdef USE_MMAP
            source = new dlmmap();
#else
            source = new dlfile();
#endif
            source->open(filename);
        } else
            dlexit("could not open url \"%s\"", filename);

        /* create the video decoder */
        if (!audioonly) {
            switch (filetype) {
                case YUV: video = new dlyuv(lumaonly);

                    /* skip to the first frame */
                    if (firstframe)
                        video->rewind(firstframe);

                    break;

                case M2V : video = new dlmpeg2; break;
                case M2VTS: video = new dlmpeg2ts; break;
                case HEVC: video = new dlhevc; break;
                case HEVCTS: video = new dlhevcts; break;
                default: dlexit("unknown input file type");
            }

            /* figure out the mode to set */
            if (video->attach(source)<0)
                dlexit("failed to initialise the video decoder");
            pic_width = video->width;
            pic_height = video->height;
            interlaced = video->interlaced;
            framerate = video->framerate;
            pixelformat = video->pixelformat;

            /* set the verbosity */
            video->set_verbose(verbose);
        }

        /* create the audio encoder */
        switch (filetype) {
            case M2VTS:

                /* try to attach a mpeg1 audio decoder */
                if (audio==NULL && !videoonly) {
                    audio = new dlmpg123;
                    aud_size = 32768;
                    if (audio->attach(source)<0) {
                        delete audio;
                        audio = NULL;
                    }
                }

                /* try to attach an ac3 audio decoder */
                if (audio==NULL && !videoonly) {
                    audio = new dlliba52;
                    aud_size = 6*256*2*sizeof(uint16_t);
                    if (audio->attach(source)<0) {
                        delete audio;
                        audio = NULL;
                    }
                }

                /* note there may not be an audio stream in the file
                 * in which case the audio decoder will be null */
                if (audio)
                    aud_data = (unsigned char *) malloc(aud_size);

                break;

            default: break;
        }

        /* sanity check */
        if (!video && !audio)
            dlexit("error: neither video nor audio to play in file \"%s\"", filename);

        if (video && verbose>=1)
            dlmessage("info: video format is %dx%d%c%.2f %s", pic_width, pic_height, interlaced? 'i' : 'p', framerate, pixelformatname[pixelformat]);

        /* override the framerate with ntscmode */
        if (framerate>29.9 && framerate<=30.0) {
            if (ntscmode)
                framerate = 30000.0/1001.0;
            else
                framerate = 30.0;
        }
        if (framerate>59.9 && framerate<=60.0) {
            if (ntscmode)
                framerate = 60000.0/1001.0;
            else
                framerate = 60.0;
        }

        /* determine display dimensions */
        if (displayformat) {
            /* lookup specified format */
            if (divine_video_format(displayformat, &dis_width, &dis_height, &interlaced, &framerate, &pixelformat)<0)
                dlexit("failed to determine output video format from filename: %s", displayformat);
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
                case 1088: dis_height = 1080; break;
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
                if (verbose>=1)
                    dlmessage("mode available: %ldx%ld%c%.2f", mode->GetWidth(), mode->GetHeight(), mode->GetFieldDominance()!=bmdProgressiveFrame? 'i' : 'p', (double)framerate_scale/framerate_duration);
                if (mode->GetWidth()==dis_width && mode->GetHeight()==dis_height) {
                    if ((mode->GetFieldDominance()==bmdProgressiveFrame) ^ interlaced) {
                        mode->GetFrameRate(&framerate_duration, &framerate_scale);
                        /* look for an integer frame rate match */
                        if ((framerate_scale / framerate_duration)==(int)floor(framerate))
                            break;
                    }
                }
            }
            iterator->Release();

            if (mode==NULL)
                dlexit("error: failed to find mode for %dx%d%c%.2f", pic_width, pic_height, interlaced? 'i' : 'p', framerate);

            /* display mode name */
            const char *name;
            if (mode->GetName(&name)==S_OK)
                dlmessage("info: video mode %s", name);
        }

        /* set the video output mode */
        if (video) {
            HRESULT result = output->EnableVideoOutput(mode->GetDisplayMode(), bmdVideoOutputFlagDefault);
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
        decode_t decode = {0, 0};

        /* playback timestamp boundaries */
        tstamp_t video_start_time = 1ll<<34;
        tstamp_t video_end_time = 0ll;
        tstamp_t audio_start_time = 1ll<<34;
        tstamp_t audio_end_time = 0ll;

        /* video frame history buffer */
        int num_history_frames = 0;
        const int MAX_HISTORY_FRAMES = 30;
        typedef struct {
            IDeckLinkVideoFrame *frame;
            tstamp_t timestamp;
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

        /* main loop */
        int queuenum = 0;
        int framenum = 0;
        int blocknum = 0;
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
                    output->GetScheduledStreamTime(90000, &time, &speed);
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
                        dlmessage("current time is %s, pause timestamp %s", describe_time(time), describe_time(history_buffer[pause_index].timestamp));

                    /* stop the playback */
                    if (output->StopScheduledPlayback(0, NULL, 0) != S_OK)
                        dlexit("%s: error: failed to pause video playback");
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
                        if (output->ScheduleVideoFrame(history_buffer[i].frame, history_buffer[i].timestamp, lround(90000.0/framerate), 90000)==S_OK)
                            video_start_time = history_buffer[i].timestamp;
                        else
                            break;

                    /* resume the audio output */
                    HRESULT result = output->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, 2, bmdAudioOutputStreamTimestamped);
                    if (result != S_OK) {
                        dlmessage("error: failed to resume audio output\n");
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
                HRESULT result = output->ScheduleVideoFrame(frame, decode.timestamp, lround(90000.0/framerate), 90000);
                if (result != S_OK) {
                    switch (result) {
                        case E_ACCESSDENIED : fprintf(stderr, "%s: error: frame %d: video output disabled when queueing video frame\n", appname, queuenum); break;
                        case E_OUTOFMEMORY  : fprintf(stderr, "%s: error: frame %d: too many frames are scheduled when queueing video frame\n", appname, queuenum);
                        case E_INVALIDARG   : fprintf(stderr, "%s: error: frame %d: frame attributes are invalid when queueing video frame\n", appname, queuenum); break;
                        default             : fprintf(stderr, "%s: error: frame %d: failed to schedule video frame, timestamp %s\n", appname, queuenum, describe_time(decode.timestamp)); break;
                    }
                    break;
                }
                queuenum++;
            }

            /* end pre-roll after a certain number of frames */
            if (preroll && queuenum>=PREROLL_FRAMES) {
                /* preroll complete */
                preroll = 0;

                /* end audio preroll
                if (output->EndAudioPreroll()!=S_OK) {
                    dlmessage("error: failed to end audio preroll");
                    return 2;
                } */

                /* start the playback */
                if (output->StartScheduledPlayback(video_start_time, 90000, 1.0) != S_OK)
                    dlexit("error: failed to start video playback");

                if (verbose>=1)
                    dlmessage("info: pre-rolled %d frames", queuenum);
                if (verbose>=1)
                    dlmessage("info: start time of video is %lld, %s", video_start_time, describe_time(video_start_time));
            }

            /* decode the next frame */
            if (video) {
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
                decode = video->decode(uyvy, frame->GetRowBytes()*frame->GetHeight());
                if (decode.size==0) {
                    dlmessage("error: failed to decode video frame %d in file \"%s\"", framenum, filename);
                    if (source->timeout())
                        restart = 1; /* wait for next sequence */
                    break;
                }
                video_start_time = mmin(decode.timestamp, video_start_time);
                video_end_time = mmax(decode.timestamp, video_start_time);

                if (verbose>=2)
                    dlmessage("info: frame %d timestamp %s, decode %.1fms render %.1fms", framenum, describe_time(decode.timestamp), decode.decode_time/1000.0, decode.render_time/1000.0);
                framenum++;

                /* store the frame in the history buffer */
                if (num_history_frames<MAX_HISTORY_FRAMES) {
                    history_buffer[num_history_frames].frame = frame;
                    history_buffer[num_history_frames].timestamp = decode.timestamp;
                    num_history_frames++;
                } else {
                    history_buffer[0].frame->Release();
                    memmove(&history_buffer[0], &history_buffer[1], (MAX_HISTORY_FRAMES-1)*sizeof(history_frame_t));
                    history_buffer[MAX_HISTORY_FRAMES-1].frame = frame;
                    history_buffer[MAX_HISTORY_FRAMES-1].timestamp = decode.timestamp;
                }
            }

            /* maintain audio buffer level */
            if (audio /*&& !preroll*/) {

                while (true) {
                    unsigned int buffered;
                    if (output->GetBufferedAudioSampleFrameCount(&buffered) != S_OK)
                        dlexit("failed to get audio buffer level");

                    if (buffered >= 48000)
                        break;

                    /* decode audio */
                    decode_t decode = audio->decode(aud_data, aud_size);
                    if (verbose>=1 && blocknum==0) {
                        audio_start_time = decode.timestamp;
                        dlmessage("info: start time of audio is %lld, %s", audio_start_time, describe_time(audio_start_time));
                    }
                    audio_end_time = mmax(decode.timestamp, audio_start_time);


                    /* buffer decoded audio */
                    uint32_t scheduled;
                    HRESULT result = output->ScheduleAudioSamples(aud_data, decode.size/2, decode.timestamp, 90000, &scheduled);
                    //dlmessage("buffer level %d: decoded %d bytes at timestamp %lld and scheduled %d samples", buffered, decode.size, decode.timestamp, scheduled);
                    if (result != S_OK) {
                        dlmessage("error: block %d: failed to schedule audio data", blocknum);
                        delete audio;
                        audio = NULL;
                        break;
                    }
                    blocknum++;
                }
            }

            /* start the playback in audio only mode */
            if (audio && !video) {
                /* preroll complete */
                preroll = 0;

                /* start the playback */
                if (output->StartScheduledPlayback(audio_start_time, 90000, 1.0) != S_OK)
                    dlexit("error: failed to start audio playback");
            }
        }

        /* stop the status display */
        exit_thread = 1;
        pthread_join(status_thread, NULL);

        /* stop the video output */
        output->StopScheduledPlayback(0, NULL, 0);
        output->DisableVideoOutput();
        output->DisableAudioOutput();

        /* release all frames in the history buffer */
        for (i=0; i<num_history_frames; i++)
            if (history_buffer[i].frame)
                history_buffer[i].frame->Release();

        /* the semaphone has to be re-initialised */
        sem_destroy(&sem);

        /* report timing statistics */
        if (verbose>=1)
            dlmessage("\nmean decode time=%.1fms, mean render time=%.1fms", 0.0, 0.0);

        /* tidy up */
        delete source;
        delete video;
        delete audio;
    }

    /* tidy up */
    card->Release();
    iterator->Release();
    if (aud_data)
        free(aud_data);

    /* report statistics */
    if (verbose>=0)
        dlmessage("%d frames: %d late, %d dropped, %d flushed", completed, late, dropped, flushed);

    return 0;
}
