/*
 * Description: display a test card with optional text, timecode and tone.
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

/* output frames are reused in turn, an even number so each frame is always odd or always even */
const int RING_FRAMES = PREROLL_FRAMES+2;

/* bits of the overlays shown */
const int OVERLAY_TEXT = 1;
const int OVERLAY_MODE = 2;
const int OVERLAY_TIMECODE = 4;

/* test tone of middle c at the smpte rp 155 alignment level, kept at least 250ms ahead in 50ms blocks */
const int AUDIO_RATE = 48000;
const double TONE_FREQUENCY = 261.6255653;  /* 440*2^(-9/12) */
const double TONE_LEVEL = -20.0;            /* dBFS */
const unsigned AUDIO_BLOCK = 2400;
const unsigned AUDIO_TARGET = 12000;
const int TONE_RAMP = 240;                  /* samples to mute or unmute, 5ms */

/* state of the tone, which is computed from the sample number so a partial write needs no buffer */
typedef struct {
    long long next;         /* sample number of the next sample to schedule */
    double gain;            /* gain of the next sample, ramped towards target */
    double target;          /* 1 when playing, 0 when muted */
    bool underrun;          /* an underrun has been reported */
    int channels;           /* channels of audio output, each with the tone */
    int16_t *buf;           /* a block of samples for every channel */
} tone_t;

/* timecode of a frame */
typedef struct {
    int hh, mm, ss, ff;
} timecode_t;

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

/* convert a frame number to a timecode counting at the nominal rate, with drop frame numbering if required */
void frame_to_timecode(long long frame, int fps, bool dropframe, timecode_t *tc)
{
    if (dropframe) {
        /* frame numbers dropped at the start of each minute, except every tenth minute */
        const int drop = fps/15;
        const long long frames_per_10min = fps*600 - drop*9;
        const long long frames_per_min = fps*60 - drop;
        long long tens = frame / frames_per_10min;
        long long rem = frame % frames_per_10min;
        frame += drop*9*tens;
        if (rem > drop)
            frame += drop*((rem - drop) / frames_per_min);
    }
    tc->ff = frame % fps;
    tc->ss = frame / fps % 60;
    tc->mm = frame / (fps*60) % 60;
    tc->hh = frame / (fps*3600) % 24;
}

/* move a gain towards its target by the given number of samples of ramp */
double ramp_gain(double gain, double target, unsigned samples)
{
    double step = (double)samples/TONE_RAMP;
    if (gain<target)
        return mmin(gain+step, target);
    return mmax(gain-step, target);
}

/* fill a buffer with 16-bit samples of the tone in every channel, starting at sample number first */
void generate_tone(int16_t *buf, int channels, long long first, unsigned count, double gain, double target)
{
    const double amplitude = 32767.0 * pow(10.0, TONE_LEVEL/20.0);
    for (unsigned i=0; i<count; i++) {
        /* whole seconds and the remaining samples keep the phase precise over long runs */
        long long n = first + i;
        double cycles = fmod((n/AUDIO_RATE) * TONE_FREQUENCY, 1.0) + (n%AUDIO_RATE) * TONE_FREQUENCY / AUDIO_RATE;
        int16_t sample = lround(gain * amplitude * sin(2.0*M_PI*cycles));
        for (int c=0; c<channels; c++)
            buf[channels*i + c] = sample;
        gain = ramp_gain(gain, target, 1);
    }
}

/* schedule tone until the card has at least the target amount of audio buffered */
void top_up_audio(IDeckLinkOutput *output, tone_t *tone, bool started)
{
    uint32_t buffered;
    HRESULT result = output->GetBufferedAudioSampleFrameCount(&buffered);
    if (result!=S_OK)
        dlapierror(result, "error: failed to get audio buffer level");
    if (started && buffered==0 && !tone->underrun) {
        dlmessage("warning: audio buffer ran empty");
        tone->underrun = true;
    }

    while (buffered<AUDIO_TARGET) {
        generate_tone(tone->buf, tone->channels, tone->next, AUDIO_BLOCK, tone->gain, tone->target);
        uint32_t written;
        result = output->ScheduleAudioSamples(tone->buf, AUDIO_BLOCK, tone->next, AUDIO_RATE, &written);
        if (result!=S_OK)
            dlapierror(result, "error: failed to schedule audio samples");

        /* carry on from the first sample not written */
        tone->next += written;
        tone->gain = ramp_gain(tone->gain, tone->target, written);
        buffered += written;
        if (written<AUDIO_BLOCK)
            break;
    }
}

