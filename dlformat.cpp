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
    token = source->attach();
    return 0;
}

size_t dlformat::read(unsigned char *buf, size_t bytes)
{
    size_t size = source->read(buf, bytes, token);
    if (size!=bytes) {
        /* no timestamp so simply loop input */
        source->rewind(token);
        size = source->read(buf, bytes, token);
    }
    return size;
}

const unsigned char *dlformat::read(size_t *bytes)
{
    size_t size = *bytes;
    const unsigned char *data = source->read(bytes, token);
    if (data==NULL || size!=*bytes) {
        /* no timestamp so simply loop input */
        source->rewind(token);
        *bytes = size; /* discard previous read */
        data = source->read(bytes, token);
    }
    return data;
}

long long dlformat::get_timestamp()
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
dltstream::dltstream(int p)
{
    source = NULL;
    pid = p;
    pts = -1ll;
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
    token = source->attach();

    /* allocate the read buffer */
    size = 184;
    data = (unsigned char *) malloc(size);

    return 0;
}

size_t dltstream::read(unsigned char *buf, size_t bytes)
{
    size_t size = 0;

    /* on first read of the data stream skip until the pts is initialised */
    do {
        long long new_pts;
        size = next_pes_packet_data(buf, &new_pts, pid, 0, source, token);
        if (new_pts>=0)
            pts = new_pts;
    } while (pts<0);
    return size;
}

const unsigned char *dltstream::read(size_t *bytes)
{
    do {
        long long new_pts;
        *bytes = next_pes_packet_data(data, &new_pts, pid, 0, source, token);
        if (new_pts>=0)
            pts = new_pts;
    } while (pts<0);
    return data;
}

long long int dltstream::get_timestamp()
{
    return pts;
}

