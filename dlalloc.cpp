/*
 * Description: custom memory allocator
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2012 4i2i Communications Ltd.
 */

#include <stdlib.h>
#include <string.h>

#include "dlutil.h"
#include "dlalloc.h"

/* there must be a better way than this! */
class dlvideobuf;
static dlvideobuf *heap[POOLSIZE];
static unsigned spare;

ULONG STDMETHODCALLTYPE dlvideobuf::AddRef()
{
    //dlmessage("videobuf %2d addref: refcnt=%d", index, refcnt+1);
    return ++refcnt;
}

ULONG STDMETHODCALLTYPE dlvideobuf::Release()
{
    ULONG ret = --refcnt;
    //dlmessage("videobuf %2d release: refcnt=%d", index, refcnt);
    if (!ret) {
        /* put buffer on spare heap */
        heap[spare++] = this;
    }
    return ret;
}

dlalloc::dlalloc()
{
    refcnt = 1;
    bufsize = 0;

    /* init pool */
    index = 0;
    spare = 0;
    memset(pool, 0, sizeof(dlvideobuf *) * POOLSIZE);
    memset(heap, 0, sizeof(dlvideobuf *) * POOLSIZE);
}

dlalloc::~dlalloc()
{
    unsigned i;

    for (i=0; i<index; i++)
        if (pool[i])
            delete pool[i];

    index = 0;
    memset(pool, 0, sizeof(void *) * POOLSIZE);
}

void dlalloc::init(uint32_t bufferSize)
{
    bufsize = bufferSize;
}

ULONG STDMETHODCALLTYPE dlalloc::AddRef()
{
    return ++refcnt;
}

ULONG STDMETHODCALLTYPE dlalloc::Release()
{
    return --refcnt;
}

HRESULT dlalloc::AllocateVideoBuffer(IDeckLinkVideoBuffer ** allocated)
{
    if (!allocated)
        return E_POINTER;
    if (bufsize==0)
        return E_UNEXPECTED;

    /* first try to reuse a spare buffer */
    if (spare>0) {
        *allocated = heap[--spare];
        return S_OK;
    }

    /* else allocate a new buffer */
    if (index==POOLSIZE) {
        dlmessage("all buffers in pool of size %d are allocated", POOLSIZE);
        return E_OUTOFMEMORY;
    }

    void *buf;
    if (posix_memalign(&buf, 1024, bufsize) != 0)
        return E_OUTOFMEMORY;
    pool[index] = new dlvideobuf(buf, index);

    *allocated = pool[index];
    index++;

    return S_OK;
}
