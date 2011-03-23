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
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <math.h>
#include <inttypes.h>
#include <errno.h>

extern "C" {
    #include <mpeg2dec/mpeg2.h>
    #include <mpeg2dec/mpeg2convert.h>
    #include <a52dec/a52.h>
    #include <a52dec/mm_accel.h>
}
#include <mpg123.h>

#include "DeckLinkAPI.h"

#include "dlutil.h"
#include "dlterm.h"

const char *appname = "dlplay";

/* semaphores for synchronisation */
sem_t sem;
int exit_thread;

/* display statistics */
unsigned int completed;
unsigned int late, dropped, flushed;

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

HRESULT callback::RenderAudioSamples(bool preroll)
{
    return S_OK;
}

/*****************************************/

void usage(int exitcode)
{
    fprintf(stderr, "%s: play raw video files\n", appname);
    fprintf(stderr, "usage: %s [options] <file> [<file>...]\n", appname);
    fprintf(stderr, "  -f, --format        : specify display format: 480i,480p,576i,720p,1080i,1080p [optional +framerate] (default: auto)\n");
    fprintf(stderr, "  -a, --firstframe    : index of first frame in input to display (default: 0)\n");
    fprintf(stderr, "  -n, --numframes     : number of frames in input to display (default: all)\n");
    fprintf(stderr, "  -m, --ntscmode      : use proper ntsc framerate, e.g. 29.97 instead of 30fps (default: on)\n");
    fprintf(stderr, "  -l, --luma          : display luma plane only (default: luma and chroma)\n");
    fprintf(stderr, "  -p, --video-pid     : decode specific video pid from transport stream (default: first program)\n");
    fprintf(stderr, "  -o, --audio-pid     : decode specific audio pid from transport stream (default: first program)\n");
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
        { "1080p",      1920, 1080, false, 30.0 },
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
        { "hdv",        1440, 1080, true,  30.0 },
        { "cif",        352,  288,  true,  30.0 },
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
        uint32_t video_buffer, audio_buffer;
        output->GetBufferedVideoFrameCount(&video_buffer);
        output->GetBufferedAudioSampleFrameCount(&audio_buffer);

        /* display status output */
        len += snprintf(string+len, sizeof(string)-len, "%dfps video buffer %d audio buffer %d", completed-framerate, video_buffer, audio_buffer);
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
    } while (!exit_thread);

    pthread_exit(0);
}

/* peek at the next character in the file */
int fpeek(FILE *file)
{
    int c = fgetc(file);
    ungetc(c, file);
    return c;
}

/* find a particular character in the file */
int ffind(int f, FILE *file)
{
    int c = fgetc(file);
    while (c!=f && !feof(file))
        c = fgetc(file);
    if (feof(file))
        return -1;
    ungetc(c, file);
    return 0;
}

/* read next data packet from transport stream */
int next_packet(unsigned char *packet, FILE *file)
{
    while (1) {
        /* read a packet sized chunk */
        if (fread(packet, 188, 1, file)!=1)
            return -1;

        if (packet[0]==0x47 && fpeek(file)==0x47)
            /* success */
            return 0;

        /* resync and try again */
        if (ffind(0x47, file)<0)
            return -1;
    }
}

/* read next packet from transport stream with specified pid
 * return packet length or zero on failure */
int next_data_packet(unsigned char *data, int pid, FILE *file)
{
    unsigned char packet[188];

    /* find next whole transport stream packet with correct pid */
    while (1) {
        /* read next packet */
        if (next_packet(packet, file)<0)
            return 0;

        /* check pid is correct */
        int packet_pid = ((packet[1]<<8) | packet[2]) & 0x1fff;
        if (packet_pid!=pid)
            continue;

        /* skip adaption field */
        int adaptation_field_control = (packet[3] >> 4) & 0x3;
        if (adaptation_field_control==1) {
            /* copy data */
            memcpy(data, packet+4, 188-4);
            return 188-4;
        } else if (adaptation_field_control==3) {
            int adaptation_field_length = packet[4];
            memcpy(data, packet+4+1+adaptation_field_length, 188-4-1-adaptation_field_length);
            return 188-4-1-adaptation_field_length;
        }
    }

    return 0;
}

/* read next packet from transport stream with either video or audio pid
 * return packet length or zero on failure */
