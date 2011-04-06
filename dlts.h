#ifndef DLTS_H
#define DLTS_H

#include "dlutil.h"

int next_packet(unsigned char *packet, FILE *file);
int next_data_packet(unsigned char *data, int pid, FILE *file);
int next_stream_packet(unsigned char *data, int vid_pid, int aud_pid, int *pid, FILE *file);
int next_pes_packet_data(unsigned char *data, long long *pts, int pid, int start, FILE *file);
int find_pid_for_stream_type(int stream_types[], int num_stream_types, const char *filename, FILE *file);

#endif
