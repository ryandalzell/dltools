/*
 * Description: measure the latency and lipsync of a chain looped from the output back to the input.
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

const char *appname = "dlsync";

/* number of frames scheduled ahead of playback */
const int PREROLL_FRAMES = 8;

/* output frames are reused in turn, an even number so each frame is always odd or always even */
const int RING_FRAMES = PREROLL_FRAMES+2;

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
} tone_t;

/* timecode of a frame */
typedef struct {
    int hh, mm, ss, ff;
} timecode_t;

/* synchronisation with the frame completion and input callbacks */
sem_t sem;
unsigned int completed;
unsigned int late, dropped;
bool stopped;
bool modechanged;

/* the input's hardware reference clock minus the output stream time, measured by main and used by the input callback */
volatile sts_t clock_offset;
volatile bool clock_valid;

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

/* receives the frames which have been through the chain and compares their timecode with the output */
class capture : public IDeckLinkInputCallback
{
public:
    /* IUnknown needs only a dummy implementation */
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) {(void)iid; (void)ppv; return E_NOINTERFACE;}
    virtual ULONG STDMETHODCALLTYPE   AddRef()  {return 1;}
    virtual ULONG STDMETHODCALLTYPE   Release() {return 1;}

    virtual HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *newmode, BMDDetectedVideoInputFormatFlags flags);
    virtual HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame *videoframe, IDeckLinkAudioInputPacket *audiopacket);

    /* set up by main before the input is started */
    IDeckLinkOutput *output;
    BMDDisplayMode displaymode;
    sts_t duration;
    int fps;
    bool dropframe, highrate, sd;
    int verbose;
    sts_t calibration;      /* the card's own delay, subtracted from the latency */

    /* results, read by main after the input is stopped */
    unsigned nosignal;      /* frames without a signal */
    unsigned received;      /* frames with a signal */
    unsigned untimed;       /* frames with a signal and no timecode */
    unsigned measured;      /* frames with a latency */
    sts_t sum, min, max;    /* statistics of the latency */
    sts_t reported;         /* latency last reported with a message */
    char newmodename[64];   /* the mode the input changed to */
};

/* the frame number of a timecode, the inverse of frame_to_timecode */
long long timecode_to_frame(const timecode_t *tc, int fps, bool dropframe)
{
    long long minutes = tc->hh*60 + tc->mm;
    long long frame = (minutes*60 + tc->ss)*fps + tc->ff;
    if (dropframe)
        frame -= (fps/15) * (minutes - minutes/10);
    return frame;
}

/* number of frames in the 24 hours of timecode */
long long frames_per_day(int fps, bool dropframe)
{
    return fps*86400LL - (dropframe? (fps/15)*9*144 : 0);
}

HRESULT capture::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *newmode, BMDDetectedVideoInputFormatFlags flags)
{
    (void) flags;

    /* only a change of display mode matters, the pixel format is converted by the card */
    if (!(events & (bmdVideoInputDisplayModeChanged | bmdVideoInputFieldDominanceChanged)) || newmode->GetDisplayMode()==displaymode)
        return S_OK;

    /* a mode change in the chain is fatal, so wake the main loop to stop and exit */
    snprintf(newmodename, sizeof(newmodename), "%s", describe_display_mode(newmode));
    modechanged = true;
    sem_post(&sem);

    return S_OK;
}

