/*
 * Description: display a static test card.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2026 4i2i Communications Ltd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <limits.h>
#include <semaphore.h>
#include <math.h>

#include "DeckLinkAPI.h"

#include "dlutil.h"
#include "dlterm.h"
#include "dlalloc.h"

#ifdef HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_STROKER_H
#include <fontconfig/fontconfig.h>
#endif

const char *appname = "dlcard";

/* number of frames scheduled ahead of playback */
const int PREROLL_FRAMES = 8;

/* synchronisation with the frame completion callback */
sem_t sem;
unsigned int completed;
unsigned int late, dropped;
bool stopped;

class callback : public IDeckLinkVideoOutputCallback
{
public:
    /* IUnknown needs only a dummy implementation */
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) {(void)iid; (void)ppv; return E_NOINTERFACE;}
    virtual ULONG STDMETHODCALLTYPE   AddRef()  {return 1;}
    virtual ULONG STDMETHODCALLTYPE   Release() {return 1;}

    virtual HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame* frame, BMDOutputFrameCompletionResult result);
    virtual HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped();
};

HRESULT callback::ScheduledFrameCompleted(IDeckLinkVideoFrame* frame, BMDOutputFrameCompletionResult result)
{
    (void) frame;
    switch (result) {
        case bmdOutputFrameDisplayedLate: late++; break;
        case bmdOutputFrameDropped: dropped++; break;
        default: break;
    }

    /* when a video frame has been completed, post a semaphore */
    if (result!=bmdOutputFrameFlushed) {
        completed++;
        sem_post(&sem);
    }

    return S_OK;
}

HRESULT callback::ScheduledPlaybackHasStopped()
{
    /* wake the main loop so it can exit */
    stopped = true;
    sem_post(&sem);

    return S_OK;
}

/* draw 75% colour bars into a uyvy buffer */
void draw_bars(unsigned char *uyvy, int width, int height, int rowbytes, bool lumaonly)
{
    /* white, yellow, cyan, green, magenta, red, blue, black */
    const int rgb[8][3] = {{1,1,1}, {1,1,0}, {0,1,1}, {0,1,0}, {1,0,1}, {1,0,0}, {0,0,1}, {0,0,0}};

    /* bt.709 for hd, bt.601 for sd */
    const double kr = width>=1280? 0.2126 : 0.299;
    const double kb = width>=1280? 0.0722 : 0.114;

    /* convert each bar to 8-bit limited range y'cbcr */
    unsigned char bar[8][4];
    for (int i=0; i<8; i++) {
        double r = 0.75*rgb[i][0], g = 0.75*rgb[i][1], b = 0.75*rgb[i][2];
        double y = kr*r + (1.0-kr-kb)*g + kb*b;
        int Y  = lround(16.0 + 219.0*y);
        int Cb = lumaonly? 128 : lround(128.0 + 224.0*(b-y)/(2.0*(1.0-kb)));
        int Cr = lumaonly? 128 : lround(128.0 + 224.0*(r-y)/(2.0*(1.0-kr)));
        bar[i][0] = Cb;
        bar[i][1] = Y;
        bar[i][2] = Cr;
        bar[i][3] = Y;
    }

    /* draw the first line, a pair of pixels at a time so each pair is one colour */
    for (int x=0; x<width; x+=2)
        memcpy(uyvy + x*2, bar[(x*8)/width], 4);

    /* copy it to the rest of the frame */
    for (int y=1; y<height; y++)
        memcpy(uyvy + y*rowbytes, uyvy, width*2);
}

#ifdef HAVE_FREETYPE
/* where to put a line of text */
typedef enum {
    CENTRE,
    BOTTOM_RIGHT
} placement_t;

/* decode the next character of a utf-8 string, a byte which is not utf-8 is taken as latin-1 */
unsigned long utf8_next(const unsigned char **s)
{
    const unsigned char *p = *s;
    unsigned long c = *p++;
    int n = 0;
    if (c>=0xc0 && c<0xe0) {
        n = 1;
        c &= 0x1f;
    } else if (c>=0xe0 && c<0xf0) {
        n = 2;
        c &= 0x0f;
    } else if (c>=0xf0 && c<0xf8) {
        n = 3;
        c &= 0x07;
    }
    for (int i=0; i<n; i++) {
        if ((p[i]&0xc0)!=0x80) {
            *s += 1;
            return (*s)[-1];
        }
        c = (c<<6) | (p[i]&0x3f);
    }
    *s = p+n;
    return c;
}

