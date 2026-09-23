/*
 * Description: capture raw video.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2011 4i2i Communications Ltd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <limits.h>
#include <semaphore.h>
#include <math.h>

#include "DeckLinkAPI.h"

#include "dlutil.h"

const char *appname = "dlcap";

class DeckLinkCapture : public IDeckLinkInputCallback
{
    public:
        DeckLinkCapture();
        ~DeckLinkCapture();

        /* implementation of IDeckLinkVideoOutputCallback */
        /* IUnknown needs only a dummy implementation */
        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }
        virtual ULONG STDMETHODCALLTYPE AddRef(void)  {return 1;}
        virtual ULONG STDMETHODCALLTYPE Release(void) {return 1;}

        virtual HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode*, BMDDetectedVideoInputFormatFlags);
        virtual HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*);

        /* control interface */
        HRESULT Init();
        BMDDisplayMode DetectInput();
        HRESULT Start();
        HRESULT Start(BMDDisplayMode mode);
        HRESULT Start(int width, int height, bool interlaced, float framerate);
        void Wait();
        void Stop();

        /* input format reporting */
        void ReportInputFormat(IDeckLinkDisplayMode *mode);

        /* configuration interface */
        void SetOutputFile(FILE *f) {fileout = f;}
        void SetMaxframes(int m) {maxframes = m;}

        /* status interface */
        int GetNumCaptures() { return numframes; }
        int GetFrameCount()  {return framecount;}

    private:
        /* decklink variables */
        IDeckLinkIterator *iterator;
        IDeckLink *card;
        IDeckLinkInput *input;
        IDeckLinkDisplayMode *mode;

        /* input format variables */
        BMDDisplayMode req_mode;
        BMDDisplayMode inp_mode;
        char inp_mode_name[32];
        float inp_mode_framerate;
        bool detecting;

        /* file output variables */
        FILE *fileout;
        int numframes;
        int maxframes;
        int framecount;

        /* synchronisation semaphores */
        sem_t sem_done;
        sem_t sem_format;
};

DeckLinkCapture::DeckLinkCapture()
{
    iterator = NULL;
    card = NULL;
    input = NULL;
    mode = NULL;
    req_mode = 0;
    inp_mode = 0;
    inp_mode_name[0] = '\0';
    inp_mode_framerate = 30.0;
    detecting = false;
    fileout = NULL;
    numframes = 0;
    maxframes = 0;
    framecount = 0;
}

HRESULT DeckLinkCapture::Init()
{
    /* initialise the DeckLink API */
    iterator = CreateDeckLinkIteratorInstance();
    if (iterator==NULL)
        dlexit("error: could not initialise, the DeckLink driver may not be installed");

    /* connect to the first card in the system */
    HRESULT result = iterator->Next(&card);
    if (result!=S_OK)
        dlapierror(result, "error: no DeckLink cards found");

    /* print the model name of the DeckLink card */
    char *name = NULL;
    result = card->GetModelName((const char **) &name);
    if (result == S_OK) {
        dlmessage("info: found a %s", name);
        free(name);
    }

    /* obtain the video input interface */
    void *voidptr;
    result = card->QueryInterface(IID_IDeckLinkInput, &voidptr);
    if (result!=S_OK)
        dlexit("error: could not obtain the video input interface");
    input = (IDeckLinkInput *)voidptr;

    /* attach this object as the callback */
    result = input->SetCallback(this);
    if (result!=S_OK)
        dlapierror(result, "error: could not set video callback object");

    return S_OK;
}

/* record and report the sdi input format */
void DeckLinkCapture::ReportInputFormat(IDeckLinkDisplayMode *mode)
{
    BMDTimeValue frame_duration;
    BMDTimeScale time_scale;

    inp_mode = mode->GetDisplayMode();
    snprintf(inp_mode_name, sizeof(inp_mode_name), "%s", describe_display_mode(mode));
    mode->GetFrameRate(&frame_duration, &time_scale);
    inp_mode_framerate = (float)time_scale / (float)frame_duration;

    /* report the input format in short form */
    dlmessage("info: detected sdi input format %s", inp_mode_name);
}

/* auto-detect input format */
BMDDisplayMode DeckLinkCapture::DetectInput()
{
    /* start the video input with a random choice of mode */
    detecting = true;
    Start(bmdModeHD1080i6000);

    /* wait for input format detection */
    sem_wait(&sem_format);

    /* stop */
    Stop();
    detecting = false;

    return inp_mode;
}

