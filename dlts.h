#ifndef DLTS_H
#define DLTS_H

#include "dlutil.h"
#include "dlsource.h"

int next_packet(unsigned char *packet, dlsource *source, dltoken_t token);
int next_data_packet(unsigned char *data, int pid, dlsource *source, dltoken_t token);
int next_stream_packet(unsigned char *data, int vid_pid, int aud_pid, int *pid, dlsource *source, dltoken_t token);
int next_pes_packet_data(unsigned char *data, long long *pts, int pid, int start, dlsource *source, dltoken_t token);
int find_pid_for_stream_type(int stream_types[], int num_stream_types, int *found_type, dlsource *source, dltoken_t token);

#endif