HRESULT capture::VideoInputFrameArrived(IDeckLinkVideoInputFrame *videoframe, IDeckLinkAudioInputPacket *audiopacket)
{
    (void) audiopacket;
    if (videoframe==NULL)
        return S_OK;

    /* report a missing signal once a second */
    if (videoframe->GetFlags() & bmdFrameHasNoInputSource) {
        if (nosignal++ % fps==0)
            dlstatus("no input signal for %u frames", nosignal);
        return S_OK;
    }
    received++;

    /* the timecode as sent, a high frame rate frame is odd if it carries vitc2 rather than vitc1 */
    IDeckLinkTimecode *tc = NULL;
    bool odd = false;
    if (sd)
        videoframe->GetTimecode(bmdTimecodeVITC, &tc);
    else if (videoframe->GetTimecode(bmdTimecodeRP188VITC1, &tc)!=S_OK) {
        if (videoframe->GetTimecode(bmdTimecodeRP188VITC2, &tc)==S_OK)
            odd = true;
        else if (videoframe->GetTimecode(bmdTimecodeRP188Any, &tc)==S_OK)
            odd = (tc->GetFlags() & bmdTimecodeFieldMark)!=0;
    }
    if (tc==NULL) {
        untimed++;
        dlstatus("frame %u: no timecode", received);
        return S_OK;
    }
    uint8_t hh, mm, ss, ff;
    HRESULT result = tc->GetComponents(&hh, &mm, &ss, &ff);
    tc->Release();
    if (result!=S_OK) {
        untimed++;
        dlstatus("frame %u: unreadable timecode", received);
        return S_OK;
    }
    timecode_t t = {hh, mm, ss, highrate? ff*2 + odd : ff};

    /* the arrival time on the input's clock, which needs the output stream time related to that clock */
    if (!clock_valid)
        return S_OK;
    BMDTimeValue arrival, frameduration;
    result = videoframe->GetHardwareReferenceTimestamp(180000, &arrival, &frameduration);
    if (result!=S_OK) {
        dlstatus("frame %u: no hardware timestamp", received);
        return S_OK;
    }
    const sts_t offset = clock_offset;

    /* the received frame number, unwrapped to the day of timecode nearest the output frame at arrival */
    const long long day = frames_per_day(fps, dropframe);
    long long now = (arrival - offset) / duration;
    long long frame = timecode_to_frame(&t, fps, dropframe);
    frame += day * llround((double)(now - frame) / day);

    /* the latency from the time the frame was sent, less the card's own delay */
    sts_t latency = arrival - (frame*duration + offset) - calibration;
    if (!measured || latency<min)
        min = latency;
    if (!measured || latency>max)
        max = latency;
    sum += latency;

    /* report a jump of more than half a frame with a message, and every frame on the status line */
    char string[16];
    snprintf(string, sizeof(string), "%02d:%02d:%02d%c%02d", t.hh, t.mm, t.ss, dropframe? ';' : ':', highrate? t.ff/2 : t.ff);
    if (!measured || llabs(latency-reported)>duration/2) {
        dlmessage("info: received %s%s as frame %lld, latency %.2f ms (%.2f frames)", string, highrate? (odd? ".1" : ".0") : "", frame, latency/180.0, (double)latency/duration);
        reported = latency;
    } else if (verbose>=0)
        dlstatus("received %s%s as frame %lld, latency %.2f ms (%.2f frames)", string, highrate? (odd? ".1" : ".0") : "", frame, latency/180.0, (double)latency/duration);
    measured++;

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

/* fill a buffer with stereo 16-bit samples of the tone, starting at sample number first */
void generate_tone(int16_t *buf, long long first, unsigned count, double gain, double target)
{
    const double amplitude = 32767.0 * pow(10.0, TONE_LEVEL/20.0);
    for (unsigned i=0; i<count; i++) {
        /* whole seconds and the remaining samples keep the phase precise over long runs */
        long long n = first + i;
        double cycles = fmod((n/AUDIO_RATE) * TONE_FREQUENCY, 1.0) + (n%AUDIO_RATE) * TONE_FREQUENCY / AUDIO_RATE;
        int16_t sample = lround(gain * amplitude * sin(2.0*M_PI*cycles));
        buf[2*i] = buf[2*i+1] = sample;
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
        int16_t buf[AUDIO_BLOCK*2];
        generate_tone(buf, tone->next, AUDIO_BLOCK, tone->gain, tone->target);
        uint32_t written;
        result = output->ScheduleAudioSamples(buf, AUDIO_BLOCK, tone->next, AUDIO_RATE, &written);
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

/* the default calibration file, in the home directory */
const char *default_calibration_file()
{
    static char filename[PATH_MAX];
    const char *home = getenv("HOME");
    snprintf(filename, sizeof(filename), "%s/.dlsync", home? home : ".");
    return filename;
}

/* read the card's own delay in a mode from the calibration file, returning false if there is none */
bool read_calibration(const char *filename, const char *modename, double *video_ms)
{
    FILE *file = fopen(filename, "r");
    if (file==NULL)
        return false;

    /* each line is a mode then its delays, a line starting # is a comment */
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), file)) {
        char mode[32];
        double ms;
        if (line[0]!='#' && sscanf(line, "%31s video %lf", mode, &ms)==2 && strcmp(mode, modename)==0) {
            *video_ms = ms;
            found = true;
        }
    }
    fclose(file);
    return found;
}

