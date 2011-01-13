/*
 * Description: play raw video files.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2010 4i2i Communications Ltd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <math.h>
#include <inttypes.h>

extern "C" {
    #include <mpeg2dec/mpeg2.h>
    #include <mpeg2dec/mpeg2convert.h>
}

#include "DeckLinkAPI.h"

#include "dlutil.h"

const char *appname = "dlplay";

/* semaphores for synchronisation */
sem_t sem;

/* display statistics */
unsigned int completed;
unsigned int late, dropped, flushed;

class callback : public IDeckLinkVideoOutputCallback
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
};

callback::callback(IDeckLinkOutput *output)
{
    if (output->SetScheduledFrameCompletionCallback(this)!=S_OK)
        dlexit("%s: error: could not set callback object");
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
    /* when a video frame has been released by the API, increment a semaphore */
    sem_post(&sem);
    frame->Release();
    completed++;
    switch (result) {
        case bmdOutputFrameDisplayedLate: late++; break;
        case bmdOutputFrameDropped: dropped++; break;
        case bmdOutputFrameFlushed: flushed++; break;
    }
    return S_OK;
}

HRESULT callback::ScheduledPlaybackHasStopped ()
{
    return S_OK;
}

/*****************************************/

void usage(int exitcode)
{
    fprintf(stderr, "%s: play raw video files\n", appname);
    fprintf(stderr, "usage: %s [options] <file> [<file>...]\n", appname);
    fprintf(stderr, "  -a, --firstframe    : index of first frame in input to display (default: 0)\n");
    fprintf(stderr, "  -n, --numframes     : number of frames in input to display (default: all)\n");
    fprintf(stderr, "  -m, --ntscmode      : use proper ntsc framerate, e.g. 29.97 instead of 30fps (default: on)\n");
    fprintf(stderr, "  -l, --luma          : display luma plane only (default: luma and chroma)\n");
    fprintf(stderr, "  -q, --quiet         : decrease verbosity, can be used multiple times\n");
    fprintf(stderr, "  -v, --verbose       : increase verbosity, can be used multiple times\n");
    fprintf(stderr, "  --                  : disable argument processing\n");
    fprintf(stderr, "  -u, --help, --usage : print this usage message\n");
    exit(exitcode);
}

int divine_video_format(const char *filename, int *width, int *height, bool *interlaced, float *framerate, pixelformat_t *pixelformat)
{
    struct format_t {
        const char *name;
        int width;
        int height;
        bool interlaced;
        float framerate;
    };

    const struct format_t formats[] = {
        { "1080p24",    1920, 1080, false, 24.0 },
        { "1080p25",    1920, 1080, false, 25.0 },
        { "1080p30",    1920, 1080, false, 30.0 },
        { "1080i50",    1920, 1080, true,  25.0 },
        { "1080i59.94", 1920, 1080, true,  30000.0/1001.0 },
        { "1080i5994",  1920, 1080, true,  30000.0/1001.0 },
        { "1080i60",    1920, 1080, true,  30.0 },
        { "1080i",      1920, 1080, true,  30000.0/1001.0 },
        { "720p50",     1280, 720,  false, 50.0 },
        { "720p60",     1280, 720,  false, 60.0 },
        { "720p59.94",  1280, 720,  false, 60000.0/1001.0 },
        { "720p5994",   1280, 720,  false, 60000.0/1001.0 },
        { "720p",       1280, 720,  false, 60000.0/1001.0 },
        { "576i50",     720,  576,  true,  25.0 },
        { "576i",       720,  576,  true,  25.0 },
        { "pal",        720,  576,  true,  25.0 },
        { "480i60",     720,  480,  true,  30.0 },
        { "480i59.94",  720,  480,  true,  30000.0/1001.0 },
        { "480i5994",   720,  480,  true,  30000.0/1001.0 },
        { "480i",       720,  480,  true,  30000.0/1001.0 },
        { "ntsc",       720,  480,  true,  30000.0/1001.0 },
    };

    if (strstr(filename, "uyvy")!=NULL || strstr(filename, "UYVY")!=NULL)
        *pixelformat = UYVY;
    else if (strstr(filename, "422")!=NULL)
        *pixelformat = I422;
    else
        *pixelformat = I420;

    for (unsigned i=0; i<sizeof(formats)/sizeof(struct format_t); i++) {
        struct format_t f = formats[i];

        if (strstr(filename, f.name)!=NULL) {
            *width = f.width;
            *height = f.height;
            *interlaced = f.interlaced;
            *framerate = f.framerate;
            return 0;
        }
    }
    return -1;
}