int next_stream_packet(unsigned char *data, int vid_pid, int aud_pid, int *pid, FILE *file)
{
    unsigned char packet[188];

    /* find next whole transport stream packet with correct pid */
    while (1) {
        /* read next packet */
        if (next_packet(packet, file)<0)
            return 0;

        /* check pid is correct */
        int packet_pid = ((packet[1]<<8) | packet[2]) & 0x1fff;
        if (packet_pid>1 && packet_pid!=vid_pid && packet_pid!=aud_pid)
            continue;
        *pid = packet_pid;

        /* skip adaption field */
        int adaptation_field_control = (packet[3] >> 4) & 0x3;
        if (adaptation_field_control==1) {
            /* copy data */
            memcpy(data, packet+4, 188-4);
            return 188-4;
        } else if (adaptation_field_control==3) {
            int adaptation_field_length = packet[4];
            memcpy(data, packet+4+1+adaptation_field_length, 188-4-1-adaptation_field_length);
            return 188-4-1-adaptation_field_length;
        }
    }

    return 0;
}

/* read next series of video data packets from transport stream */

/* read next packet from transport which is part of a pes packet
 * return packet length or zero on failure */
int next_pes_packet_data(unsigned char *data, int pid, int start, FILE *file)
{
    unsigned char packet[188];

    /* find next whole transport stream packet with correct pid */
    while (1) {
        /* read next packet */
        if (next_packet(packet, file)<0)
            return 0;

        /* check start indicator */
        int payload_unit_start_indicator = packet[1] & 0x40;
        if (start && !payload_unit_start_indicator)
            continue;

        /* check pid is correct */
        int packet_pid = ((packet[1]<<8) | packet[2]) & 0x1fff;
        if (packet_pid!=pid)
            continue;

        /* skip transport packet header */
        int ptr = 4;

        /* skip adaption field */
        int adaptation_field_control = (packet[3] >> 4) & 0x3;
        if (adaptation_field_control==3)
            ptr += 1 + packet[4];

        /* skip pes header */
        if (payload_unit_start_indicator) {
            int packet_start_code_prefix = (packet[ptr]<<16) | (packet[ptr+1]<<8) | packet[ptr+2];
            int stream_id = packet[ptr+3];
            if (packet_start_code_prefix!=0x1)
                dlexit("error parsing pes header, start_code=0x%06x stream_id=0x%02x", packet_start_code_prefix, stream_id);

            int pes_header_data_length = packet[ptr+8];

            ptr += 9 + pes_header_data_length;
        }

        /* copy data */
        memcpy(data, packet+ptr, 188-ptr);
        return 188-ptr;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    FILE *filein = NULL;
    FILE *fileau = NULL;
    char *filename[16] = {0};
    unsigned int numfiles = 0;
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
    int firstframe = 0;
    int numframes = -1;
    int ntscmode = 1;   /* use e.g. 29.97 instead of 30fps */
    int lumaonly = 0;
    int use_mmap = 0;
    int verbose = 0;

    /* memory map variables */
    off_t pagesize = sysconf(_SC_PAGE_SIZE);
    off_t pageindex;
    off_t offset = 0;

    /* buffer variables */
    size_t vid_size = 0;
    unsigned char *vid_data = NULL;
    size_t aud_size = 0;
    unsigned char *aud_data = NULL;
    size_t read = 0;
    int maxframes = 0;

    /* libmpeg2 variables */
    mpeg2dec_t *mpeg2dec = NULL;
    const mpeg2_info_t *info = NULL;

    /* mpg123 variables */
    mpg123_handle *m = NULL;
    int ret;
    size_t mpa_size = 32768;
    unsigned char *mpa_data = NULL;

    /* liba52 variables */
    a52_state_t *a52_state = NULL;
    sample_t *sample = NULL;
    int ac3_length;
    unsigned char *ac3_frame = NULL;
    size_t ac3_size = 256*2*sizeof(uint16_t);
    int16_t *ac3_block = NULL;

    /* transport stream variables */
    int vid_pid = 0;
    int aud_pid = 0;
    int ac3_pid = 0;

    /* terminal input variables */
    class dlterm term;

    /* parse command line for options */
    while (1) {
        static struct option long_options[] = {
            {"format",    1, NULL, 'f'},
            {"firstframe",1, NULL, 'a'},
            {"numframes", 1, NULL, 'n'},
            {"ntscmode",  1, NULL, 'm'},
            {"luma",      0, NULL, 'l'},
            {"video-pid", 1, NULL, 'p'},
            {"audio-pid", 1, NULL, 'o'},
            {"quiet",     0, NULL, 'q'},
            {"verbose",   0, NULL, 'v'},
            {"usage",     0, NULL, 'u'},
            {"help",      0, NULL, 'u'},
            {NULL,        0, NULL,  0 }
        };

        int optchar = getopt_long(argc, argv, "f:a:n:m:lp:o:qvu", long_options, NULL);
        if (optchar==-1)
            break;

        switch (optchar) {
            case 'f':
                displayformat = optarg;
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

    /* initialise the DeckLink API */
    IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
    if (iterator==NULL)
        dlexit("error: could not initialise, the DeckLink driver may not be installed");

    /* connect to the first card in the system */
    IDeckLink *card;
    if (iterator->Next(&card)!=S_OK)
        dlexit("error: no DeckLink cards found");

    /* obtain the audio/video output interface */
    void *voidptr;
    if (card->QueryInterface(IID_IDeckLinkOutput, &voidptr)!=S_OK)
        dlexit("%s: error: could not obtain the video output interface");
    IDeckLinkOutput *output = (IDeckLinkOutput *)voidptr;

    /* create callback object */
    class callback the_callback(output);

    /* play input files sequentially */
    unsigned int fileindex;
    for (fileindex=0; fileindex<numfiles; fileindex++) {

        /* initialise the semaphore */
        sem_init(&sem, 0, 0);

        /* determine the file type */
        if (strstr(filename[fileindex], "m2v")!=NULL || strstr(filename[fileindex], "M2V")!=NULL)
            filetype = M2V;
        else if (strstr(filename[fileindex], "ts")!=NULL || strstr(filename[fileindex], "trp")!=NULL)
            filetype = TS;
        else
            filetype = YUV;

        /* open the input file */
        filein = fopen(filename[fileindex], "rb");
        fileau = fopen(filename[fileindex], "rb");
        if (!filein || !fileau)
            dlerror("error: failed to open input file \"%s\"", filename[fileindex]);

        /* stat the input file */
        struct stat stat;
        fstat(fileno(filein), &stat);

        /* figure out the mode to set */
        switch (filetype) {
            case YUV:
                if (divine_video_format(filename[fileindex], &pic_width, &pic_height, &interlaced, &framerate, &pixelformat)<0)
                    dlexit("failed to determine output video format from filename: %s", filename[fileindex]);
                if (verbose>=1)
                    dlmessage("video format is %dx%d%c%.2f %s", pic_width, pic_height, interlaced? 'i' : 'p', framerate, pixelformatname[pixelformat]);

                /* allocate the read buffer */
                vid_size = pixelformat==I422? pic_width*pic_height*2 : pic_width*pic_height*3/2;
                vid_data = (unsigned char *) malloc(vid_size);

                /* skip to the first frame */
                if (firstframe) {
                    off_t skip = firstframe * vid_size;
                    if (fseeko(filein, skip, SEEK_SET)<0)
                        dlerror("failed to seek in input file");
                }

                /* calculate the number of frames in the input file */
                maxframes = pixelformat==I420? stat.st_size / (pic_width*pic_height*3/2) : stat.st_size / (pic_width*pic_height*2);

                break;

            case TS:
                /* allocate the read buffer */
                vid_size = aud_size = 184;
                vid_data = (unsigned char *) malloc(vid_size);
                aud_data = (unsigned char *) malloc(aud_size);

                /* skip ts parsing if video pid specified */
                if (vid_pid==0) {
                    /* find the pmt pid */
                    int pmt_pid[16] = {0};
                    int num_pmts = 0;
                    do {

                        /* find the next pat */
                        read = next_data_packet(vid_data, 0, filein);
                        if (read==0)
                            dlexit("failed to find a pat in input file \"%s\" (need to specify the pids)", filename[fileindex]);

                        /* find the pmt_pids */
                        int section_length = (vid_data[2]<<8 | vid_data[3]) & 0xfff;
                        int index = 9;
                        while (index<section_length+4-4) { /* +4: data before section_length, -4: crc_32 */
                            int program_number = (vid_data[index]<<8) | vid_data[index+1];
                            if (program_number>0)
                                pmt_pid[num_pmts++] = (vid_data[index+2]<<8 | vid_data[index+3]) & 0x1fff;
                            index += 4;
                        }

                    } while (num_pmts==0);

                    if (verbose>=1)
                        dlmessage("num_pmts=%d pmt_pid[0]=%d", num_pmts, pmt_pid[0]);

                    /* find the video and audio pids */
                    vid_pid = 0;
                    aud_pid = 0;
                    ac3_pid = 0;
                    int pmt_index = 0;
                    do {
                        /* find the next pmt */
                        read = next_data_packet(vid_data, pmt_pid[pmt_index], filein);
                        if (read==0)
                            dlexit("failed to find pmt with pid in input file \"%s\"", filename[fileindex], pmt_pid[pmt_index]);

                        /* find the video pid */
                        int program_info_length = (vid_data[11]<<8 | vid_data[12]) & 0x0fff;
                        size_t index = 13 + program_info_length;
                        while (index<read) {
                            int stream_type = vid_data[index];
                            int pid = (vid_data[index+1]<<8 | vid_data[index+2]) & 0x1fff;
                            int es_info_length = (vid_data[index+3]<<8 | vid_data[index+4]) & 0xfff;
                            if (vid_pid==0 && (stream_type==0x02 || stream_type==0x80)) /* some files use user private stream types */
                                vid_pid = pid;
                            if (aud_pid==0 && stream_type==0x03 || stream_type==0x04)
                                aud_pid = pid;
                            if (ac3_pid==0 && stream_type==0x81)
                                ac3_pid = pid;
                            index += 5 + es_info_length;
                        }

                        /* try the next pmt */
                        pmt_index++;

                    } while (vid_pid==0); /* might not find an audio pid */

                    if (verbose>=1)
                        dlmessage("vid_pid=%d aud_pid=%d ac3_pid=%d", vid_pid, aud_pid, ac3_pid);
                }

                /* initialise the mpeg2 video decoder */
                mpeg2dec = mpeg2_init();
                if (mpeg2dec==NULL)
                    dlexit("failed to initialise libmpeg2");
                info = mpeg2_info(mpeg2dec);
                mpeg2_accel(MPEG2_ACCEL_DETECT);

                /* initialise the mpeg1 audio decoder */
                ret = mpg123_init();
                if (ret!=MPG123_OK)
                    dlexit("failed to initialise mpg123");
                m = mpg123_new(NULL, &ret);
                if (m==NULL)
                    dlexit("failed to create mpg123 handle");
                mpg123_param(m, MPG123_VERBOSE, 2, 0);
                mpg123_open_feed(m);

                /* initialise the ac3 audio decoder */
                //a52_state = a52_init(mm_accel());
                a52_state = a52_init(MM_ACCEL_X86_MMXEXT);
                sample = a52_samples(a52_state);
                if (a52_state==NULL || sample==NULL)
                    dlexit("failed to initialise liba52");

                /* find the sequence header */
                if (vid_pid) {
                    int seq_found = 0;

                    do {
                        mpeg2_state_t state = mpeg2_parse(mpeg2dec);
                        switch (state) {
                            case STATE_BUFFER:
                                read = next_pes_packet_data(vid_data, vid_pid, 1, filein);
                                mpeg2_buffer(mpeg2dec, vid_data, vid_data+read);
                                break;

                            case STATE_SEQUENCE:
                                pic_width = info->sequence->width;
                                pic_height = info->sequence->height;
                                interlaced = pic_height==720? 0 : 1;
                                framerate = 27000000.0/info->sequence->frame_period;
                                pixelformat = info->sequence->height==info->sequence->chroma_height? I422 : I420;
                                if (verbose>=1)
                                    dlmessage("video format is %dx%d%c%.2f %s", pic_width, pic_height, interlaced? 'i' : 'p', framerate, pixelformatname[pixelformat]);
                                seq_found = 1;
                                break;

                            default:
                                break;
                        }
                    } while(read && !seq_found);
                }

                /* find the audio format */
                if (aud_pid) {
                    /* allocate the audio buffer */
                    mpa_data = (unsigned char *)malloc(mpa_size);

                    do {
                        size_t bytes;
                        read = next_pes_packet_data(aud_data, aud_pid, 1, fileau);
                        // TODO try mpg123_feed
                        ret = mpg123_decode(m, aud_data, read, NULL, 0, &bytes);
                        if (ret==MPG123_ERR) {
                            dlmessage("failed to determine format of audio data: %s", mpg123_strerror(m));
                            aud_pid = 0;
                            break;
                        }
                    } while (ret!=MPG123_NEW_FORMAT);

                    long rate;
                    int channels, enc;
                    mpg123_getformat(m, &rate, &channels, &enc);
                    dlmessage("audio format is  %ldHz x%d channels", rate, channels);
                }

                /* find the ac3 audio format */
                if (ac3_pid) {
                    int flags = 0, sample_rate = 0, bit_rate = 0;

                    /* allocate the audio buffers */
                    ac3_frame = (unsigned char *)malloc(3840+188);
                    ac3_block = (int16_t *)malloc(ac3_size);

                    unsigned int sync;
                    do {
                        read = next_pes_packet_data(aud_data, ac3_pid, 1, fileau);
                        if (read==0) {
                            if (feof(fileau) || ferror(fileau)) {
                                dlmessage("failed to sync ac3 audio");
                                ac3_pid = 0;
                                break;
                            }
                        }

                        /* look for sync in ac3 stream FIXME this won't work if sync is in last 7 bytes of packet */
                        for (sync=0; sync<read-7; sync++) {
                            ret = a52_syncinfo(aud_data+sync, &flags, &sample_rate, &bit_rate);
                            if (ret)
                                break;
                        }
                    } while (ret==0);

                    /* queue the synchronised data */
                    memcpy(ac3_frame, aud_data+sync, read-sync);
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
                }

                break;

            case M2V:
                /* initialise the mpeg2 video decoder */
                mpeg2dec = mpeg2_init();
                if (mpeg2dec==NULL)
                    dlexit("failed to initialise libmpeg2");
                info = mpeg2_info(mpeg2dec);
                mpeg2_accel(MPEG2_ACCEL_DETECT);

                /* allocate the read buffer */
                vid_size = 32*1024;
                vid_data = (unsigned char *) malloc(vid_size);

                /* find the sequence header */
                {
                    int seq_found = 0;

                    do {
                        mpeg2_state_t state = mpeg2_parse(mpeg2dec);
                        switch (state) {
                            case STATE_BUFFER:
                                read = fread(vid_data, 1, vid_size, filein);
                                mpeg2_buffer(mpeg2dec, vid_data, vid_data+read);
                                break;

                            case STATE_SEQUENCE:
                                pic_width = info->sequence->width;
                                pic_height = info->sequence->height;
                                interlaced = pic_height==720? 0 : 1;
                                framerate = 27000000.0/info->sequence->frame_period;
                                pixelformat = info->sequence->height==info->sequence->chroma_height? I422 : I420;
                                if (verbose>=1)
                                    dlmessage("input file is %dx%d%c%.2f %s", pic_width, pic_height, interlaced? 'i' : 'p', framerate, pixelformatname[pixelformat]);
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
        {
            IDeckLinkDisplayModeIterator *iterator;
            if (output->GetDisplayModeIterator(&iterator) != S_OK)
                return false;

            /* find mode for given width and height */
            while (iterator->Next(&mode) == S_OK) {
                mode->GetFrameRate(&framerate_duration, &framerate_scale);
                //fprintf(stderr, "%ldx%ld%c%lld/%lld\n", mode->GetWidth(), mode->GetHeight(), mode->GetFieldDominance()!=bmdProgressiveFrame? 'i' : 'p', framerate_duration, framerate_scale);
                if (mode->GetWidth()==dis_width && mode->GetHeight()==dis_height) {
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
            dlexit("error: failed to find mode for %dx%d%c%.2f", pic_width, pic_height, interlaced? 'i' : 'p', framerate);

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

        /* set the audio output mode */
        //result = output->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, 2, bmdAudioOutputStreamContinuous);
        result = output->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, 2, bmdAudioOutputStreamTimestamped);
        if (result != S_OK) {
            switch (result) {
                case E_ACCESSDENIED : fprintf(stderr, "%s: error: access denied when enabling audio output\n", appname); break;
                case E_OUTOFMEMORY  : fprintf(stderr, "%s: error: out of memory when enabling audio output\n", appname); break;
                case E_INVALIDARG   : fprintf(stderr, "%s: error: invalid number of channels when enabling audio output\n", appname); break;
                default             : fprintf(stderr, "%s: error: failed to enable audio output\n", appname); break;
            }
            return 2;
        }

        /* carry the last frame from the preroll to the main loop */
        IDeckLinkMutableVideoFrame *frame;

        /* preroll as many frames as possible */
        int index = 0;
        int framenum = 0;
        int blocknum = 0;
        int frame_done;
        while (!feof(filein) && (framenum<numframes || numframes<0)) {

            switch (filetype) {
                case YUV:
                {
                    /* loop input file */
                    if (index==maxframes) {
                        index = firstframe;
                        off_t skip = index * vid_size;
                        if (fseeko(filein, skip, SEEK_SET)<0)
                            dlerror("failed to seek in input file");
                    }

                    /* allocate a new frame object */
                    if (output->CreateVideoFrame(pic_width, pic_height, pic_width*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame)!=S_OK)
                        dlexit("error: failed to create video frame\n");

                    frame->GetBytes(&voidptr);
                    unsigned char *uyvy = (unsigned char *)voidptr;
                    if (pixelformat==UYVY) {
                        /* read directly into frame */
                        if (fread(uyvy, pic_width*pic_height*2, 1, filein)!=1)
                            dlerror("failed to read frame from input file");
                    } else {
                        /* read frame from file */
                        if (fread(vid_data, vid_size, 1, filein)!=1)
                            dlerror("failed to read frame from input file");

                        /* convert to uyvy */
                        if (!lumaonly)
                            convert_i420_uyvy(vid_data, uyvy, pic_width, pic_height, pixelformat);
                        else
                            convert_i420_uyvy_lumaonly(vid_data, uyvy, pic_width, pic_height);
                    }
                    break;
                }

                case TS:
                    frame_done = 0;
                    do {
                        mpeg2_state_t state = mpeg2_parse(mpeg2dec);
                        switch (state) {
                            case STATE_BUFFER:
                                read = next_pes_packet_data(vid_data, vid_pid, 0, filein);
                                if (read==0 || feof(filein))  {
                                    if (fseek(filein, 0, SEEK_SET)<0)
                                        dlerror("failed to seek in file \"%s\"", filename[fileindex]);
                                    read = next_pes_packet_data(vid_data, vid_pid, 0, filein);
                                }
                                mpeg2_buffer(mpeg2dec, vid_data, vid_data+read);
                                break;

                            case STATE_SLICE:
                            case STATE_END:
                                if (info->display_fbuf) {
                                    /* allocate a new frame object */
                                    if (output->CreateVideoFrame(pic_width, pic_height, pic_width*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame)!=S_OK)
                                        dlexit("error: failed to create video frame\n");
                                    frame->GetBytes(&voidptr);
                                    unsigned char *uyvy = (unsigned char *)voidptr;
                                    const unsigned char *yuv[3] = {info->display_fbuf->buf[0], info->display_fbuf->buf[1], info->display_fbuf->buf[2]};
                                    convert_yuv_uyvy(yuv, uyvy, pic_width, pic_height, pixelformat);
                                    frame_done = 1;
                                }
                                break;

                            default:
                                break;
                        }
                    } while(read && !frame_done);
                    break;

                case M2V:
                    frame_done = 0;
                    do {
                        mpeg2_state_t state = mpeg2_parse(mpeg2dec);
                        switch (state) {
                            case STATE_BUFFER:
                                read = fread(vid_data, 1, vid_size, filein);
                                if (read==0 || feof(filein)) {
                                    if (fseek(filein, 0, SEEK_SET)<0)
                                        dlerror("failed to seek in file \"%s\"", filename[fileindex]);
                                    read = fread(vid_data, 1, vid_size, filein);
                                }
                                mpeg2_buffer(mpeg2dec, vid_data, vid_data+read);
                                break;

                            case STATE_SLICE:
                            case STATE_END:
                                if (info->display_fbuf) {
                                    /* allocate a new frame object */
                                    if (output->CreateVideoFrame(pic_width, pic_height, pic_width*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame)!=S_OK)
                                        dlexit("error: failed to create video frame\n");
                                    frame->GetBytes(&voidptr);
                                    unsigned char *uyvy = (unsigned char *)voidptr;
                                    const unsigned char *yuv[3] = {info->display_fbuf->buf[0], info->display_fbuf->buf[1], info->display_fbuf->buf[2]};
                                    convert_yuv_uyvy(yuv, uyvy, pic_width, pic_height, pixelformat);
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
        exit_thread = 0;
        pthread_t status_thread;
        if (pthread_create(&status_thread, NULL, display_status, output)<0)
            dlerror("failed to create status thread");

        /* start the video output */
        if (output->StartScheduledPlayback(0, 100, 1.0) != S_OK)
            dlexit("%s: error: failed to start video playback");

        if (verbose>=0)
            dlmessage("info: pre-rolled %d frames", framenum);

        dlmessage("press q to exit...");

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
            if (term.kbhit()) {
                int c = term.readchar();
                if (c=='q')
                    break;
                if (c=='\n')
                    break;
            }

            /* prepare next video frame */
            switch (filetype) {
                case YUV:
                {
                    /* loop input file */
                    if (index==maxframes) {
                        index = firstframe;
                        off_t skip = index * vid_size;
                        if (fseeko(filein, skip, SEEK_SET)<0)
                            dlerror("failed to seek in input file");
                    }

                    /* allocate a new frame object */
                    if (output->CreateVideoFrame(pic_width, pic_height, pic_width*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame)!=S_OK)
                        dlexit("error: failed to create video frame\n");

                    frame->GetBytes(&voidptr);
                    unsigned char *uyvy = (unsigned char *)voidptr;
                    if (pixelformat==UYVY) {
                        /* read directly into frame */
                        if (fread(uyvy, pic_width*pic_height*2, 1, filein)!=1)
                            dlerror("failed to read frame from input file");
                    } else {
                        if (use_mmap) {
                            /* align address to page size */
                            pageindex = (index*vid_size) / pagesize;
                            offset = (index*vid_size) - (pageindex*pagesize);

                            /* memory map next frame from file */
                            if ((vid_data = (unsigned char *) mmap(NULL, vid_size+pagesize, PROT_READ, MAP_PRIVATE, fileno(filein), pageindex*pagesize))==MAP_FAILED)
                                dlerror("failed to map frame from input file");
                        } else {
                            /* read next frame from file */
                            if (fread(vid_data, vid_size, 1, filein)!=1)
                                dlerror("failed to read frame from input file");
                        }

                        /* convert to uyvy */
                        if (!lumaonly)
                            convert_i420_uyvy(vid_data, uyvy, pic_width, pic_height, pixelformat);
                        else
                            convert_i420_uyvy_lumaonly(vid_data, uyvy, pic_width, pic_height);
                    }

                    if (use_mmap)
                        munmap(vid_data, vid_size+pagesize);
                    break;
                }

                case TS:
                    frame_done = 0;
                    do {
                        mpeg2_state_t state = mpeg2_parse(mpeg2dec);
                        switch (state) {
                            case STATE_BUFFER:
                                read = next_pes_packet_data(vid_data, vid_pid, 0, filein);
                                if (read==0 || feof(filein)) {
                                    if (fseek(filein, 0, SEEK_SET)<0)
                                        dlerror("failed to seek in file \"%s\"", filename[fileindex]);
                                    read = next_pes_packet_data(vid_data, vid_pid, 0, filein);
                                }
                                mpeg2_buffer(mpeg2dec, vid_data, vid_data+read);
                                break;

                            case STATE_SLICE:
                            case STATE_END:
                                if (info->display_fbuf) {
                                    /* allocate a new frame object */
                                    if (output->CreateVideoFrame(pic_width, pic_height, pic_width*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame)!=S_OK)
                                        dlexit("error: failed to create video frame\n");
                                    const unsigned char *yuv[3] = {info->display_fbuf->buf[0], info->display_fbuf->buf[1], info->display_fbuf->buf[2]};
                                    frame->GetBytes(&voidptr);
                                    unsigned char *uyvy = (unsigned char *)voidptr;
                                    convert_yuv_uyvy(yuv, uyvy, pic_width, pic_height, pixelformat);
                                    frame_done = 1;
                                }
                                break;

                            default:
                                break;
                        }
                    } while(read && !frame_done);
                    break;

                case M2V:
                    frame_done = 0;
                    do {
                        mpeg2_state_t state = mpeg2_parse(mpeg2dec);
                        switch (state) {
                            case STATE_BUFFER:
                                read = fread(vid_data, 1, vid_size, filein);
                                if (read==0 || feof(filein)) {
                                    if (fseek(filein, 0, SEEK_SET)<0)
                                        dlerror("failed to seek in file \"%s\"", filename[fileindex]);
                                    read = fread(vid_data, 1, vid_size, filein);
                                }
                                mpeg2_buffer(mpeg2dec, vid_data, vid_data+read);
                                break;

                            case STATE_SLICE:
                            case STATE_END:
                                if (info->display_fbuf) {
                                    /* allocate a new frame object */
                                    if (output->CreateVideoFrame(pic_width, pic_height, pic_width*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &frame)!=S_OK)
                                        dlexit("error: failed to create video frame\n");
                                    const unsigned char *yuv[3] = {info->display_fbuf->buf[0], info->display_fbuf->buf[1], info->display_fbuf->buf[2]};
                                    frame->GetBytes(&voidptr);
                                    unsigned char *uyvy = (unsigned char *)voidptr;
                                    convert_yuv_uyvy(yuv, uyvy, pic_width, pic_height, pixelformat);
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

            /* maintain audio buffer level */
            switch (filetype) {
                case TS:
                    if (aud_pid) {

                        while (1) {
                            unsigned int buffered;
                            if (output->GetBufferedAudioSampleFrameCount(&buffered) != S_OK)
                                dlexit("failed to get audio buffer level");

                            if (buffered >= 48000)
                                break;

                            /* feed the audio decoder */
                            size_t decoded = 0;
                            do {
                                read = next_pes_packet_data(aud_data, aud_pid, 0, fileau);
                                if (read==0) {
                                    if (ferror(fileau)) {
                                        dlmessage("error reading file \"%s\": %s", filename[fileindex], strerror(errno));
                                        aud_pid = 0;
                                        break;
                                    }
                                    if (feof(fileau)) {
                                        off_t offset = 0;
                                        ret = mpg123_feedseek(m, 0, SEEK_SET, &offset);
                                        if (ret != MPG123_OK)
                                            dlerror("failed to seek in audio stream: %d", mpg123_strerror(m));
                                        if (fseek(fileau, offset, SEEK_SET)<0)
                                            dlerror("failed to seek in file \"%s\"", filename[fileindex]);
                                        continue;
                                    }
                                }
                                ret = mpg123_decode(m, aud_data, read, mpa_data, mpa_size, &decoded);
                            } while (!decoded && ret!=MPG123_ERR);

                            /* buffer decoded audio */
                            uint32_t scheduled;
                            result = output->ScheduleAudioSamples(mpa_data, decoded/4, 0, 0, &scheduled);
                            //dlmessage("buffer level %d: decoded %d bytes and scheduled %d frames", buffered, decoded, scheduled);
                            if (result != S_OK) {
                                dlmessage("%s: error: frame %d: failed to schedule audio data", appname, framenum);
                                aud_pid = 0;
                                break;
                            }
                        }
                    }

                    if (ac3_pid) {

                        while (1) {
                            unsigned int buffered;
                            if (output->GetBufferedAudioSampleFrameCount(&buffered) != S_OK)
                                dlexit("failed to get audio buffer level");

                            if (buffered >= 48000)
                                break;

                            /* sync to next frame */
                            int length = 0;
                            int flags, sample_rate, bit_rate;
                            do {

                                if (ac3_length<7) {
                                    read = next_pes_packet_data(aud_data, ac3_pid, 0, fileau);
                                    if (read==0) {
                                        if (ferror(fileau)) {
                                            dlmessage("error reading file \"%s\": %s", filename[fileindex], strerror(errno));
                                            ac3_pid = 0;
                                            break;
                                        }
                                        if (feof(fileau)) {
                                            off_t offset = 0;
                                            if (fseek(fileau, offset, SEEK_SET)<0)
                                                dlerror("failed to seek in file \"%s\"", filename[fileindex]);
                                            continue;
                                        }
                                    }
                                    memcpy(ac3_frame+ac3_length, aud_data, read);
                                    ac3_length += read;
                                }
                                length = a52_syncinfo(ac3_frame, &flags, &sample_rate, &bit_rate);

                                if (length==0)
                                    fprintf(stderr, "length=%d ac_length=%d ac3_frame=%02x %02x %02x %02x\n", length, ac3_length,
                                        ac3_frame[0], ac3_frame[1], ac3_frame[2], ac3_frame[3]);
                            } while (0);

                            /* prepare the next frame for decoding */
                            do {
                                /* read data from transport stream to complete frame */
                                while (ac3_length < length) {
                                    read = next_pes_packet_data(aud_data, ac3_pid, 0, fileau);
                                    if (read==0) {
                                        if (ferror(fileau)) {
                                            dlmessage("error reading file \"%s\": %s", filename[fileindex], strerror(errno));
                                            ac3_pid = 0;
                                            break;
                                        }
                                        if (feof(fileau)) {
                                            off_t offset = 0;
                                            if (fseek(fileau, offset, SEEK_SET)<0)
                                                dlerror("failed to seek in file \"%s\"", filename[fileindex]);
                                            continue;
                                        }
                                    }
                                    memcpy(ac3_frame+ac3_length, aud_data, read);
                                    ac3_length += read;
                                }
                            } while (0);

                            /* feed the frame to the audio decoder */
                            flags = A52_STEREO;
                            sample_t level = 32767.0;
                            sample_t bias = 0.0;
                            a52_frame(a52_state, ac3_frame, &flags, &level, bias);

                            /* buffer decoded audio */
                            int i, j;
                            for (i=0; i<6; i++) {
                                a52_block(a52_state);

                                /* convert decoded samples to integer and interleave */
                                for (j=0; j<256; j++) {
                                    ac3_block[j*2  ] = (int16_t) lround(sample[j    ]);
                                    ac3_block[j*2+1] = (int16_t) lround(sample[j+256]);
                                }

                                uint32_t scheduled;
                                result = output->ScheduleAudioSamples(ac3_block, 256, blocknum*256*framerate_scale/48000, framerate_scale, &scheduled);
                                //dlmessage("buffer level %d: decoded %d bytes and scheduled %d frames", buffered, decoded, scheduled);
                                if (result != S_OK) {
                                    dlmessage("%s: error: block %d: failed to schedule audio data", appname, blocknum);
                                    aud_pid = 0;
                                    break;
                                }

                                blocknum++;
                            }

                            /* keep leftover data for next frame */
                            if (ac3_length-length)
                                memcpy(ac3_frame, ac3_frame+length, ac3_length-length);
                            ac3_length = ac3_length-length;
                        }
                    }
                    break;

                default:
                    /* no other audio support */
                    break;
            }

        }

        /* stop the status display */
        exit_thread = 1;
        pthread_join(status_thread, NULL);

        /* stop the video output */
        output->StopScheduledPlayback(0, NULL, 0);
        output->DisableVideoOutput();
        output->DisableAudioOutput();

        /* the semaphone has to be re-initialised */
        sem_destroy(&sem);

    }

    /* tidy up */
    if (mpeg2dec)
        mpeg2_close(mpeg2dec);
    mpg123_delete(m);
    mpg123_exit();
    if (a52_state)
        a52_free(a52_state);
    card->Release();
    iterator->Release();
    if (ac3_block)
        free(ac3_block);
    if (ac3_frame)
        free(ac3_frame);
    if (mpa_data)
        free(mpa_data);
    if (vid_data)
        free(vid_data);

    /* report statistics */
    if (verbose>=0)
        fprintf(stdout, "\n%d frames: %d late, %d dropped, %d flushed\n", completed, late, dropped, flushed);

    return 0;
}
