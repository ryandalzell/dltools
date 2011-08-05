/*
 * Description: capture raw video.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2011 4i2i Communications Ltd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
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
        HRESULT Start(int width, int height, bool interlaced, float framerate);
        void Wait();
        void Stop();

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
        BMDDisplayMode inp_mode;
        const char *inp_mode_name;
        float inp_mode_framerate;

        /* file output variables */
        FILE *fileout;
        int numframes;
        int maxframes;
        int framecount;

        /* synchronisation semaphore */
        sem_t sem;
};

DeckLinkCapture::DeckLinkCapture()
{
    iterator = NULL;
    card = NULL;
    input = NULL;
    mode = NULL;
    inp_mode = 0;
    inp_mode_name = NULL;
    inp_mode_framerate = 30.0;
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

HRESULT DeckLinkCapture::Start(int width, int height, bool interlaced, float framerate)
{
    /* get display mode iterator */
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
        const char *name;
        if (mode->GetName(&name)==S_OK)
            dlmessage("info: requested capture format %s", name);
    }

    /* configure the video input */
    HRESULT result = input->EnableVideoInput(mode->GetDisplayMode(), bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection);
    if (result!=S_OK)
        dlapierror(result, "failed to configure video input");

    /* initialise the semaphore */
    sem_init(&sem, 0, 0);

    /* start the video input */
    result = input->StartStreams();
    if (result!=S_OK) {
        sem_destroy(&sem);
        dlapierror(result, "failed to start video input");
    }

    return S_OK;
}

HRESULT DeckLinkCapture::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoframe, IDeckLinkAudioInputPacket* audioframe)
{
    //void *audioFrameBytes;

    /* handle video frame */
    if (videoframe)
    {
        if (videoframe->GetFlags() & bmdFrameHasNoInputSource)
        {
            if (framecount%lroundf(inp_mode_framerate)==0) {
                if (inp_mode_name == NULL)
                    dlstatus("frame %d: no input signal detected", framecount);
                else
                    dlstatus("frame %d: format mismatch: %s detected", framecount, inp_mode_name);
            }
        }
        else
        {
            /* write video frame to file */
            if (fileout) {
                int framesize = videoframe->GetRowBytes() * videoframe->GetHeight();
                void *framedata;
                videoframe->GetBytes(&framedata);
                int write = fwrite(framedata, framesize, 1, fileout);
                if (write!=1)
                    dlerror("failed to write %d bytes to output");
            }

            /* report frame */
            const BMDTimecodeFormat timecodeformat = bmdTimecodeRP188;
            //const BMDTimecodeFormat timecodeformat = bmdTimecodeVITC;
            //const BMDTimecodeFormat timecodeformat = bmdTimecodeSerial;
            const char *timecodeString = NULL;
            IDeckLinkTimecode *timecode;
            if (videoframe->GetTimecode(timecodeformat, &timecode) == S_OK)
                timecode->GetString(&timecodeString);

            if (framecount%lroundf(inp_mode_framerate)==0)
                dlstatus("frame %d: [%s]", framecount, timecodeString? timecodeString : "no timecode");

            /* tidy up */
            if (timecodeString)
                free((void*)timecodeString);

            numframes++;
        }
        framecount++;

        if (maxframes>0 && numframes>=maxframes)
        {
            sem_post(&sem);
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
    BMDTimeValue framerate_duration;
    BMDTimeScale framerate_scale;

    inp_mode = mode->GetDisplayMode();
    mode->GetName(&inp_mode_name);
    mode->GetFrameRate(&framerate_scale, &framerate_duration);
    inp_mode_framerate = (float)framerate_duration / (float) framerate_scale;

    return S_OK;
}

void DeckLinkCapture::Wait()
{
    /* wait for the callback to signal capture is finished */
    sem_wait(&sem);
}

void DeckLinkCapture::Stop()
{
    /* stop the video input */
    if (input) {
        input->StopStreams();
        input->DisableVideoInput();
        //input->DisableAudioOutput();
    }
    sem_destroy(&sem);
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
    fprintf(stderr, "  -u, --help, --usage : print this usage message\n");
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
    int width;
    int height;
    bool interlaced;
    float framerate;
    pixelformat_t pixelformat;

    /* parse command line for options */
    while (1) {
        static struct option long_options[] = {
            {"format",    1, NULL, 'f'},
            {"numframes", 1, NULL, 'n'},
            {"output",    1, NULL, 'o'},
            {"quiet",     0, NULL, 'q'},
            {"verbose",   0, NULL, 'v'},
            {"usage",     0, NULL, 'u'},
            {"help",      0, NULL, 'u'},
            {NULL,        0, NULL,  0 }
        };

        int optchar = getopt_long(argc, argv, "s:n:o:qvu", long_options, NULL);
        if (optchar==-1)
            break;

        switch (optchar) {
            case 's':
                format = optarg;
                break;

            case 'n':
                numframes = atoi(optarg);
                if (numframes<=0)
                    dlexit("invalid value for numframes: %d", numframes);
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
        if ((unsigned)numfiles < sizeof(filename)/sizeof(filename[0]))
            filename[numfiles++] = argv[optind++];
        else
            dlexit("more than %d input files", numfiles);
    }

    /* lookup format type */
    if (!format && !outfile)
        dlexit("need to specify the input sdi format type, either use the -s switch or specify it in the output filename");
    if (divine_video_format(format? format : outfile, &width, &height, &interlaced, &framerate, &pixelformat)<0)
        dlexit("format type unsupported: %s", format);

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
    capture.Start(width, height, interlaced, framerate);
    capture.Wait();
    capture.Stop();

    /* report statistics */
    if (verbose>=0)
        fprintf(stdout, "\ncaptured %d frames\n", capture.GetNumCaptures());

    return 0;
}