HRESULT DeckLinkCapture::Start(BMDDisplayMode mode)
{
    /* remember the requested mode, the input may already match it */
    req_mode = mode;

    /* configure the video input */
    HRESULT result = input->EnableVideoInput(mode, bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection);
    if (result!=S_OK)
        dlapierror(result, "failed to configure video input");

    /* initialise the counters */
    numframes = 0;
    framecount = 0;

    /* initialise the semaphores */
    sem_init(&sem_done, 0, 0);
    sem_init(&sem_format, 0, 0);

    /* start the video input */
    result = input->StartStreams();
    if (result!=S_OK) {
        sem_destroy(&sem_done);
        sem_destroy(&sem_format);
        dlapierror(result, "failed to start video input");
    }

    return S_OK;
}

HRESULT DeckLinkCapture::Start()
{
    DetectInput();
    return Start(inp_mode);
}

HRESULT DeckLinkCapture::Start(int width, int height, bool interlaced, float framerate)
{
    /* find display mode from  arguments */
    IDeckLinkDisplayMode *mode;
    {
        IDeckLinkDisplayModeIterator *iterator;
        HRESULT result = input->GetDisplayModeIterator(&iterator);
        if (result != S_OK)
            dlapierror(result, "failed to get display mode iterator");

        /* find mode for given width and height */
        while (iterator->Next(&mode) == S_OK) {
            if (mode->GetWidth()==width && mode->GetHeight()==height) {
                if ((mode->GetFieldDominance()==bmdProgressiveFrame) ^ interlaced) {
                    BMDTimeValue framerate_duration;
                    BMDTimeScale framerate_scale;
                    mode->GetFrameRate(&framerate_duration, &framerate_scale);
                    /* look for an integer frame rate match */
                    if ((framerate_scale / framerate_duration)==(int)floor(framerate))
                        break;
                }
            }
        }
        iterator->Release();

        if (mode==NULL)
            dlexit("error: failed to find mode for %dx%d%c%.2f", width, height, interlaced? 'i' : 'p', framerate);

        /* display mode name */
        dlmessage("info: requested capture format %s", describe_display_mode(mode));
    }

    return Start(mode->GetDisplayMode());
}

HRESULT DeckLinkCapture::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoframe, IDeckLinkAudioInputPacket* audioframe)
{
    //void *audioFrameBytes;

    /* handle video frame */
    if (videoframe)
    {
        if (videoframe->GetFlags() & bmdFrameHasNoInputSource)
        {
            if (framecount && framecount%lroundf(inp_mode_framerate)==0) {
                if (inp_mode_name[0]=='\0')
                    dlstatus("frame %d: no input signal detected", framecount);
                else
                    dlstatus("frame %d: format mismatch: %s detected", framecount, inp_mode_name);
            }
        }
        else
        {
            /* a frame with a valid signal while detecting means the input matches the requested mode */
            if (detecting) {
                if (inp_mode_name[0]=='\0') {
                    IDeckLinkDisplayMode *reqmode = NULL;
                    if (input->GetDisplayMode(req_mode, &reqmode)==S_OK) {
                        ReportInputFormat(reqmode);
                        reqmode->Release();
                    }
                }
                sem_post(&sem_format);
                return S_OK;
            }

            /* write video frame to file */
            if (fileout) {
                int framesize = videoframe->GetRowBytes() * videoframe->GetHeight();
                IDeckLinkVideoBuffer* videoBuffer;
                void *framedata;

                HRESULT result = videoframe->QueryInterface(IID_IDeckLinkVideoBuffer, (void**)&videoBuffer);
                if (result!=S_OK)
                    dlapierror(result, "failed to query video frame");

                /* the buffer has to be locked for reading before it can be addressed */
                result = videoBuffer->StartAccess(bmdBufferAccessRead);
                if (result!=S_OK)
                    dlapierror(result, "failed to lock frame buffer for reading");
                result = videoBuffer->GetBytes(&framedata);
                if (result!=S_OK)
                    dlapierror(result, "failed to access frame buffer address");
                int write = fwrite(framedata, framesize, 1, fileout);
                if (write!=1)
                    dlerror("failed to write %d bytes to output", framesize);

                /* release the buffer back to the driver */
                videoBuffer->EndAccess(bmdBufferAccessRead);
                videoBuffer->Release();
            }

            /* report frame */
            const BMDTimecodeFormat timecodeformat = bmdTimecodeRP188Any;
            //const BMDTimecodeFormat timecodeformat = bmdTimecodeVITC;
            //const BMDTimecodeFormat timecodeformat = bmdTimecodeSerial;
            const char *timecodeString = NULL;
            IDeckLinkTimecode *timecode;
            if (videoframe->GetTimecode(timecodeformat, &timecode) == S_OK)
                timecode->GetString(&timecodeString);

            if (framecount && framecount%lroundf(inp_mode_framerate)==0)
                dlstatus("frame %d: captured %d/%d [%s]", framecount, numframes, maxframes, timecodeString? timecodeString : "no timecode");

            /* tidy up */
            if (timecodeString)
                free((void*)timecodeString);

            numframes++;
        }
        framecount++;

        if (maxframes>0 && numframes>=maxframes)
        {
            sem_post(&sem_done);
        }
    }

    /* handle audio frame */
    if (audioframe)
    {
    }

    return S_OK;
}