/* set the timecode in the ancillary data of a frame, following smpte st 12-2 for the choice of vitc1 and vitc2 */
void set_timecode(IDeckLinkMutableVideoFrame *frame, const timecode_t *tc, bool dropframe, bool progressive, bool highrate, bool sd)
{
    /* the frames field cannot exceed 30, so high frame rates count frame pairs */
    int ff = highrate? tc->ff/2 : tc->ff;
    bool odd = tc->ff & 1;
    BMDTimecodeFlags flags = dropframe? bmdTimecodeIsDropFrame : bmdTimecodeFlagDefault;

    /* standard definition has vitc in the vertical interval rather than rp188 */
    BMDTimecodeFormat vitc1 = sd? bmdTimecodeVITC : bmdTimecodeRP188VITC1;
    BMDTimecodeFormat vitc2 = sd? bmdTimecodeVITCField2 : bmdTimecodeRP188VITC2;

    /* an interlaced or psf frame has both, a high frame rate uses vitc1 for even frames and vitc2 for odd */
    bool set1 = !progressive || !highrate || !odd;
    bool set2 = !progressive || (highrate && odd);

    HRESULT result;
    if (set1) {
        result = frame->SetTimecodeFromComponents(vitc1, tc->hh, tc->mm, tc->ss, ff, flags);
        if (result!=S_OK)
            dlapierror(result, "error: failed to set timecode");
    }
    if (set2) {
        result = frame->SetTimecodeFromComponents(vitc2, tc->hh, tc->mm, tc->ss, ff, flags | bmdTimecodeFieldMark);
        if (result!=S_OK)
            dlapierror(result, "error: failed to set timecode");
    }
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

/* draw white outlined text of the given pixel size, shrunk if needed to fit the frame, and above the avoid box if given */
void draw_text(unsigned char *uyvy, int width, int height, int rowbytes, FT_Face face, const char *text, int size, placement_t where, const int *avoid)
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
            if (avoid && x+box[0]<avoid[2] && x+box[2]>avoid[0] && y+box[3]>avoid[1])
                y = avoid[1] - height/40 - box[3];
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

/* prerendered glyphs of the characters of a timecode */
const char TIMECODE_CHARS[] = "0123456789:;";
typedef struct {
    FT_BitmapGlyph outline[sizeof(TIMECODE_CHARS)-1];
    FT_BitmapGlyph fill[sizeof(TIMECODE_CHARS)-1];
    int advance[sizeof(TIMECODE_CHARS)-1];
    int digit_cell, separator_cell;
    int x, y;           /* pen position of the first character */
    int box[4];         /* ink box of the timecode in the frame */
} tcfont_t;

/* render the timecode characters once and lay out a timecode at the bottom centre of the frame */
void tcfont_init(tcfont_t *tc, FT_Face face, int size, int width, int height)
{
    FT_Stroker stroker;
    if (FT_Stroker_New(face->glyph->library, &stroker))
        dlexit("error: failed to create font stroker");
    FT_Set_Pixel_Sizes(face, 0, size);
    FT_Stroker_Set(stroker, mmax(64, size*64/24), FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0);

    int top = 0, bottom = 0;
    tc->digit_cell = tc->separator_cell = 0;
    for (int i=0; TIMECODE_CHARS[i]; i++) {
        if (FT_Load_Char(face, TIMECODE_CHARS[i], FT_LOAD_DEFAULT))
            dlexit("error: failed to load glyph for '%c'", TIMECODE_CHARS[i]);
        FT_Glyph fill, outline;
        if (FT_Get_Glyph(face->glyph, &fill) || FT_Glyph_Copy(fill, &outline))
            dlexit("error: failed to get glyph for '%c'", TIMECODE_CHARS[i]);
        FT_Glyph_StrokeBorder(&outline, stroker, 0, 1);
        if (FT_Glyph_To_Bitmap(&fill, FT_RENDER_MODE_NORMAL, NULL, 1) || FT_Glyph_To_Bitmap(&outline, FT_RENDER_MODE_NORMAL, NULL, 1))
            dlexit("error: failed to render glyph for '%c'", TIMECODE_CHARS[i]);
        tc->fill[i] = (FT_BitmapGlyph)fill;
        tc->outline[i] = (FT_BitmapGlyph)outline;
        tc->advance[i] = face->glyph->advance.x >> 6;

        /* every digit gets the same cell so the timecode does not jitter */
        if (i<10)
            tc->digit_cell = mmax(tc->digit_cell, tc->advance[i]);
        else
            tc->separator_cell = mmax(tc->separator_cell, tc->advance[i]);
        top = mmin(top, -tc->outline[i]->top);
        bottom = mmax(bottom, (int)tc->outline[i]->bitmap.rows - tc->outline[i]->top);
    }
    FT_Stroker_Done(stroker);

    /* centre the width of hh:mm:ss:ff, with its bottom on the title safe margin */
    int w = 8*tc->digit_cell + 3*tc->separator_cell;
    tc->x = (width - w)/2;
    tc->y = height - height/20 - bottom;
    tc->box[0] = tc->x;
    tc->box[1] = tc->y + top;
    tc->box[2] = tc->x + w;
    tc->box[3] = tc->y + bottom;
}

void tcfont_done(tcfont_t *tc)
{
    for (int i=0; TIMECODE_CHARS[i]; i++) {
        FT_Done_Glyph((FT_Glyph)tc->fill[i]);
        FT_Done_Glyph((FT_Glyph)tc->outline[i]);
    }
}

/* draw a timecode string from the prerendered glyphs, outlines first then the fill */
void draw_timecode(unsigned char *uyvy, int width, int height, int rowbytes, const tcfont_t *tc, const char *string)
{
    for (int pass=0; pass<2; pass++) {
        int x = tc->x;
        for (const char *p=string; *p; p++) {
            const char *c = strchr(TIMECODE_CHARS, *p);
            if (c==NULL || *c==0)
                continue;
            int i = c - TIMECODE_CHARS;
            int cell = i<10? tc->digit_cell : tc->separator_cell;
            FT_BitmapGlyph glyph = pass? tc->fill[i] : tc->outline[i];
            blend_bitmap(uyvy, width, height, rowbytes, &glyph->bitmap, x + (cell-tc->advance[i])/2 + glyph->left, tc->y - glyph->top, pass? 235 : 16);
            x += cell;
        }
    }
}
#endif

void usage(int exitcode)
{
    fprintf(stderr, "%s: display a test card\n", appname);
    fprintf(stderr, "usage: %s [options]\n", appname);
    fprintf(stderr, "  -s, --sizeformat    : specify display size format: 480i,480p,576i,720p,1080i,1080p [optional +framerate] (default: 720p5994)\n");
    fprintf(stderr, "  -n, --numframes     : total number of frames to display (default: no limit)\n");
    fprintf(stderr, "  -l, --luma          : display luma only (default: luma and chroma)\n");
    fprintf(stderr, "  -o, --text          : display a string in the centre of the image, toggle with o (default: off, card name when toggled on)\n");
    fprintf(stderr, "  -m, --showmode      : display the video mode in the bottom right of the image, toggle with m (default: off)\n");
    fprintf(stderr, "  -t, --timecode      : add timecode to the sdi output and display it at the bottom of the image, toggle display with t (default: off)\n");
    fprintf(stderr, "  -a, --audio         : add a middle c tone at -20 dBFS on 8 channels, mute with a (default: off)\n");
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
    bool timecode = false;
    bool audio = false;
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
            {"timecode",  0, NULL, 't'},
            {"audio",     0, NULL, 'a'},
            {"index",     1, NULL, 'i'},
            {"quiet",     0, NULL, 'q'},
            {"verbose",   0, NULL, 'v'},
            {"usage",     0, NULL, 'h'},
            {"help",      0, NULL, 'h'},
            {NULL,        0, NULL,  0 }
        };

        int optchar = getopt_long(argc, argv, "s:n:lo:mtai:qvh", long_options, NULL);
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

            case 't':
                timecode = true;
                break;

            case 'a':
                audio = true;
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

    /* the 525 line display modes are 486 lines high */
    if (height==480)
        height = 486;

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

    /* timecode counts at the nominal frame rate, with drop frame numbering at 29.97 and 59.94 */
    const int fps = (framerate_scale + framerate_duration - 1) / framerate_duration;
    const bool fractional = framerate_scale % framerate_duration != 0;
    const bool dropframe = fractional && (fps==30 || fps==60);
    const bool progressive = mode->GetFieldDominance()==bmdProgressiveFrame;
    const bool highrate = fps>30;
    const bool sd = width<1280;

    /* create callback object */
    class callback the_callback;
    if (output->SetScheduledFrameCompletionCallback(&the_callback)!=S_OK)
        dlexit("error: could not set video callback object");

    /* set the video output mode, with timecode in the ancillary data if required */
    BMDVideoOutputFlags flags = bmdVideoOutputFlagDefault;
    if (timecode)
        flags = sd? bmdVideoOutputVITC : bmdVideoOutputRP188;
    result = output->EnableVideoOutput(mode->GetDisplayMode(), flags);
    if (result!=S_OK)
        dlapierror(result, "failed to enable video output");

    /* set the audio output mode and fill the audio buffer during preroll */
    tone_t tone = {0, 1.0, 1.0, false, 0, NULL};
    if (audio) {
        tone.channels = audio_channels(card, AUDIO_CHANNELS);
        tone.buf = (int16_t *)malloc(AUDIO_BLOCK*tone.channels*sizeof(int16_t));
        if (tone.buf==NULL)
            dlexit("error: failed to allocate audio buffer");
        result = output->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, tone.channels, bmdAudioOutputStreamTimestamped);
        if (result!=S_OK)
            dlapierror(result, "error: failed to enable audio output");
        result = output->BeginAudioPreroll();
        if (result!=S_OK)
            dlapierror(result, "error: failed to begin audio preroll");
    }

    /* one background for each combination of text and video mode overlays */
    int32_t rowbytes;
    result = output->RowBytesForPixelFormat(bmdFormat8BitYUV, width, &rowbytes);
    if (result!=S_OK)
        dlapierror(result, "error: failed to get row bytes for pixel format");
    const size_t size = rowbytes*height;