void convert_i420_uyvy(const unsigned char *i420, unsigned char *uyvy, int width, int height, pixelformat_t pixelformat)
{
    const unsigned char *yuv[3] = {i420};
    for (int y=0; y<height; y++) {
        if (pixelformat==I422) {
            yuv[1] = i420 + width*height + (width/2)*y;
            yuv[2] = i420 + width*height*6/4 + (width/2)*y;
        } else {
            yuv[1] = i420 + width*height + (width/2)*(y/2);
            yuv[2] = i420 + width*height*5/4 + (width/2)*(y/2);
        }
        for (int x=0; x<width/2; x++) {
            *(uyvy++) = *(yuv[1]++);
            *(uyvy++) = *(yuv[0]++);
            *(uyvy++) = *(yuv[2]++);
            *(uyvy++) = *(yuv[0]++);
        }
    }
}

void convert_yuv_uyvy(const unsigned char *yuv[3], unsigned char *uyvy, int width, int height, pixelformat_t pixelformat)
{
    const unsigned char *ptr[3] = {yuv[0]};
    for (int y=0; y<height; y++) {
        if (pixelformat==I422) {
            ptr[1] = yuv[1] + (width/2)*y;
            ptr[2] = yuv[2] + (width/2)*y;
        } else {
            ptr[1] = yuv[1] + (width/2)*(y/2);
            ptr[2] = yuv[2] + (width/2)*(y/2);
        }
        for (int x=0; x<width/2; x++) {
            *(uyvy++) = *(ptr[1]++);
            *(uyvy++) = *(ptr[0]++);
            *(uyvy++) = *(ptr[2]++);
            *(uyvy++) = *(ptr[0]++);
        }
    }
}

void convert_i420_uyvy_lumaonly(const unsigned char *i420, unsigned char *uyvy, int width, int height)
{
    for (int y=0; y<height; y++) {
        for (int x=0; x<width/2; x++) {
            *(uyvy++) = 0x80;
            *(uyvy++) = *(i420++);
            *(uyvy++) = 0x80;
            *(uyvy++) = *(i420++);
        }
    }
}

void *display_status(void *arg)
{
    IDeckLinkOutput *output = (IDeckLinkOutput *)arg;

    sleep(1);

    unsigned int framerate = completed;
    unsigned int latecount = late;
    unsigned int dropcount = dropped;
    do {
        char string[256];
        int len = 0;

        /* get buffer depth */
        uint32_t buffered;
        output->GetBufferedVideoFrameCount(&buffered);

        /* display status output */
        len += snprintf(string+len, sizeof(string)-len, "%dfps buffer %d", completed-framerate, buffered);
        framerate = completed;
        if (late!=latecount) {
            len += snprintf(string+len, sizeof(string)-len, " late %d frame%s", late-latecount, late-latecount>1? "s" : "");
            latecount = late;
        }
        if (dropped!=dropcount) {
            len += snprintf(string+len, sizeof(string)-len, " dropped %d frame%s", dropped-dropcount, dropped-dropcount>1? "s" : "");
            dropcount = dropped;
        }
        fprintf(stdout, "\rperformance: %s", string);
        fflush(stdout);

        /* clear stats */

        /* wait for stats to accumulate */
        sleep(1);
    } while (1);

    pthread_exit(0);
}

