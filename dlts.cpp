/*
 * Description: read packets of data from transport stream.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2011 4i2i Communications Ltd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dlutil.h"
#include "dlts.h"

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
int next_packet(unsigned char *packet, dlsource *source)
{
    /* the bytes of the packet which are already in the buffer, either from a
       short read or from a resync */
    int read = 0;

    while (1) {
        /* fill the buffer to a packet sized chunk */
        while (read!=188) {
            int ret = source->read(packet+read, 188-read);
            if (ret<=0) {
                /* the end of the input is not an error, a partial packet is */
                if (read)
                    dlmessage("failed to read the last %d bytes of a transport stream packet", 188-read);
                return -1;
            }
            read += ret;
        }

        if (packet[0]==0x47 /*&& fpeek(file)==0x47*/) // TODO lookahead in dlsource.
            /* success */
            return 0;

        /* resync to the first sync byte in the chunk and keep what follows it,
           the rest of that packet is read by the loop above, or start again
           with a whole new chunk if there is no sync byte in this one */
        read = 0;
        for (int i=1; i<188; i++) {
            if (packet[i]==0x47) {
                memmove(packet, packet+i, 188-i);
                read = 188-i;
                break;
            }
        }
    }
}

/* read next packet from transport stream with specified pid
 * return packet length or error code on failure */