#ifdef HAVE_FREETYPE
    const int NUM_CARDS = 4;
    FT_Library library;
    if (FT_Init_FreeType(&library))
        dlexit("error: failed to initialise freetype");
    FT_Face face = open_font(library);

    /* the timecode is laid out first so the video mode can avoid it */
    tcfont_t tcfont;
    if (timecode)
        tcfont_init(&tcfont, face, height*10/100, width, height);
#else
    const int NUM_CARDS = 1;
#endif
    if (!text)
        text = cardname;

    /* draw the backgrounds */
    unsigned char *cards[NUM_CARDS];
    for (int i=0; i<NUM_CARDS; i++) {
        cards[i] = (unsigned char *)malloc(size);
        if (cards[i]==NULL)
            dlexit("error: failed to allocate test card");
        draw_bars(cards[i], width, height, rowbytes, lumaonly);
#ifdef HAVE_FREETYPE
        if ((i&OVERLAY_TEXT) && text)
            draw_text(cards[i], width, height, rowbytes, face, text, height*20/100, CENTRE, NULL);
        if ((i&OVERLAY_MODE) && modename)
            draw_text(cards[i], width, height, rowbytes, face, modename, height*5/100, BOTTOM_RIGHT, timecode? tcfont.box : NULL);
#endif
    }
