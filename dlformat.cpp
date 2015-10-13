/*
 * Description: container format decoder.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2015 4i2i Communications Ltd.
 */

#include <stdlib.h>
#include <assert.h>

#include "dlformat.h"
#include "dlts.h"

/* virtual base class for data format decoders */
dlformat::dlformat()
{
    source = NULL;
    data = NULL;
    size = 0;
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

size_t dlformat::read(unsigned char *buf, size_t bytes)
{
    return source->read(buf, bytes);
}

const unsigned char *dlformat::read(size_t *bytes)
{
    return source->read(bytes);
}

dlsource* dlformat::get_source()
{
    return source;
}

/* elementary stream format decoder class */
/* transport stream format decoder class */
dltstream::dltstream(int p)
{
    source = NULL;
    pid = p;
    data = NULL;
    size = 0;
}

dltstream::~dltstream()
{
    if (data)
        free(data);
}

int dltstream::attach(dlsource *s)
{
    /* attach the input source */
    source = s;

    /* allocate the read buffer */
    size = 184;
    data = (unsigned char *) malloc(size);

    return 0;
}

size_t dltstream::read(unsigned char *buf, size_t bytes)
{
    long long pts;
    return next_pes_packet_data(buf, &pts, pid, 0, source);
}

const unsigned char *dltstream::read(size_t *bytes)
{
    long long pts;
    *bytes = next_pes_packet_data(data, &pts, pid, 0, source);
    return data;
}