HRESULT DeckLinkCapture::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode, BMDDetectedVideoInputFormatFlags flags)
{
    /* record and report the new input format */
    ReportInputFormat(mode);

    /* signal that input format has been detected */
    sem_post(&sem_format);

    return S_OK;
}

void DeckLinkCapture::Wait()
{
    /* wait for the callback to signal capture is finished */
    sem_wait(&sem_done);
}

void DeckLinkCapture::Stop()
{
    /* stop the video input */
    if (input) {
        input->StopStreams();
        input->DisableVideoInput();
        //input->DisableAudioOutput();
    }
    sem_destroy(&sem_done);
    sem_destroy(&sem_format);
}

DeckLinkCapture::~DeckLinkCapture()
{
    /* tidy up */
    card->Release();
    iterator->Release();
}

void usage(int exitcode)
{
    fprintf(stderr, "%s: capture raw video\n", appname);
    fprintf(stderr, "usage: %s [options] [<file>] [<file>...]\n", appname);
    fprintf(stderr, "  -s, --format        : sdi input format\n");
    fprintf(stderr, "  -n, --numframes     : number of frames (default: all)\n");
    fprintf(stderr, "  -o, --output        : write output to file\n");
    fprintf(stderr, "  -q, --quiet         : decrease verbosity, can be used multiple times\n");
    fprintf(stderr, "  -v, --verbose       : increase verbosity, can be used multiple times\n");
    fprintf(stderr, "  --                  : disable argument processing\n");
    fprintf(stderr, "  -h, --help, --usage : print this usage message\n");
    exit(exitcode);
}

int main(int argc, char *argv[])
{
    FILE *fileout = stdout;
    char *filename[16] = {0};
    char *outfile = NULL;
    int numfiles = 0;

    /* command line defaults */
    char *format = NULL;
    int numframes = -1;
    int verbose = 0;

    /* input format variables */
    int width = 0;
    int height = 0;
    bool interlaced = false;
    float framerate = 0.0f;

    /* parse command line for options */
    while (1) {
        static struct option long_options[] = {
            {"format",    1, NULL, 'f'},
            {"numframes", 1, NULL, 'n'},
            {"output",    1, NULL, 'o'},
            {"quiet",     0, NULL, 'q'},
            {"verbose",   0, NULL, 'v'},
            {"usage",     0, NULL, 'h'},
            {"help",      0, NULL, 'h'},
            {NULL,        0, NULL,  0 }
        };

        int optchar = getopt_long(argc, argv, "s:n:o:qvh", long_options, NULL);
        if (optchar==-1)
            break;

        switch (optchar) {
            case 's':
                format = optarg;
                break;

            case 'n':
                numframes = parse_int_arg(optarg, 1, INT_MAX, "number of frames");
                break;

            case 'o':
                outfile = optarg;
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

    /* all non-options are input filenames */
    while (optind<argc) {
        if ((unsigned)numfiles < sizeof(filename)/sizeof(filename[0]))
            filename[numfiles++] = argv[optind++];
        else
            dlexit("more than %d input files", numfiles);
    }

    /* lookup format type */
    if (format || outfile) {
        if (divine_video_format(format? format : outfile, &width, &height, &interlaced, &framerate)<0)
            dlmessage("info: auto-detecting input format");
    } else
        dlmessage("info: auto-detecting input format");

    /* open outfile after all other error conditions */
    if (outfile) {
        fileout = fopen(outfile, "wb");
        if (fileout==NULL)
            dlerror("failed to open output file \"%s\"");
    }

    /* create callback object */
    class DeckLinkCapture capture;
    if (outfile)
        capture.SetOutputFile(fileout);
    if (numframes)
        capture.SetMaxframes(numframes);

    /* run the capture */
    capture.Init();
    if (width && height && framerate)
        capture.Start(width, height, interlaced, framerate);
    else
        capture.Start(); /* auto-detect format */
    capture.Wait();
    capture.Stop();

    /* report statistics */
    if (verbose>=0)
        dlmessage("captured %d frames", capture.GetNumCaptures());

    return 0;
}