/* write the card's own delay in a mode to the calibration file, keeping the lines for other modes */
void write_calibration(const char *filename, const char *modename, double video_ms)
{
    /* keep the existing lines except the one for this mode */
    char *kept = NULL;
    size_t keptlen = 0;
    FILE *file = fopen(filename, "r");
    if (file) {
        char line[256];
        size_t len = strlen(modename);
        while (fgets(line, sizeof(line), file)) {
            if (line[0]=='#' || (strncmp(line, modename, len)==0 && line[len]==' '))
                continue;
            size_t n = strlen(line);
            kept = (char *)realloc(kept, keptlen+n+1);
            if (kept==NULL)
                dlexit("error: failed to allocate calibration file buffer");
            memcpy(kept+keptlen, line, n+1);
            keptlen += n;
        }
        fclose(file);
    }

    file = fopen(filename, "w");
    if (file==NULL)
        dlerror("error: failed to open calibration file \"%s\"", filename);
    fprintf(file, "# dlsync calibration: video mode, then the card's own delay from output to input in ms\n");
    if (kept)
        fputs(kept, file);
    fprintf(file, "%s video %.3f\n", modename, video_ms);
    if (fclose(file)!=0)
        dlerror("error: failed to write calibration file \"%s\"", filename);
    free(kept);
}