#ifdef HAVE_FREETYPE
    FT_Done_Face(face);
#endif
    free((char *)modename);

    /* allocate the ring of output frames */
    class dlalloc alloc;
    alloc.init(size);
    IDeckLinkMutableVideoFrame *ring[RING_FRAMES];
    unsigned char *ringbuf[RING_FRAMES];
    for (int i=0; i<RING_FRAMES; i++) {
        IDeckLinkVideoBuffer *buffer;
        result = alloc.AllocateVideoBuffer(&buffer);
        if (result!=S_OK)
            dlapierror(result, "error: failed to allocate video buffer");
        result = output->CreateVideoFrameWithBuffer(width, height, rowbytes, bmdFormat8BitYUV, bmdFrameFlagDefault, buffer, &ring[i]);
        if (result!=S_OK)
            dlapierror(result, "error: failed to create video frame");
        result = buffer->GetBytes(&voidptr);
        if (result!=S_OK)
            dlapierror(result, "error: failed to get pointer to data in video frame");
        ringbuf[i] = (unsigned char *)voidptr;
    }

    /* the overlays shown at the start */
    int overlays = (text!=cardname? OVERLAY_TEXT : 0) | (showmode? OVERLAY_MODE : 0) | (timecode? OVERLAY_TIMECODE : 0);

    /* main loop, which prerolls frames without waiting until playback starts */
    sem_init(&sem, 0, 0);
    unsigned scheduled = 0;
    bool started = false;
    bool stopping = false;
    {
        class dlterm term;
        while (!stopped) {

            /* check for user input */
            if (term.kbhit()) {
                int c = term.readchar();
                if (c=='q' || c=='Q' || c=='\n' || c==27)
                    break;

                /* toggle an overlay, which shows once the frames already scheduled have played */
                if (c=='o' && NUM_CARDS>1)
                    overlays ^= OVERLAY_TEXT;
                if (c=='m' && NUM_CARDS>1)
                    overlays ^= OVERLAY_MODE;
                if (c=='t' && NUM_CARDS>1 && timecode)
                    overlays ^= OVERLAY_TIMECODE;

                /* mute or unmute the tone, which is heard once the audio already scheduled has played */
                if (c=='a' && audio)
                    tone.target = 1.0 - tone.target;
            }

            /* the last frame is held on output and never completes, so stop playback at its end */
            if (started && numframes && scheduled==numframes && !stopping) {
                result = output->StopScheduledPlayback(scheduled*duration, NULL, 180000);
                if (result!=S_OK)
                    dlapierror(result, "error: failed to stop video playback");
                stopping = true;
            }

            /* wait for a frame to complete, which frees the oldest frame in the ring */
            if (started)
                sem_wait(&sem);

            /* build and schedule the next frame */
            if (!numframes || scheduled<numframes) {
                IDeckLinkMutableVideoFrame *frame = ring[scheduled % RING_FRAMES];
                unsigned char *uyvy = ringbuf[scheduled % RING_FRAMES];
                memcpy(uyvy, cards[overlays & (NUM_CARDS-1)], size);
                if (timecode) {
                    timecode_t tc;
                    frame_to_timecode(scheduled, fps, dropframe, &tc);
                    set_timecode(frame, &tc, dropframe, progressive, highrate, sd);
#ifdef HAVE_FREETYPE
                    if (overlays & OVERLAY_TIMECODE) {
                        char string[16];
                        snprintf(string, sizeof(string), "%02d:%02d:%02d%c%02d", tc.hh, tc.mm, tc.ss, dropframe? ';' : ':', highrate? tc.ff/2 : tc.ff);
                        draw_timecode(uyvy, width, height, rowbytes, &tcfont, string);
                    }
#endif
                }
                result = output->ScheduleVideoFrame(frame, scheduled*duration, duration, 180000);
                if (result!=S_OK)
                    dlapierror(result, "error: failed to schedule video frame");
                scheduled++;
            }

            /* keep the audio buffer filled, audio is timed in samples rather than 180kHz so every sample time is exact,
               and after the stop is requested it is only needed up to the end of the last frame, the buffer
               level being unavailable once playback has stopped */
            if (audio && (!stopping || tone.next < scheduled*duration*AUDIO_RATE/180000))
                top_up_audio(output, &tone, started);

            /* start playback once prerolled */
            if (!started && (scheduled==PREROLL_FRAMES || scheduled==numframes)) {
                if (audio) {
                    result = output->EndAudioPreroll();
                    if (result!=S_OK)
                        dlapierror(result, "error: failed to end audio preroll");
                }
                result = output->StartScheduledPlayback(0, 180000, 1.0);
                if (result!=S_OK)
                    dlapierror(result, "error: failed to start video playback");
                started = true;

                if (verbose>=0) {
                    if (NUM_CARDS==1)
                        dlmessage("press q to exit%s", audio? ", a to mute tone" : "");
                    else
                        dlmessage("press q to exit, o to toggle text, m to toggle video mode%s%s", timecode? ", t to toggle timecode" : "", audio? ", a to mute tone" : "");
                }
            }
        }
    }

    /* stop the video output */
    output->StopScheduledPlayback(0, NULL, 0);
    output->DisableVideoOutput();
    if (audio)
        output->DisableAudioOutput();

    /* report statistics, after disabling the output completes the last frame */
    if (verbose>=1)
        dlmessage("info: displayed %d frames, %d late, %d dropped", completed, late, dropped);
    if (verbose>=1 && audio)
        dlmessage("info: audio: %lld samples scheduled on %d channels", tone.next, tone.channels);

    /* tidy up */
    for (int i=0; i<RING_FRAMES; i++)
        ring[i]->Release();
    for (int i=0; i<NUM_CARDS; i++)
        free(cards[i]);
    free(tone.buf);
#ifdef HAVE_FREETYPE
    if (timecode)
        tcfont_done(&tcfont);
    FT_Done_FreeType(library);
#endif
    free(cardname);
    mode->Release();
    config->Release();
    output->Release();
    card->Release();
    iterator->Release();
    sem_destroy(&sem);

    return 0;
}