/* find a bold sans serif font with fontconfig and load it */
FT_Face open_font(FT_Library library)
{
    FcConfig *config = FcInitLoadConfigAndFonts();
    FcPattern *pattern = FcNameParse((const FcChar8 *)"sans:bold");
    FcConfigSubstitute(config, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern *match = FcFontMatch(config, pattern, &result);
    FcChar8 *file;
    if (match==NULL || FcPatternGetString(match, FC_FILE, 0, &file)!=FcResultMatch)
        dlexit("error: failed to find a font for text");
    int index = 0;
    FcPatternGetInteger(match, FC_INDEX, 0, &index);

    FT_Face face;
    if (FT_New_Face(library, (const char *)file, index, &face))
        dlexit("error: failed to load font %s", file);

    FcPatternDestroy(match);
    FcPatternDestroy(pattern);
    FcConfigDestroy(config);
    return face;
}

/* blend a glyph bitmap into a uyvy buffer with the given luma, taking the chroma towards neutral */
void blend_bitmap(unsigned char *uyvy, int width, int height, int rowbytes, const FT_Bitmap *bitmap, int left, int top, int luma)
{
    for (unsigned r=0; r<bitmap->rows; r++) {
        int y = top + r;
        if (y<0 || y>=height)
            continue;
        unsigned char *line = uyvy + y*rowbytes;
        const unsigned char *src = bitmap->buffer + r*bitmap->pitch;
        for (unsigned c=0; c<bitmap->width; c++) {
            int x = left + c;
            int a = src[c];
            if (x<0 || x>=width || a==0)
                continue;
            /* the chroma of a pixel pair is blended once for each pixel */
            unsigned char *pair = line + (x&~1)*2;
            pair[0] += ((128-pair[0])*a)/255;
            pair[2] += ((128-pair[2])*a)/255;
            pair[(x&1)*2+1] += ((luma-pair[(x&1)*2+1])*a)/255;
        }
    }
}

/* render a string with its pen at x,y, or only measure its ink box if uyvy is null, the stroker draws an outline */
void render_text(FT_Face face, FT_Stroker stroker, const char *text, int x, int y, unsigned char *uyvy, int width, int height, int rowbytes, int luma, int box[4])
{
    const unsigned char *p = (const unsigned char *)text;
    FT_Pos pen = 0;
    FT_UInt previous = 0;
    if (box) {
        box[0] = box[1] = INT_MAX;
        box[2] = box[3] = INT_MIN;
    }
    while (*p) {
        FT_UInt glyph_index = FT_Get_Char_Index(face, utf8_next(&p));
        if (FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT))
            continue;

        /* apply kerning between this glyph and the last */
        if (previous && FT_HAS_KERNING(face)) {
            FT_Vector delta;
            FT_Get_Kerning(face, previous, glyph_index, FT_KERNING_DEFAULT, &delta);
            pen += delta.x;
        }
        previous = glyph_index;

        /* render the glyph, or its outline */
        FT_Glyph glyph;
        if (FT_Get_Glyph(face->glyph, &glyph))
            continue;
        if (stroker)
            FT_Glyph_StrokeBorder(&glyph, stroker, 0, 1);
        if (FT_Glyph_To_Bitmap(&glyph, FT_RENDER_MODE_NORMAL, NULL, 1)==0) {
            FT_BitmapGlyph bitmap = (FT_BitmapGlyph)glyph;
            int left = x + (pen>>6) + bitmap->left;
            int top = y - bitmap->top;
            if (box && bitmap->bitmap.width && bitmap->bitmap.rows) {
                box[0] = mmin(box[0], left);
                box[1] = mmin(box[1], top);
                box[2] = mmax(box[2], left + (int)bitmap->bitmap.width);
                box[3] = mmax(box[3], top + (int)bitmap->bitmap.rows);
            }
            if (uyvy)
                blend_bitmap(uyvy, width, height, rowbytes, &bitmap->bitmap, left, top, luma);
        }
        FT_Done_Glyph(glyph);

        pen += face->glyph->advance.x;
    }
}

