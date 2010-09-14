#ifndef DLUTIL_H
#define DLUTIL_H

void dlerror(const char *format, ...);
void dlexit(const char *format, ...);
void dlmessage(const char *format, ...);
void dlabort(const char *format, ...);

#endif
