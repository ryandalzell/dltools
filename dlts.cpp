/*
 * Description: read packets of data from transport stream.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2011 4i2i Communications Ltd.
 */

#include <stdio.h>
#include <string.h>

#include "dlutil.h"

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
int next_pes_packet_data(unsigned char *data, long long *pts, int pid, int start, FILE *file)
{
    unsigned char packet[188];

    /* default no pts */
    *pts = -1;

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

            /* look for pts and dts */
            int pts_dts_flags = packet[ptr+7] >> 6;
            if (pts_dts_flags==2 || pts_dts_flags==3) {
                long long pts3 = (packet[ptr+9] >> 1) && 0x7;
                long long pts2 = (packet[ptr+10] << 7 | (packet[ptr+11] >> 1));
                long long pts1 = (packet[ptr+12] << 7 | (packet[ptr+13] >> 1));
                *pts = (pts3<<30) | (pts2<<15) | pts1;
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

int find_pid_for_stream_type(int stream_types[], int num_stream_types, const char *filename, FILE *file)
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
        int read = next_data_packet(packet, 0, file);
        if (read==0)
            dlexit("failed to find a pat in input file \"%s\" (need to specify the pids)", filename);

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
        size_t read = next_data_packet(packet, pmt_pid[pmt_index], file);
        if (read==0)
            return 0;

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
        while (index<read) {
            int stream_type = packet[index];
            int pid = (packet[index+1]<<8 | packet[index+2]) & 0x1fff;
            int es_info_length = (packet[index+3]<<8 | packet[index+4]) & 0xfff;
            /* try to match stream type */
            for (int i=0; i<num_stream_types; i++)
                if (stream_types[i]==stream_type)
                    return pid;
            /* stream_type==0x02 - mpeg2 video
             * stream_type==0x80 - user private, assume mpeg2 video
             * stream_type==0x03 - mpeg1 audio
             * stream_type==0x04 - mpeg2 audio
             * stream_type==0x81 - user private, assume ac3 audio */
            index += 5 + es_info_length;
        }

        /* try the next pmt */
        pmt_index++;

    } while (pid==0 && pmt_index<num_pmts);

    return 0;
}