/* draw white outlined text of the given pixel size, shrunk if needed to fit the frame */
void draw_text(unsigned char *uyvy, int width, int height, int rowbytes, FT_Face face, const char *text, int size, placement_t where)
{
    FT_Stroker stroker;
    if (FT_Stroker_New(face->glyph->library, &stroker))
        dlexit("error: failed to create font stroker");

    /* measure the outlined text, reducing the size until it fits in 90% of the width */
    int box[4];
    while (1) {
        FT_Set_Pixel_Sizes(face, 0, size);
        FT_Stroker_Set(stroker, mmax(64, size*64/24), FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);
        render_text(face, stroker, text, 0, 0, NULL, width, height, rowbytes, 0, box);
        if (box[2]<box[0] || box[2]-box[0]<=width*9/10 || size<=8)
            break;
        size = mmin(size-1, size*(width*9/10)/(box[2]-box[0]));
    }
    if (box[2]<box[0]) {
        /* nothing to draw */
        FT_Stroker_Done(stroker);
        return;
    }

    /* position the pen so the ink box lands in place, with a title safe margin in the corner */
    int x, y;
    switch (where) {
        case BOTTOM_RIGHT:
            x = width - width/20 - box[2];
            y = height - height/20 - box[3];
            break;
        default:
            x = (width - (box[2]-box[0]))/2 - box[0];
            y = (height - (box[3]-box[1]))/2 - box[1];
            break;
    }

    /* black outline first, then the white text over it */
    render_text(face, stroker, text, x, y, uyvy, width, height, rowbytes, 16, NULL);
    render_text(face, NULL, text, x, y, uyvy, width, height, rowbytes, 235, NULL);

    FT_Stroker_Done(stroker);
}
#endif

void usage(int exitcode)
{
    fprintf(stderr, "%s: display a static test card\n", appname);
    fprintf(stderr, "usage: %s [options]\n", appname);
    fprintf(stderr, "  -s, --sizeformat    : specify display size format: 480i,480p,576i,720p,1080i,1080p [optional +framerate] (default: 720p5994)\n");
    fprintf(stderr, "  -n, --numframes     : total number of frames to display (default: no limit)\n");
    fprintf(stderr, "  -l, --luma          : display luma only (default: luma and chroma)\n");
    fprintf(stderr, "  -o, --text          : display a string in the centre of the image, toggle with o (default: off, card name when toggled on)\n");
    fprintf(stderr, "  -m, --showmode      : display the video mode in the bottom right of the image, toggle with m (default: off)\n");
    fprintf(stderr, "  -i, --index         : index of decklink card to use (default: 0)\n");
    fprintf(stderr, "  -q, --quiet         : decrease verbosity, can be used multiple times\n");
    fprintf(stderr, "  -v, --verbose       : increase verbosity, can be used multiple times\n");
    fprintf(stderr, "  --                  : disable argument processing\n");
    fprintf(stderr, "  -h, --help, --usage : print this usage message\n");
    exit(exitcode);
}

