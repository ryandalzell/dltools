#ifndef DLUTIL_H
#define DLUTIL_H

/* chroma formats */
typedef enum {
    UNKNOWN = 0,
    LUMA,
    YUV420,
    YUV420mpeg,
    YUV420jpeg,
    YUV420dv,
    YUV422,
    YUV444,
} chromaformat_t;

extern const char *chromaformatname[];

void dlerror(const char *format, ...);
void dlexit(const char *format, ...);
void dlmessage(const char *format, ...);
void dlabort(const char *format, ...);

#endif