void usage(int exitcode)
{
    fprintf(stderr, "%s: measure the latency of a chain from the sdi output back to the sdi input\n", appname);
    fprintf(stderr, "usage: %s [options]\n", appname);
    fprintf(stderr, "  -s, --sizeformat    : specify display size format: 480i,480p,576i,720p,1080i,1080p [optional +framerate] (default: 720p5994)\n");
    fprintf(stderr, "  -c, --calibrate     : measure the card's own delay with a loopback cable and save it (default: subtract the saved delay)\n");
    fprintf(stderr, "  -f, --calfile       : calibration file (default: ~/.dlsync)\n");
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
    bool calibrate = false;
    const char *calfile = default_calibration_file();
    int index = 0;
    int verbose = 0;

    /* parse command line for options */
    while (1) {
        static struct option long_options[] = {
            {"sizeformat",1, NULL, 's'},
            {"calibrate", 0, NULL, 'c'},
            {"calfile",   1, NULL, 'f'},
            {"index",     1, NULL, 'i'},
            {"quiet",     0, NULL, 'q'},
            {"verbose",   0, NULL, 'v'},
            {"usage",     0, NULL, 'h'},
            {"help",      0, NULL, 'h'},
            {NULL,        0, NULL,  0 }
        };

        int optchar = getopt_long(argc, argv, "s:cf:i:qvh", long_options, NULL);
        if (optchar==-1)
            break;

        switch (optchar) {
            case 's':
                sizeformat = optarg;
                break;

            case 'c':
                calibrate = true;
                break;

            case 'f':
                calfile = optarg;
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

    /* print the model name of the DeckLink card */
    char *cardname = NULL;
    HRESULT result = card->GetModelName((const char **) &cardname);
    if (result == S_OK)
        dlmessage("info: found a %s", cardname);

    /* obtain the video output and input interfaces */
    void *voidptr;
    if (card->QueryInterface(IID_IDeckLinkOutput, &voidptr)!=S_OK)
        dlexit("error: could not obtain the video output interface");
    IDeckLinkOutput *output = (IDeckLinkOutput *)voidptr;
    if (card->QueryInterface(IID_IDeckLinkInput, &voidptr)!=S_OK)
        dlexit("error: could not obtain the video input interface");
    IDeckLinkInput *input = (IDeckLinkInput *)voidptr;

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

    /* capture from SDI, with the output kept independent of the input */
    if (config->SetInt(bmdDeckLinkConfigVideoInputConnection, bmdVideoConnectionSDI)!=S_OK)
        dlmessage("warning: failed to set card configuration to input SDI");
    if (config->SetInt(bmdDeckLinkConfigCapturePassThroughMode, bmdDeckLinkCapturePassthroughModeDisabled)!=S_OK)
        dlmessage("warning: failed to set card configuration to disable capture passthrough");

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
    free((char *)modename);

    /* the card's own delay in this mode, which calibration measures rather than subtracts */
    char calmode[32];
    snprintf(calmode, sizeof(calmode), "%s", describe_display_mode(mode));
    double calibration_ms = 0.0;
    if (calibrate)
        dlmessage("info: calibrating %s, the output must be looped straight back to the input", calmode);
    else if (read_calibration(calfile, calmode, &calibration_ms))
        dlmessage("info: subtracting the card's own delay of %.3f ms from %s", calibration_ms, calfile);
    else
        dlmessage("warning: no calibration for %s in %s, the latency includes the card's own delay", calmode, calfile);

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

    /* set the video output mode, with timecode in the ancillary data */
    result = output->EnableVideoOutput(mode->GetDisplayMode(), sd? bmdVideoOutputVITC : bmdVideoOutputRP188);
    if (result!=S_OK)
        dlapierror(result, "failed to enable video output");

    /* set the audio output mode and fill the audio buffer during preroll */
    tone_t tone = {0, 1.0, 1.0, false};
    result = output->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, 2, bmdAudioOutputStreamTimestamped);
    if (result!=S_OK)
        dlapierror(result, "error: failed to enable audio output");
    result = output->BeginAudioPreroll();
    if (result!=S_OK)
        dlapierror(result, "error: failed to begin audio preroll");

    /* set the video input to the same mode, detecting a change so it can be reported */
    class capture the_capture = {};
    the_capture.output = output;
    the_capture.displaymode = mode->GetDisplayMode();
    the_capture.duration = duration;
    the_capture.fps = fps;
    the_capture.dropframe = dropframe;
    the_capture.highrate = highrate;
    the_capture.sd = sd;
    the_capture.verbose = verbose;
    the_capture.calibration = llround(calibration_ms*180.0);
    if (input->SetCallback(&the_capture)!=S_OK)
        dlexit("error: could not set input callback object");
    result = input->EnableVideoInput(mode->GetDisplayMode(), bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection);
    if (result!=S_OK)
        dlapierror(result, "error: failed to enable video input");

    /* the background */
    int32_t rowbytes;
    result = output->RowBytesForPixelFormat(bmdFormat8BitYUV, width, &rowbytes);
    if (result!=S_OK)
        dlapierror(result, "error: failed to get row bytes for pixel format");
    const size_t size = rowbytes*height;
    unsigned char *background = (unsigned char *)malloc(size);
    if (background==NULL)
        dlexit("error: failed to allocate test card");
    draw_bars(background, width, height, rowbytes, false);
#ifdef HAVE_FREETYPE
    FT_Library library;
    if (FT_Init_FreeType(&library))
        dlexit("error: failed to initialise freetype");
    FT_Face face = open_font(library);
    tcfont_t tcfont;
    tcfont_init(&tcfont, face, height*10/100, width, height);
    FT_Done_Face(face);
#endif

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

    /* samples of the offset of the input's clock from the output stream time */
    sts_t best_offset = 0, best_spread = 0, offset_min = 0, offset_max = 0, spread_max = 0;
    int window = 0;
    unsigned clock_samples = 0;

    /* main loop, which prerolls frames without waiting until playback starts */
    sem_init(&sem, 0, 0);
    unsigned scheduled = 0;
    bool started = false;
    {
        class dlterm term;
        while (!stopped) {

            /* check for user input */
            if (term.kbhit()) {
                int c = term.readchar();
                if (c=='q' || c=='Q' || c=='\n' || c==27)
                    break;
            }

            /* wait for a frame to complete, which frees the oldest frame in the ring */
            if (started)
                sem_wait(&sem);
            if (modechanged)
                break;

            /* build and schedule the next frame */
            IDeckLinkMutableVideoFrame *frame = ring[scheduled % RING_FRAMES];
            unsigned char *uyvy = ringbuf[scheduled % RING_FRAMES];
            memcpy(uyvy, background, size);
            timecode_t tc;
            frame_to_timecode(scheduled, fps, dropframe, &tc);
            set_timecode(frame, &tc, dropframe, progressive, highrate, sd);
#ifdef HAVE_FREETYPE
            char string[16];
            snprintf(string, sizeof(string), "%02d:%02d:%02d%c%02d", tc.hh, tc.mm, tc.ss, dropframe? ';' : ':', highrate? tc.ff/2 : tc.ff);
            draw_timecode(uyvy, width, height, rowbytes, &tcfont, string);
#endif
            result = output->ScheduleVideoFrame(frame, scheduled*duration, duration, 180000);
            if (result!=S_OK)
                dlapierror(result, "error: failed to schedule video frame");
            scheduled++;

            /* keep the audio buffer filled, audio is timed in samples rather than 180kHz so every sample time is exact */
            top_up_audio(output, &tone, started);

            /* relate the stream time to the input's clock, which times the received frames, publishing the sample of
               each second whose clock reads were closest together so the offset follows any drift between the two */
            if (started) {
                BMDTimeValue hw1, hw2, streamtime, timeinframe, ticksperframe;
                double speed;
                if (input->GetHardwareReferenceClock(180000, &hw1, &timeinframe, &ticksperframe)==S_OK &&
                    output->GetScheduledStreamTime(180000, &streamtime, &speed)==S_OK &&
                    input->GetHardwareReferenceClock(180000, &hw2, &timeinframe, &ticksperframe)==S_OK && speed>0.0) {
                    sts_t offset = (hw1+hw2)/2 - streamtime;
                    sts_t spread = hw2 - hw1;
                    if (window==0 || spread<=best_spread) {
                        best_offset = offset;
                        best_spread = spread;
                    }
                    if (++window==fps) {
                        clock_offset = best_offset;
                        clock_valid = true;
                        window = 0;

                        /* the range of the published offsets shows any drift between the stream time and the clock */
                        offset_min = clock_samples? mmin(offset_min, best_offset) : best_offset;
                        offset_max = clock_samples? mmax(offset_max, best_offset) : best_offset;
                        spread_max = clock_samples? mmax(spread_max, best_spread) : best_spread;
                        clock_samples++;
                    }
                }
            }

            /* start playback once prerolled, then the input so every frame received was sent in this run */
            if (!started && scheduled==PREROLL_FRAMES) {
                result = output->EndAudioPreroll();
                if (result!=S_OK)
                    dlapierror(result, "error: failed to end audio preroll");
                result = output->StartScheduledPlayback(0, 180000, 1.0);
                if (result!=S_OK)
                    dlapierror(result, "error: failed to start video playback");
                result = input->StartStreams();
                if (result!=S_OK)
                    dlapierror(result, "error: failed to start video input");
                started = true;

                if (verbose>=0)
                    dlmessage(calibrate? "press q to exit calibration and save, when result looks stable" : "press q to exit");
            }
        }
    }

    /* stop the input, then the output */
    input->StopStreams();
    input->DisableVideoInput();
    output->StopScheduledPlayback(0, NULL, 0);
    output->DisableVideoOutput();
    output->DisableAudioOutput();

    if (modechanged)
        dlexit("error: input changed to %s, the chain must keep the video mode", the_capture.newmodename);

    /* report statistics, after disabling the output completes the last frame */
    if (verbose>=1)
        dlmessage("info: displayed %d frames, %d late, %d dropped", completed, late, dropped);
    if (verbose>=1)
        dlmessage("info: received %u frames, %u without timecode", the_capture.received, the_capture.untimed);
    if (verbose>=1 && clock_samples)
        dlmessage("info: over %u seconds the stream time moved %.3f ms against the input clock, each second read within %.3f ms", clock_samples, (offset_max-offset_min)/180.0, spread_max/180.0);
    if (the_capture.measured) {
        double mean = the_capture.sum / 180.0 / the_capture.measured;
        dlmessage("latency %.2f ms (%.2f frames), min %.2f ms, max %.2f ms, over %u frames", mean, mean*180.0/duration, the_capture.min/180.0, the_capture.max/180.0, the_capture.measured);

        /* save the card's own delay */
        if (calibrate) {
            if (mean*180.0 > 3*duration)
                dlmessage("warning: %.2f ms is long for a loopback cable, is a chain still connected?", mean);
            write_calibration(calfile, calmode, mean);
            dlmessage("saved the card's own delay in %s to %s", calmode, calfile);
        }
    } else
        dlmessage("no latency measured, %s", the_capture.received? "no timecode was received" : "no input signal was received");
    if (calibrate && !the_capture.measured)
        dlmessage("warning: nothing saved to %s", calfile);

    /* tidy up */
    for (int i=0; i<RING_FRAMES; i++)
        ring[i]->Release();
    free(background);
#ifdef HAVE_FREETYPE
    tcfont_done(&tcfont);
    FT_Done_FreeType(library);
#endif
    free(cardname);
    mode->Release();
    config->Release();
    input->Release();
    output->Release();
    card->Release();
    iterator->Release();
    sem_destroy(&sem);

    return 0;
}
