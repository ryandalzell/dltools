#ifndef DLTS_H
#define DLTS_H

#include <deque>

#include "dlutil.h"
#include "dlsource.h"

/* maximum number of pids which can be demultiplexed at once */
#define MAX_STREAMS 8

/* bytes queued for one pid before the oldest data is dropped */
#define MAX_QUEUE_BYTES (4*1024*1024)

/* a complete pes packet, the data belongs to whoever reads it */
typedef struct {
    unsigned char *data;
    size_t size;
    long long pts, dts;
    bool discontinuity;     /* the first packet of a new pass through the input */
} pespacket_t;

/* a single reader on a transport stream, fanning pes packets out to a queue for
   each pid, so that streams which are far apart in the file but together in time
   can be read at the same time from one position, and from a socket */
class dldemux
{
public:
    dldemux();
    ~dldemux();

    /* attach a reader on the input source */
    int attach(dlsource *source);

    /* register a pid to demultiplex, before any data is read */
    int register_pid(int pid);

    /* read the next complete pes packet for a pid, the caller owns the data */
    int read(int pid, pespacket_t *packet);

    /* start the input again for every pid */
    int rewind();

    /* the reader, for the metadata interface of the format layer */
    dlsource *get_source() { return source; }

protected:
    /* index of a registered pid, or -1 if the pid is not wanted */
    int find(int pid);
    /* apply the timestamp offset which keeps the input monotonic when it loops */
    long long rebase_timestamp(long long timestamp);
    /* read transport packets until the stream has a complete pes packet queued */
    int fill(int index);
    /* queue the pes packet under assembly, and start a new one */
    void queue_packet(int index);
    /* discard the partly assembled packets at a discontinuity in the input */
    void resync();

    /* the reader shared by every pid */
    dlsource *source;

    /* the assembly state and queue of complete packets for each pid */
    struct stream_t {
        int pid;
        pespacket_t assembly;   /* the packet being assembled */
        size_t assembly_size;   /* bytes allocated to it */
        bool started;           /* the start of a pes packet has been seen */
        std::deque<pespacket_t> queue;
        size_t queued;          /* bytes in the queue */
        bool overflowed;        /* the queue has been dropping data */
    } stream[MAX_STREAMS];
    int num_streams;

    /* the transport packet being parsed */
    unsigned char *packet;

    /* the input timestamps continue where the previous pass through the input
       finished, so that looping the input does not move the clock backwards */
    long long pts_offset;
    long long pts_highest;
    bool pts_rebase;
};

int next_packet(unsigned char *packet, dlsource *source);
int next_data_packet(unsigned char *data, int pid, dlsource *source);
int next_stream_packet(unsigned char *data, int vid_pid, int aud_pid, int *pid, dlsource *source);
int next_pes_packet_data(unsigned char *data, long long *pts, long long *dts, int pid, int start, dlsource *source);
int find_pid_for_stream_type(int stream_types[], int num_stream_types, int *found_type, dlsource *source);

#endif