int main(int argc, char *argv[])
{
    FILE *filein = stdin;
    char *filename[16] = {0};
    unsigned int numfiles = 0;
    filetype_t filetype = OTHER;

    /* command line defaults */
    int width = 0;
    int height = 0;
    bool interlaced = 0;
    float framerate = 0.0;
    pixelformat_t pixelformat;
    int ntscmode = 1;   /* use e.g. 29.97 instead of 30fps */
    int firstframe = 0;
    int numframes = -1;
    int lumaonly = 0;
    int use_mmap = 0;
    int verbose = 0;

    /* memory map variables */
    off_t pagesize = sysconf(_SC_PAGE_SIZE);
    off_t pageindex;
    off_t offset = 0;

    /* buffer variables */
    size_t size = 0;
    unsigned char *data = NULL;
    size_t read = 0;
    int maxframes = 0;

    /* libmpeg2 variables */
    mpeg2dec_t *mpeg2dec = NULL;
    const mpeg2_info_t *info = NULL;

    /* parse command line for options */
    while (1) {
        static struct option long_options[] = {
            {"firstframe",1, NULL, 'a'},
            {"numframes", 1, NULL, 'n'},
            {"ntscmode",  1, NULL, 'm'},
            {"luma",      0, NULL, 'l'},
            {"quiet",     0, NULL, 'q'},
            {"verbose",   0, NULL, 'v'},
            {"usage",     0, NULL, 'u'},
            {"help",      0, NULL, 'u'},
            {NULL,        0, NULL,  0 }
        };

        int optchar = getopt_long(argc, argv, "a:n:m:lqvu", long_options, NULL);
        if (optchar==-1)
            break;

        switch (optchar) {
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

    /* all non-options are input filenames */
    while (optind<argc) {
        if (numfiles < sizeof(filename)/sizeof(filename[0]))
            filename[numfiles++] = argv[optind++];
        else
            dlexit("more than %d input files", numfiles);
    }

    /* sanity check the command line */
    if (numfiles<1)
        usage(1);

    /* determine the file type */
    if (strstr(filename[0], "m2v")!=NULL || strstr(filename[0], "M2V")!=NULL)
        filetype = M2V;
    else
        filetype = YUV;

    /* open the first input file */
    filein = fopen(filename[0], "rb");
    if (!filein)
        dlerror("error: failed to open input file \"%s\"", filename[0]);

    /* stat the first input file */
    struct stat stat;
    fstat(fileno(filein), &stat);

    /* initialise the semaphore */
    sem_init(&sem, 0, 0);

    /* initialise the DeckLink API */
    IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
    if (iterator==NULL)
        dlexit("error: could not initialise, the DeckLink driver may not be installed");

    /* connect to the first card in the system */
    IDeckLink *card;
    if (iterator->Next(&card)!=S_OK)
        dlexit("error: no DeckLink cards found");

    /* obtain the audio/video output interface */
    IDeckLinkOutput *output;
    if (card->QueryInterface(IID_IDeckLinkOutput, (void**)&output)!=S_OK)
        dlexit("%s: error: could not obtain the video output interface");

    /* create callback object */
    class callback the_callback(output);

    /* figure out the mode to set */
    switch (filetype) {
        case YUV:
            if (divine_video_format(filename[0], &width, &height, &interlaced, &framerate, &pixelformat)<0)
                dlexit("failed to determine output video format from filename: %s", filename[0]);
            if (verbose>=1)
                dlmessage("input file is %dx%d%c%.2f %s", width, height, interlaced? 'i' : 'p', framerate, pixelformatname[pixelformat]);

            /* allocate the read buffer */
            size = pixelformat==I422? width*height*2 : width*height*3/2;
            data = (unsigned char *) malloc(size);

            /* skip to the first frame */
            if (firstframe) {
                off_t skip = firstframe * size;
                if (fseeko(filein, skip, SEEK_SET)<0)
                    dlerror("failed to seek in input file");
            }

            /* calculate the number of frames in the input file */
            maxframes = pixelformat==I420? stat.st_size / (width*height*3/2) : stat.st_size / (width*height*2);

            break;

        case M2V:
            /* initialise the mpeg2 decoder */
            mpeg2dec = mpeg2_init();
            if (mpeg2dec==NULL)
                dlexit("failed to initialise libmpeg2");
            info = mpeg2_info(mpeg2dec);
            //mpeg2_convert(mpeg2dec, mpeg2convert_rgb32, NULL);
            mpeg2_accel(MPEG2_ACCEL_DETECT);

            /* allocate the read buffer */
            size = 32*1024;
            data = (unsigned char *) malloc(size);

            /* find the sequence header */
            {
                int seq_found = 0;

                do {
                    mpeg2_state_t state = mpeg2_parse(mpeg2dec);
                    switch (state) {
                        case STATE_BUFFER:
                            read = fread(data, 1, size, filein);
                            mpeg2_buffer(mpeg2dec, data, data+read);
                            break;

                        case STATE_SEQUENCE:
                            width = info->sequence->width;
                            height = info->sequence->height;
                            interlaced = 1;
                            framerate = 27000000.0/info->sequence->frame_period;
                            pixelformat = info->sequence->height==info->sequence->chroma_height? I422 : I420;
                            if (verbose>=1)
                                dlmessage("input file is %dx%d%c%.2f %s", width, height, interlaced? 'i' : 'p', framerate, pixelformatname[pixelformat]);
                            seq_found = 1;
                            break;

                        default:
                            break;
                    }
                } while(read && !seq_found);
            }
            break;

        default:
            dlexit("unknown input file type");
    }

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

    /* get display mode iterator */
    IDeckLinkDisplayMode *mode;
    BMDTimeValue framerate_duration;
    BMDTimeScale framerate_scale;
    {
        IDeckLinkDisplayModeIterator *iterator;
        if (output->GetDisplayModeIterator(&iterator) != S_OK)
            return false;

        /* find mode for given width and height */
        while (iterator->Next(&mode) == S_OK) {
            mode->GetFrameRate(&framerate_duration, &framerate_scale);
            fprintf(stderr, "%ldx%ld%c%lld/%lld\n", mode->GetWidth(), mode->GetHeight(), mode->GetFieldDominance()!=bmdProgressiveFrame? 'i' : 'p', framerate_duration, framerate_scale);
            if (mode->GetWidth()==width && (mode->GetHeight()==height || (mode->GetHeight()==486 && height==480) || (mode->GetHeight()==1080 && height==1088))) {
                if (mode->GetFieldDominance()==bmdProgressiveFrame ^ interlaced) {
                    mode->GetFrameRate(&framerate_duration, &framerate_scale);
                    /* look for an integer frame rate match */
                    if ((framerate_scale / framerate_duration)==(int)floor(framerate))
                        break;
                }
            }
        }
        iterator->Release();
    }

    if (mode==NULL)
        dlexit("error: failed to find mode for %dx%d%c%.2f", width, height, interlaced? 'i' : 'p', framerate);

    /* display mode name */
    const char *name;
    if (mode->GetName(&name)==S_OK)
        dlmessage("info: video mode %s", name);

    /* set the video output mode */
    HRESULT result = output->EnableVideoOutput(mode->GetDisplayMode(), bmdVideoOutputFlagDefault);
    if (result != S_OK) {
        switch (result) {
            case E_ACCESSDENIED : fprintf(stderr, "%s: error: access denied when enabling video output\n", appname); break;
            case E_OUTOFMEMORY  : fprintf(stderr, "%s: error: out of memory when enabling video output\n", appname); break;
            default             : fprintf(stderr, "%s: error: failed to enable video output\n", appname); break;
        }
        return 2;
    }

    dlmessage("press return to exit...");

    /* carry the last frame from the preroll to the main loop */
    IDeckLinkMutableVideoFrame *frame;

    /* preroll as many frames as possible */
    int index = 0;
    int framenum = 0;
    int frame_done;
    while (!feof(filein) && (framenum<numframes || numframes<0)) {

        switch (filetype) {
            case YUV:
                /* loop input file */
                if (index==maxframes) {
                    index = firstframe;
                    off_t skip = index * size;
                    if (fseeko(filein, skip, SEEK_SET)<0)
                        dlerror("failed to seek in input file");
                }

                /* allocate a new frame object */
                if (output->CreateVideoFrame(width, height, width*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame)!=S_OK)
                    dlexit("error: failed to create video frame\n");

                unsigned char *uyvy;
                frame->GetBytes((void**)&uyvy);
                if (pixelformat==UYVY) {
                    /* read directly into frame */
                    if (fread(uyvy, width*height*2, 1, filein)!=1)
                        dlerror("failed to read frame from input file");
                } else {
                    /* read frame from file */
                    if (fread(data, size, 1, filein)!=1)
                        dlerror("failed to read frame from input file");

                    /* convert to uyvy */
                    if (!lumaonly)
                        convert_i420_uyvy(data, uyvy, width, height, pixelformat);
                    else
                        convert_i420_uyvy_lumaonly(data, uyvy, width, height);
                }
                break;

            case M2V:
                frame_done = 0;
                do {
                    mpeg2_state_t state = mpeg2_parse(mpeg2dec);
                    switch (state) {
                        case STATE_BUFFER:
                            read = fread(data, 1, size, filein);
                            mpeg2_buffer(mpeg2dec, data, data+read);
                            break;

                        case STATE_SLICE:
                        case STATE_END:
                            if (info->display_fbuf) {
                                /* allocate a new frame object */
#if 0
                                if (output->CreateVideoFrame(width, height, width*4, bmdFormat8BitBGRA, bmdFrameFlagDefault, &frame)!=S_OK)
                                    dlexit("error: failed to create video frame\n");
                                unsigned char *uyvy;
                                frame->GetBytes((void**)&uyvy);
                                memcpy(uyvy, info->display_fbuf->buf[0], width*height*4);
#else
                                if (output->CreateVideoFrame(width, height, width*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame)!=S_OK)
                                    dlexit("error: failed to create video frame\n");
                                unsigned char *uyvy;
                                frame->GetBytes((void**)&uyvy);
                                const unsigned char *yuv[3] = {info->display_fbuf->buf[0], info->display_fbuf->buf[1], info->display_fbuf->buf[2]};
                                convert_yuv_uyvy(yuv, uyvy, width, height, pixelformat);
#endif
                                frame_done = 1;
                            }
                            break;

                        default:
                            break;
                    }
                } while(read && !frame_done);
                break;

            default:
                dlexit("unknown input file type");
        }

        result = output->ScheduleVideoFrame(frame, framenum*framerate_duration, framerate_duration, framerate_scale);
        if (result != S_OK) {
            switch (result) {
                case E_ACCESSDENIED : fprintf(stderr, "%s: error: frame %d: video output disabled when queueing video frame\n", appname, framenum); break;
                case E_OUTOFMEMORY  : fprintf(stderr, "%s: error: frame %d: too many frames are scheduled when queueing video frame\n", appname, framenum); break;
                case E_INVALIDARG   : fprintf(stderr, "%s: error: frame %d: frame attributes are invalid when queueing video frame\n", appname, framenum); break;
            }
            /* preroll complete */
            break;
        }
        index++;
        framenum++;
    }

    /* start the status thread */
    pthread_t status_thread;
    if (pthread_create(&status_thread, NULL, display_status, output)<0)
        dlerror("failed to create status thread");

    /* start the video output */
    if (output->StartScheduledPlayback(0, 100, 1.0) != S_OK)
        dlexit("%s: error: failed to start video playback");

    if (verbose>=0)
        dlmessage("info: pre-rolled %d frames", framenum);

    /* continue queueing frames when semaphore is signalled */
    while (!feof(filein) && (framenum<numframes || numframes<0)) {
        /* check that one or more frames have been displayed */
        sem_wait(&sem);

        /* enqueue previous frame */
        result = output->ScheduleVideoFrame(frame, framenum*framerate_duration, framerate_duration, framerate_scale);
        if (result != S_OK) {
            switch (result) {
                case E_ACCESSDENIED : fprintf(stderr, "%s: error: frame %d: video output disabled when queueing video frame\n", appname, framenum); break;
                case E_OUTOFMEMORY  : fprintf(stderr, "%s: error: frame %d: too many frames are scheduled when queueing video frame\n", appname, framenum); break;
                case E_INVALIDARG   : fprintf(stderr, "%s: error: frame %d: frame attributes are invalid when queueing video frame\n", appname, framenum); break;
                default             : fprintf(stderr, "%s: error: frame %d: failed to queue video frame\n", appname, framenum); break;
            }
            break;
        }
        index++;
        framenum++;

        /* check for user input */
        {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fileno(stdin), &fds);
            struct timeval tv = {0, 0};
            select(fileno(stdin)+1, &fds, NULL, NULL, &tv);
            if (FD_ISSET(fileno(stdin), &fds))
                break;
        }

        switch (filetype) {
            case YUV:
                /* loop input file */
                if (index==maxframes) {
                    index = firstframe;
                    off_t skip = index * size;
                    if (fseeko(filein, skip, SEEK_SET)<0)
                        dlerror("failed to seek in input file");
                }

                /* allocate a new frame object */
                if (output->CreateVideoFrame(width, height, width*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame)!=S_OK)
                    dlexit("error: failed to create video frame\n");

                unsigned char *uyvy;
                frame->GetBytes((void**)&uyvy);
                if (pixelformat==UYVY) {
                    /* read directly into frame */
                    if (fread(uyvy, width*height*2, 1, filein)!=1)
                        dlerror("failed to read frame from input file");
                } else {
                    if (use_mmap) {
                        /* align address to page size */
                        pageindex = (index*size) / pagesize;
                        offset = (index*size) - (pageindex*pagesize);

                        /* memory map next frame from file */
                        if ((data = (unsigned char *) mmap(NULL, size+pagesize, PROT_READ, MAP_PRIVATE, fileno(filein), pageindex*pagesize))==MAP_FAILED)
                            dlerror("failed to map frame from input file");
                    } else {
                        /* read next frame from file */
                        if (fread(data, size, 1, filein)!=1)
                            dlerror("failed to read frame from input file");
                    }

                    /* convert to uyvy */
                    if (!lumaonly)
                        convert_i420_uyvy(data, uyvy, width, height, pixelformat);
                    else
                        convert_i420_uyvy_lumaonly(data, uyvy, width, height);
                }

                if (use_mmap)
                    munmap(data, size+pagesize);
                break;

            case M2V:
                frame_done = 0;
                do {
                    mpeg2_state_t state = mpeg2_parse(mpeg2dec);
                    switch (state) {
                        case STATE_BUFFER:
                            read = fread(data, 1, size, filein);
                            if (feof(filein) && !ferror(filein)) {
                                fseek(filein, 0, SEEK_SET);
                                read = fread(data, 1, size, filein);
                            }
                            mpeg2_buffer(mpeg2dec, data, data+read);
                            break;

                        case STATE_SLICE:
                        case STATE_END:
                            if (info->display_fbuf) {
                                /* allocate a new frame object */
#if 0
                                if (output->CreateVideoFrame(width, height, width*4, bmdFormat8BitBGRA, bmdFrameFlagDefault, &frame)!=S_OK)
                                    dlexit("error: failed to create video frame\n");
                                unsigned char *uyvy;
                                frame->GetBytes((void**)&uyvy);
                                memcpy(uyvy, info->display_fbuf->buf[0], width*height*4);
#else
                                if (output->CreateVideoFrame(width, height, width*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame)!=S_OK)
                                    dlexit("error: failed to create video frame\n");
                                const unsigned char *yuv[3] = {info->display_fbuf->buf[0], info->display_fbuf->buf[1], info->display_fbuf->buf[2]};
                                unsigned char *uyvy;
                                frame->GetBytes((void**)&uyvy);
                                convert_yuv_uyvy(yuv, uyvy, width, height, pixelformat);
#endif
                                frame_done = 1;
                            }
                            break;

                        default:
                            break;
                    }
                } while(read && !frame_done);
                break;

            default:
                dlexit("unknown input file type");
        }
    }

    /* stop the video output */
    output->StopScheduledPlayback(0, NULL, 0);
    output->DisableVideoOutput();

    /* tidy up */
    if (mpeg2dec)
        mpeg2_close(mpeg2dec);
    sem_destroy(&sem);
    card->Release();
    iterator->Release();

    /* report statistics */
    if (verbose>=0)
        fprintf(stdout, "%d frames: %d late, %d dropped, %d flushed\n", framenum, late, dropped, flushed);

    return 0;
}