int next_data_packet(unsigned char *data, int pid, dlsource *source)
{
    unsigned char packet[188];

    /* find next whole transport stream packet with correct pid */
    while (1) {
        /* read next packet */
        int read = next_packet(packet, source);
        if (read<0)
            return read;

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
int next_stream_packet(unsigned char *data, int vid_pid, int aud_pid, int *pid, dlsource *source)
{
    unsigned char packet[188];

    /* find next whole transport stream packet with correct pid */
    while (1) {
        /* read next packet */
        if (next_packet(packet, source)<0)
            return 0;

        /* check pid is correct */
        int packet_pid = ((packet[1]<<8) | packet[2]) & 0x1fff;
        if (packet_pid!=vid_pid && packet_pid!=aud_pid)
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
int next_pes_packet_data(unsigned char *data, long long *pts, long long *dts, int pid, int start, dlsource *source)
{
    unsigned char packet[188];

    /* default no pts */
    if (pts)
        *pts = -1;
    if (dts)
        *dts = -1;

    /* find next whole transport stream packet with correct pid */
    while (1) {
        /* read next packet */
        if (next_packet(packet, source)<0)
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

            /* look for pts and dts */
            int pts_dts_flags = packet[ptr+7] >> 6;
            if (pts_dts_flags==2 || pts_dts_flags==3) {
                long long pts3 = (packet[ptr+9] >> 1) & 0x7;
                long long pts2 = (packet[ptr+10] << 7 | (packet[ptr+11] >> 1));
                long long pts1 = (packet[ptr+12] << 7 | (packet[ptr+13] >> 1));
                if (pts)
                    *pts = (pts3<<30) | (pts2<<15) | pts1;
            }
            if (pts_dts_flags==3) {
                long long dts3 = (packet[ptr+14] >> 1) & 0x7;
                long long dts2 = (packet[ptr+15] << 7 | (packet[ptr+16] >> 1));
                long long dts1 = (packet[ptr+17] << 7 | (packet[ptr+18] >> 1));
                if (dts)
                    *dts = (dts3<<30) | (dts2<<15) | dts1;
            }

            int pes_header_data_length = packet[ptr+8];

            ptr += 9 + pes_header_data_length;
        }

        /* copy data */
        memcpy(data, packet+ptr, 188-ptr);
        return 188-ptr;
    }

    return 0;
}

/* the stream type of private data in a transport stream does not say what the
   data is, the descriptors of the elementary stream do: dvb signals ac3 audio as
   private data with an ac3 descriptor, and smpte 302m audio with a registration
   descriptor of "BSSD". return the stream type which normally carries the codec
   the descriptors describe, or zero if they do not describe one */
static int stream_type_of_private_data(const unsigned char *descriptors, size_t length)
{
    for (size_t i=0; i+2<=length; i+=2+descriptors[i+1]) {
        int tag = descriptors[i];
        int len = descriptors[i+1];

        /* a descriptor which runs past the end of the data is not usable */
        if (i+2+len>length)
            break;

        switch (tag) {
            case 0x05:
                /* registration descriptor, a four character format identifier */
                if (len>=4 && memcmp(descriptors+i+2, "BSSD", 4)==0)
                    return 0x06;    /* smpte 302m audio, decoded as private data */
                if (len>=4 && memcmp(descriptors+i+2, "AC-3", 4)==0)
                    return 0x81;
                break;

            case 0x6a:
                /* dvb ac3 descriptor */
                return 0x81;

            /* there is no decoder for these here, but naming the stream type
               which normally carries them means the pid is not selected */
            case 0x7a:
                /* dvb enhanced ac3 descriptor */
                return 0x87;

            case 0x7b:
                /* dvb dts descriptor */
                return 0x82;

            case 0x7c:
                /* dvb aac descriptor */
                return 0x11;
        }
    }

    return 0;
}

int find_pid_for_stream_type(int stream_types[], int num_stream_types, int *found_type, dlsource *source)
{
    unsigned char packet[188];

    /* sanity check */
    if (stream_types==NULL)
        return 0;
    if (num_stream_types==0)
        return 0;

    /* find the pmt pid */
    int pmt_pid[16] = {0};
    int num_pmts = 0;
    do {

        /* find the next pat */
        int read = next_data_packet(packet, 0, source);
        if (read<=0) {
            dlmessage("failed to find a pat in input file \"%s\" (need to specify the pids)", source->name());
            return 0;
        }

        /* find the pmt_pids */
        int section_length = (packet[2]<<8 | packet[3]) & 0xfff;
        int index = 9;
        while (index<section_length+4-4) { /* +4: packet before section_length, -4: crc_32 */
            int program_number = (packet[index]<<8) | packet[index+1];
            if (program_number>0)
                pmt_pid[num_pmts++] = (packet[index+2]<<8 | packet[index+3]) & 0x1fff;
            index += 4;
        }

    } while (num_pmts==0);

    //dlmessage("num_pmts=%d pmt_pid[0]=%d pmt_pid[1]=%d", num_pmts, pmt_pid[0], pmt_pid[1]);

    /* find the video or audio pid */
    int pid = 0;
    int pmt_index = 0;
    do {
        /* find the next pmt */
        size_t read = next_data_packet(packet, pmt_pid[pmt_index], source);
        if (read<=0) {
            dlmessage("failed to find a pmt in input file \"%s\" (need to specify the pids)", source->name());
            return 0;
        }

        int section_length = (packet[2]<<8 | packet[3]) & 0xfff;
        if (section_length>1021)
            continue;

        /* skip any descriptors */
        int program_info_length = (packet[11]<<8 | packet[12]) & 0xfff;
        if (program_info_length>section_length-9)
            /* this seems to be a problem in some streams, ignore packet */
            continue;
        size_t index = 13 + program_info_length;

        /* find the pid which carries one of the given stream types */
        while (index+5<=read) {
            int stream_type = packet[index];
            int pid = (packet[index+1]<<8 | packet[index+2]) & 0x1fff;
            int es_info_length = (packet[index+3]<<8 | packet[index+4]) & 0xfff;
            /* private data can be any codec, so ask the descriptors which it is */
            if (stream_type==0x06) {
                int private_type = stream_type_of_private_data(packet+index+5, mmin((size_t)es_info_length, read-index-5));
                if (private_type)
                    stream_type = private_type;
            }
            /* try to match stream type */
            for (int i=0; i<num_stream_types; i++)
                if (stream_types[i]==stream_type) {
                    *found_type = stream_type;
                    return pid;
                }
            /* stream_type==0x02 - mpeg2 video
             * stream_type==0x80 - user private, assume mpeg2 video
             * stream_type==0x03 - mpeg1 audio
             * stream_type==0x04 - mpeg2 audio
             * stream_type==0x06 - private data, assume smpte 302m audio
             * stream_type==0x81 - user private, assume ac3 audio
             * stream_type==0x1b - h.264 video
             * stream_type==0x24 - hevc video */
            index += 5 + es_info_length;
        }

        /* try the next pmt */
        pmt_index++;

    } while (pid==0 && pmt_index<num_pmts);

    return 0;
}

/* transport stream demultiplexer class */
dldemux::dldemux()
{
    source = NULL;
    num_streams = 0;
    packet = NULL;
    pts_offset = 0ll;
    pts_highest = -1ll;
    pts_rebase = false;

    for (int i=0; i<MAX_STREAMS; i++) {
        stream[i].pid = -1;
        stream[i].assembly.data = NULL;
        stream[i].assembly.size = 0;
        stream[i].assembly.pts = stream[i].assembly.dts = -1ll;
        stream[i].assembly.discontinuity = false;
        stream[i].assembly_size = 0;
        stream[i].started = false;
        stream[i].queued = 0;
        stream[i].overflowed = false;
    }
}

dldemux::~dldemux()
{
    for (int i=0; i<num_streams; i++) {
        if (stream[i].assembly.data)
            free(stream[i].assembly.data);
        while (!stream[i].queue.empty()) {
            free(stream[i].queue.front().data);
            stream[i].queue.pop_front();
        }
    }

    if (packet)
        free(packet);
}

int dldemux::attach(dlsource *s)
{
    /* attach our own reader on the input source */
    source = s->attach();

    /* allocate the transport packet buffer */
    packet = (unsigned char *) malloc(188);

    return 0;
}

int dldemux::register_pid(int pid)
{
    /* a pid may only be demultiplexed once */
    if (find(pid)>=0)
        dlexit("pid %d is already demultiplexed", pid);

    if (num_streams==MAX_STREAMS)
        dlexit("cannot demultiplex more than %d pids", MAX_STREAMS);

    stream[num_streams].pid = pid;
    stream[num_streams].assembly_size = 64*1024;    /* arbitrary size to start with */
    stream[num_streams].assembly.data = (unsigned char *) malloc(stream[num_streams].assembly_size);

    return num_streams++;
}

int dldemux::find(int pid)
{
    for (int i=0; i<num_streams; i++)
        if (stream[i].pid==pid)
            return i;

    return -1;
}

/* move the packet under assembly to the queue, dropping the oldest packet if the
   consumer of this pid has stopped reading */
void dldemux::queue_packet(int index)
{
    struct stream_t *s = &stream[index];

    if (s->assembly.size==0)
        return;

    /* trim the buffer to the packet, so that a queue of small packets does not
       hold on to a buffer the size of the largest packet for each one */
    s->assembly.data = (unsigned char *) realloc(s->assembly.data, s->assembly.size);

    /* hand the buffer to the queue and allocate another for the next packet */
    s->queue.push_back(s->assembly);
    s->queued += s->assembly.size;
    s->assembly.data = (unsigned char *) malloc(s->assembly_size);
    s->assembly.size = 0;
    s->assembly.pts = s->assembly.dts = -1ll;
    s->assembly.discontinuity = false;

    /* the queue only has to hold the skew between the pids in the stream */
    while (s->queued>MAX_QUEUE_BYTES && !s->queue.empty()) {
        /* only complain the once, the reader of this pid is not keeping up */
        if (!s->overflowed) {
            dlmessage("warning: dropping data queued for pid %d, more than %d bytes are buffered", s->pid, MAX_QUEUE_BYTES);
            s->overflowed = true;
        }
        s->queued -= s->queue.front().size;
        free(s->queue.front().data);
        s->queue.pop_front();
    }
}

/* offset a timestamp so that the input carries on from the previous pass through
   it, the first timestamp after a loop sets the offset for every pid at once */
long long dldemux::rebase_timestamp(long long timestamp)
{
    if (pts_rebase) {
        if (pts_highest>=0)
            pts_offset += pts_highest - (timestamp + pts_offset);
        pts_rebase = false;
    }

    timestamp += pts_offset;
    if (timestamp>pts_highest)
        pts_highest = timestamp;

    return timestamp;
}

/* the input has jumped, so the packets under assembly are incomplete, and the next
   packet of every pid joins data which is not continuous with what came before */
void dldemux::resync()
{
    for (int i=0; i<num_streams; i++) {
        stream[i].assembly.size = 0;
        stream[i].assembly.pts = stream[i].assembly.dts = -1ll;
        stream[i].assembly.discontinuity = true;
        stream[i].started = false;
    }
}

/* read transport packets, filling the queue of every registered pid, until the
   given stream has a complete pes packet */
int dldemux::fill(int index)
{
    int rewinds = 0;

    while (stream[index].queue.empty()) {

        /* read the next transport packet */
        if (next_packet(packet, source)<0) {
            /* loop the input, for every pid at once */
            if (++rewinds>1 || source->rewind()<0)
                return -1;
            resync();
            pts_rebase = true;
            continue;
        }

        /* discard packets for pids nobody is reading */
        int packet_pid = ((packet[1]<<8) | packet[2]) & 0x1fff;
        int i = find(packet_pid);
        if (i<0)
            continue;

        struct stream_t *s = &stream[i];
        int payload_unit_start_indicator = packet[1] & 0x40;

        /* the start of a pes packet is the end of the previous one */
        if (payload_unit_start_indicator)
            queue_packet(i);
        else if (!s->started)
            /* looking for the start of the first pes packet of this pid */
            continue;

        /* skip transport packet header */
        int ptr = 4;

        /* skip adaption field */
        int adaptation_field_control = (packet[3] >> 4) & 0x3;
        if (adaptation_field_control==2 || adaptation_field_control==3)
            ptr += 1 + packet[4];
        if (ptr>=188)
            continue;

        /* skip pes header */
        if (payload_unit_start_indicator) {
            int packet_start_code_prefix = (packet[ptr]<<16) | (packet[ptr+1]<<8) | packet[ptr+2];
            int stream_id = packet[ptr+3];
            if (packet_start_code_prefix!=0x1)
                dlexit("error parsing pes header, start_code=0x%06x stream_id=0x%02x", packet_start_code_prefix, stream_id);
            s->started = true;

            /* look for pts and dts */
            int pts_dts_flags = packet[ptr+7] >> 6;
            if (pts_dts_flags==2 || pts_dts_flags==3) {
                long long pts3 = (packet[ptr+9] >> 1) & 0x7;
                long long pts2 = (packet[ptr+10] << 7 | (packet[ptr+11] >> 1));
                long long pts1 = (packet[ptr+12] << 7 | (packet[ptr+13] >> 1));
                s->assembly.pts = rebase_timestamp((pts3<<30) | (pts2<<15) | pts1);
            }
            if (pts_dts_flags==3) {
                long long dts3 = (packet[ptr+14] >> 1) & 0x7;
                long long dts2 = (packet[ptr+15] << 7 | (packet[ptr+16] >> 1));
                long long dts1 = (packet[ptr+17] << 7 | (packet[ptr+18] >> 1));
                s->assembly.dts = ((dts3<<30) | (dts2<<15) | dts1) + pts_offset;
            }

            int pes_header_data_length = packet[ptr+8];

            ptr += 9 + pes_header_data_length;
            if (ptr>=188)
                continue;
        }

        /* resize the assembly buffer if necessary */
        if (s->assembly.size+188-ptr > s->assembly_size) {
            s->assembly_size += s->assembly_size;
            s->assembly.data = (unsigned char *) realloc(s->assembly.data, s->assembly_size);
        }

        /* copy the payload of the packet */
        memcpy(s->assembly.data+s->assembly.size, packet+ptr, 188-ptr);
        s->assembly.size += 188-ptr;
    }

    return 0;
}

int dldemux::read(int pid, pespacket_t *packet)
{
    int index = find(pid);
    if (index<0)
        dlexit("pid %d is not demultiplexed", pid);

    /* read more of the input if this pid has nothing queued */
    if (stream[index].queue.empty())
        if (fill(index)<0)
            return -1;

    *packet = stream[index].queue.front();
    stream[index].queued -= packet->size;
    stream[index].queue.pop_front();

    return 0;
}

int dldemux::rewind()
{
    if (source->rewind()<0)
        return -1;

    /* the queued data is from the previous pass through the input */
    for (int i=0; i<num_streams; i++) {
        while (!stream[i].queue.empty()) {
            free(stream[i].queue.front().data);
            stream[i].queue.pop_front();
        }
        stream[i].queued = 0;
        stream[i].overflowed = false;
    }
    resync();
    pts_rebase = true;

    return 0;
}