int main(int argc, char *argv[])
{
    /* command line defaults */
    const char *sizeformat = "720p5994";
    unsigned numframes = 0;
    bool lumaonly = false;
    const char *text = NULL;
    bool showmode = false;
    int index = 0;
    int verbose = 0;

    /* parse command line for options */
    while (1) {
        static struct option long_options[] = {
            {"sizeformat",1, NULL, 's'},
            {"numframes", 1, NULL, 'n'},
            {"luma",      0, NULL, 'l'},
            {"text",      1, NULL, 'o'},
            {"showmode",  0, NULL, 'm'},
            {"index",     1, NULL, 'i'},
            {"quiet",     0, NULL, 'q'},
            {"verbose",   0, NULL, 'v'},
            {"usage",     0, NULL, 'h'},
            {"help",      0, NULL, 'h'},
            {NULL,        0, NULL,  0 }
        };

        int optchar = getopt_long(argc, argv, "s:n:lo:mi:qvh", long_options, NULL);
        if (optchar==-1)
            break;

        switch (optchar) {
            case 's':
                sizeformat = optarg;
                break;

            case 'n':
                numframes = parse_int_arg(optarg, 1, INT_MAX, "number of frames");
                break;

            case 'l':
                lumaonly = true;
                break;

            case 'o':
                text = optarg;
                break;

            case 'm':
                showmode = true;
                break;

            case 'i':
                index = parse_int_arg(optarg, 0, INT_MAX, "card index");
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

    /* no non-option arguments */
    if (optind<argc)
        usage(1);

#ifndef HAVE_FREETYPE
    if (text || showmode)
        dlexit("no support for text in this build");
#endif

    /* lookup the display format */
    int width, height;
    bool interlaced;
    float framerate;
    if (divine_video_format(sizeformat, &width, &height, &interlaced, &framerate)<0)
        dlexit("failed to determine output video format: %s", sizeformat);

    /* initialise the DeckLink API */
    IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
    if (iterator==NULL)
        dlexit("error: could not initialise, the DeckLink driver may not be installed");

    /* connect to the requested card */
    IDeckLink *card;
    for (int i=0; i<=index; i++) {
        if (iterator->Next(&card)!=S_OK)
            dlexit("error: failed to find DeckLink card with index %d", i);
        if (i<index)
            card->Release();
    }

    /* print the model name of the DeckLink card, kept as the default text */
    char *cardname = NULL;
    HRESULT result = card->GetModelName((const char **) &cardname);
    if (result == S_OK)
        dlmessage("info: found a %s", cardname);

    /* obtain the video output interface */
    void *voidptr;
    if (card->QueryInterface(IID_IDeckLinkOutput, &voidptr)!=S_OK)
        dlexit("error: could not obtain the video output interface");
    IDeckLinkOutput *output = (IDeckLinkOutput *)voidptr;

    /* configure the card to output on SDI single-link, smpte level A for 3G and no PsF */
    if (card->QueryInterface(IID_IDeckLinkConfiguration, &voidptr)!=S_OK)
        dlexit("error: could not obtain the configuration interface");
    IDeckLinkConfiguration *config = (IDeckLinkConfiguration *)voidptr;
    if (config->SetInt(bmdDeckLinkConfigVideoOutputConnection, bmdVideoConnectionSDI)!=S_OK)
        dlmessage("warning: failed to set card configuration to output SDI");
    if (config->SetInt(bmdDeckLinkConfigSDIOutputLinkConfiguration, bmdLinkConfigurationSingleLink)!=S_OK)
        dlmessage("warning: failed to set card configuration to single link SDI");
    if (config->SetFlag(bmdDeckLinkConfigSMPTELevelAOutput, true)!=S_OK)
        dlmessage("warning: failed to set card configuration to SMPTE A");
    if (config->SetFlag(bmdDeckLinkConfigOutput1080pAsPsF, false)!=S_OK)
        dlmessage("warning: failed to set card configuration to not use PsF");

    /* find the display mode for the format */
    IDeckLinkDisplayModeIterator *modes;
    if (output->GetDisplayModeIterator(&modes)!=S_OK)
        dlexit("error: failed to get display mode iterator");
    IDeckLinkDisplayMode *mode;
    BMDTimeValue framerate_duration;
    BMDTimeScale framerate_scale;
    while (modes->Next(&mode)==S_OK) {
        if (mode->GetWidth()==width && mode->GetHeight()==height)
            if ((mode->GetFieldDominance()==bmdProgressiveFrame) ^ interlaced) {
                /* look for an integer frame rate match */
                mode->GetFrameRate(&framerate_duration, &framerate_scale);
                if ((framerate_scale / framerate_duration)==(int)floor(framerate))
                    break;
            }
        mode->Release();
    }
    modes->Release();
    if (mode==NULL)
        dlexit("error: failed to find mode for %s", sizeformat);

    /* display mode name */
    const char *modename = NULL;
    if (mode->GetName(&modename)==S_OK)
        dlmessage("info: video mode %s", modename);

    /* frame duration in 180kHz */
    sts_t duration = llround(180000.0 * framerate_duration / framerate_scale);

    /* create callback object */
    class callback the_callback;
    if (output->SetScheduledFrameCompletionCallback(&the_callback)!=S_OK)
        dlexit("error: could not set video callback object");

    /* set the video output mode */
    result = output->EnableVideoOutput(mode->GetDisplayMode(), bmdVideoOutputFlagDefault);
    if (result!=S_OK)
        dlapierror(result, "failed to enable video output");

    /* one frame for each combination of overlays, bit 0 is the centre text and bit 1 the video mode */
#ifdef HAVE_FREETYPE
    const int NUM_CARDS = 4;
    FT_Library library;
    if (FT_Init_FreeType(&library))
        dlexit("error: failed to initialise freetype");
    FT_Face face = open_font(library);
#else
    const int NUM_CARDS = 1;
#endif
    if (!text)
        text = cardname;

    /* allocate and draw the frames of the test card */
    class dlalloc alloc;
    int32_t rowbytes;
    result = output->RowBytesForPixelFormat(bmdFormat8BitYUV, width, &rowbytes);
    if (result!=S_OK)
        dlapierror(result, "error: failed to get row bytes for pixel format");
    alloc.init(rowbytes*height);
    IDeckLinkMutableVideoFrame *frames[NUM_CARDS];
    for (int i=0; i<NUM_CARDS; i++) {
        IDeckLinkVideoBuffer *buffer;
        result = alloc.AllocateVideoBuffer(&buffer);
        if (result!=S_OK)
            dlapierror(result, "error: failed to allocate video buffer");
        result = output->CreateVideoFrameWithBuffer(width, height, rowbytes, bmdFormat8BitYUV, bmdFrameFlagDefault, buffer, &frames[i]);
        if (result!=S_OK)
            dlapierror(result, "error: failed to create video frame");
        result = buffer->GetBytes(&voidptr);
        if (result!=S_OK)
            dlapierror(result, "error: failed to get pointer to data in video frame");
        unsigned char *uyvy = (unsigned char *)voidptr;

        draw_bars(uyvy, width, height, rowbytes, lumaonly);
#ifdef HAVE_FREETYPE
        if ((i&1) && text)
            draw_text(uyvy, width, height, rowbytes, face, text, height*20/100, CENTRE);
        if ((i&2) && modename)
            draw_text(uyvy, width, height, rowbytes, face, modename, height*5/100, BOTTOM_RIGHT);
#endif
    }
#ifdef HAVE_FREETYPE
    FT_Done_Face(face);
    FT_Done_FreeType(library);
#endif
    free((char *)modename);

    /* the overlays shown at the start */
    int overlays = (text!=cardname? 1 : 0) | (showmode? 2 : 0);

    /* preroll the same frame several times */
    sem_init(&sem, 0, 0);
    unsigned scheduled = 0;
    while (scheduled<PREROLL_FRAMES && (!numframes || scheduled<numframes)) {
        result = output->ScheduleVideoFrame(frames[overlays], scheduled*duration, duration, 180000);
        if (result!=S_OK)
            dlapierror(result, "error: failed to schedule video frame");
        scheduled++;
    }

    /* start playback */
    result = output->StartScheduledPlayback(0, 180000, 1.0);
    if (result!=S_OK)
        dlapierror(result, "error: failed to start video playback");

    if (verbose>=0)
        dlmessage(NUM_CARDS>1? "press q to exit, o to toggle text, m to toggle video mode" : "press q to exit");

    /* main loop */
    {
        class dlterm term;
        bool stopping = false;
        while (!stopped) {

            /* check for user input */
            if (term.kbhit()) {
                int c = term.readchar();
                if (c=='q' || c=='Q' || c=='\n' || c==27)
                    break;

                /* toggle an overlay, which shows once the frames already scheduled have played */
                if (c=='o' && NUM_CARDS>1)
                    overlays ^= 1;
                if (c=='m' && NUM_CARDS>1)
                    overlays ^= 2;
            }

            /* the last frame is held on output and never completes, so stop playback at its end */
            if (numframes && scheduled==numframes && !stopping) {
                result = output->StopScheduledPlayback(scheduled*duration, NULL, 180000);
                if (result!=S_OK)
                    dlapierror(result, "error: failed to stop video playback");
                stopping = true;
            }

            /* wait for a frame to complete */
            sem_wait(&sem);

            /* schedule the next frame */
            if (!numframes || scheduled<numframes) {
                result = output->ScheduleVideoFrame(frames[overlays], scheduled*duration, duration, 180000);
                if (result!=S_OK)
                    dlapierror(result, "error: failed to schedule video frame");
                scheduled++;
            }
        }
    }

    /* stop the video output */
    output->StopScheduledPlayback(0, NULL, 0);
    output->DisableVideoOutput();

    /* report statistics, after disabling the output completes the last frame */
    if (verbose>=1)
        dlmessage("info: displayed %d frames, %d late, %d dropped", completed, late, dropped);

    /* tidy up */
    for (int i=0; i<NUM_CARDS; i++)
        frames[i]->Release();
    free(cardname);
    mode->Release();
    config->Release();
    output->Release();
    card->Release();
    iterator->Release();
    sem_destroy(&sem);

    return 0;
}
