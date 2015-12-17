/*
 * Description: container format decoder.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2015 4i2i Communications Ltd.
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "dlformat.h"
#include "dlts.h"

/* virtual base class for data format decoders */
dlformat::dlformat()
{
    source = NULL;
}

dlformat::~dlformat()
{
}

int dlformat::attach(dlsource* s)
{
    /* attach the input source */
    source = s;
    return 0;
}

size_t dlformat::read(unsigned char *buf, size_t bytes, int mux)
{
    return source->read(buf, bytes);
}

const unsigned char *dlformat::read(size_t *bytes, int mux)
{
    return source->read(bytes);
}

long long dlformat::get_timestamp(int pid)
{
    /* only implemented in sub-classes */
    return -1ll;
}

dlsource* dlformat::get_source()
{
    return source;
}

/* elementary stream format decoder class */
/* transport stream format decoder class */
dltstream::dltstream()
{
    source = NULL;
}

dltstream::~dltstream()
{
    // TODO empty pids and free buffers.
}

void dltstream::register_pid(int pid)
{
    pid_t p = { pid, 0, NULL, 0, -1ll };

    /* allocate data buffer for pid */
    p.size = 184 * 1024; /* 1k ts packet payloads */
    p.data = (unsigned char *)malloc(p.size);

    /* register pid */
    pids.push_back(p);
}

int dltstream::attach(dlsource *s)
{
    /* attach the input source */
    source = s;

    /* prime the pids to a payload start indicator */
    for (unsigned i=0; i<pids.size(); i++)
        process(1, pids[i], true);

    return 0;
}

size_t dltstream::read(unsigned char *buf, size_t bytes, int pid)
{
    pid_t *p = find_pid(pid);
    /* keep amount of data stored to a minimum */
    if (p->bytes==0)
        process(1, *p);
    size_t valid = mmin(bytes, p->bytes);
    memcpy(buf, p->data, valid);
    if (valid<p->bytes)
        memmove(p->data, p->data+valid, p->bytes-valid);
    p->bytes -= valid;
    return valid;
}

const unsigned char *dltstream::read(size_t *bytes, int pid)
{
    pid_t *p = find_pid(pid);
    /* keep amount of data stored to a minimum */
    if (p->bytes==0)
        process(1, *p);
    // FIXME p->bytes larger than bytes?
    *bytes = p->bytes;
    return p->data;
}

long long int dltstream::get_timestamp(int pid)
{
    pid_t *p = find_pid(pid);
    return p->pts;
}

/* poor man's associative array? */
dltstream::pid_t *dltstream::find_pid(int pid)
{
    for (unsigned i=0; i<pids.size(); i++)
        if (pid==pids[i].pid)
            return &pids[i];
    return NULL;
}

size_t dltstream::process(size_t bytes, dltstream::pid_t &req_pid, bool start)
{
    /* process transport stream until required amount of
     * pes payload data is available for the required pid */
    while (req_pid.bytes < bytes) {

        /* read next packet */
        unsigned char packet[188];
        if (next_packet(packet, source)<0)
            return 0;

        /* check start indicator */
        int payload_unit_start_indicator = packet[1] & 0x40;

        /* look for registered pid */
        unsigned i;
        int packet_pid = ((packet[1]<<8) | packet[2]) & 0x1fff;
        for (i=0; i<pids.size(); i++)
            if (packet_pid==pids[i].pid)
                break;

        /* otherwise keep looking for more data */
        if (i==pids.size())
            continue;
        pid_t &p = pids[i];

        /* skip transport packet header */
        int ptr = 4;

        /* skip adaption field */
        int adaptation_field_control = (packet[3] >> 4) & 0x3;
        if (adaptation_field_control==3) {
            int adaptation_field_length = packet[4] + 1;
            ptr += adaptation_field_length;
        }

        /* skip pes header */
        if (payload_unit_start_indicator) {
            int packet_start_code_prefix = (packet[ptr]<<16) | (packet[ptr+1]<<8) | packet[ptr+2];
            int stream_id = packet[ptr+3];
            if (packet_start_code_prefix!=0x1)
                dlexit("error parsing pes header, start_code=0x%06x stream_id=0x%02x", packet_start_code_prefix, stream_id);

            /* look for pts and dts */
            int pts_dts_flags = packet[ptr+7] >> 6;
            if (pts_dts_flags==2 || pts_dts_flags==3) {
                long long pts3 = (packet[ptr+9] >> 1) && 0x7;
                long long pts2 = (packet[ptr+10] << 7 | (packet[ptr+11] >> 1));
                long long pts1 = (packet[ptr+12] << 7 | (packet[ptr+13] >> 1));
                p.pts = (pts3<<30) | (pts2<<15) | pts1;
            }

            int pes_header_data_length = packet[ptr+8];

            ptr += 9 + pes_header_data_length;
        }

        /* when starting up, don't copy data until first timestamp */
        if (start)
            if (p.pts<0)
                continue;

        /* copy payload data */
        int payload_length = 188-ptr;
        if (p.bytes + payload_length > p.size) {
            p.size += 184 * 1024;
            p.data = (unsigned char *)realloc(p.data, p.size);
        }
        memcpy(p.data+p.bytes, packet+ptr, payload_length);
        p.bytes += payload_length;

    }

    return bytes;
}
