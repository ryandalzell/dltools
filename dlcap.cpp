/*
 * Description: capture raw video.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2011 4i2i Communications Ltd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <semaphore.h>

#include "DeckLinkAPI.h"

#include "dlutil.h"

const char *appname = "dlcap";

/* synchronisation semaphore */
sem_t sem;

class callback : public IDeckLinkInputCallback
{
    public:
        callback(IDeckLinkInput *input);
        //~callback();

        /* implementation of IDeckLinkVideoOutputCallback */
        /* IUnknown needs only a dummy implementation */
        virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }
        virtual ULONG STDMETHODCALLTYPE AddRef(void)  {return 1;}
        virtual ULONG STDMETHODCALLTYPE Release(void) {return 1;}

        virtual HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode*, BMDDetectedVideoInputFormatFlags);
        virtual HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*);

        /* configuration interface */
        void SetOutputFile(FILE *f) {fileout = f;}
        void SetMaxframes(int m) {maxframes = m;}

        /* status interface */
        int GetFrameCount() {return framecount;}

    private:
        FILE *fileout;
        int maxframes;
        int framecount;
};

callback::callback(IDeckLinkInput *input)
{
    if (input->SetCallback(this)!=S_OK)
        dlexit("%s: error: could not set video callback object");
    fileout = NULL;
    maxframes = 0;
    framecount = 0;
}

HRESULT callback::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoframe, IDeckLinkAudioInputPacket* audioframe)
{
    //void *audioFrameBytes;

    /* handle video frame */
    if (videoframe)
    {
        if (videoframe->GetFlags() & bmdFrameHasNoInputSource)
        {
            if (framecount%60==0)
                dlmessage("frame %d: no input signal detected", framecount);
        }
        else
        {
            /* write video frame to file */
            int framesize = videoframe->GetRowBytes() * videoframe->GetHeight();
            if (fileout) {
                void *framedata;
                videoframe->GetBytes(&framedata);
                fwrite(framedata, framesize, 1, fileout);
            }

            /* report frame */
            const BMDTimecodeFormat timecodeformat = bmdTimecodeRP188;
            //const BMDTimecodeFormat timecodeformat = bmdTimecodeVITC;
            //const BMDTimecodeFormat timecodeformat = bmdTimecodeSerial;
            const char *timecodeString = NULL;
            IDeckLinkTimecode *timecode;
            if (videoframe->GetTimecode(timecodeformat, &timecode) == S_OK)
                timecode->GetString(&timecodeString);

            dlmessage("frame %d: [%s] %li bytes", framecount, timecodeString? timecodeString : "no timecode", framesize);

            /* tidy up */
            if (timecodeString)
                free((void*)timecodeString);

        }
        framecount++;

        if (maxframes>0 && framecount>=maxframes)
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

HRESULT callback::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode, BMDDetectedVideoInputFormatFlags)
{
    return S_OK;
}

void usage(int exitcode)
{
    fprintf(stderr, "%s: capture raw video\n", appname);
    fprintf(stderr, "usage: %s [options] [<file>] [<file>...]\n", appname);
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

        int optchar = getopt_long(argc, argv, "n:o:qvu", long_options, NULL);
        if (optchar==-1)
            break;

        switch (optchar) {
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

    /* initialise the DeckLink API */
    IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
    if (iterator==NULL)
        dlexit("error: could not initialise, the DeckLink driver may not be installed");

    /* connect to the first card in the system */
    IDeckLink *card;
    HRESULT result = iterator->Next(&card);
    if (result!=S_OK)
        dlexit("error: no DeckLink cards found");

    /* print the model name of the DeckLink card */
    char *name = NULL;
    result = card->GetModelName((const char **) &name);
    if (result == S_OK) {
        dlmessage("info: found a %s", name);
        free(name);
    }

    /* obtain the video input interface */
    void *voidptr;
    if (card->QueryInterface(IID_IDeckLinkInput, &voidptr)!=S_OK)
        dlexit("error: could not obtain the video input interface");
    IDeckLinkInput *input = (IDeckLinkInput *)voidptr;

    /* get display mode iterator */
    IDeckLinkDisplayMode *mode;
    {
        IDeckLinkDisplayModeIterator *iterator;
        if (input->GetDisplayModeIterator(&iterator) != S_OK)
            dlerror("failed to get display mode iterator");

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
            dlmessage("info: video mode %s", name);
    }

    /* configure the video input */
    //BMDDisplayMode selectedDisplayMode = bmdModeHD1080i5994;
    BMDDisplayMode selectedDisplayMode = bmdModeNTSC;
    result = input->EnableVideoInput(selectedDisplayMode, bmdFormat8BitYUV, 0);
    if (result!=S_OK)
        dlexit("failed to configure video input, is card in use?");

    /* open outfile after all other error conditions */
    if (outfile) {
        fileout = fopen(outfile, "wb");
        if (fileout==NULL)
            dlerror("failed to open output file \"%s\"");
    }

    /* initialise the semaphore */
    sem_init(&sem, 0, 0);

    /* create callback object */
    class callback the_callback(input);
    if (outfile)
        the_callback.SetOutputFile(fileout);
    if (numframes)
        the_callback.SetMaxframes(numframes);

    /* start the video input */
    result = input->StartStreams();
    if (result==S_OK) {

        /* wait for the callback to signal capture is finished */
        sem_wait(&sem);

    }

    /* stop the video input */
    input->StopStreams();
    input->DisableVideoInput();
    //input->DisableAudioOutput();

    /* tidy up */
    card->Release();
    iterator->Release();
    sem_destroy(&sem);

    /* report statistics */
    if (verbose>=0)
        fprintf(stdout, "\ncaptured %d frames\n", the_callback.GetFrameCount());

    return 0;
}
