/*
 * Description: custom memory allocator
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2012 4i2i Communications Ltd.
 */

#include <stdlib.h>
#include <string.h>

#include "dlutil.h"
#include "dlalloc.h"

dlalloc::dlalloc()
{
    refcnt = 0;
    index = 0;
}

dlalloc::~dlalloc()
{
    Decommit();
}

ULONG STDMETHODCALLTYPE dlalloc::AddRef()
{
    return ++refcnt;
}

ULONG STDMETHODCALLTYPE dlalloc::Release()
{
    return --refcnt;
}

HRESULT STDMETHODCALLTYPE dlalloc::AllocateBuffer(unsigned int size, void **buf)
{
    /* first try to reuse a spare buffer */
    if (spare>0) {
        *buf = heap[--spare];
        return S_OK;
    }

    /* else allocate a new buffer */
    if (index==POOLSIZE)
        return E_OUTOFMEMORY;

    if (posix_memalign(&pool[index], 1024, size) != 0)
        return E_OUTOFMEMORY;

    *buf = pool[index];
    index++;

    return S_OK;
}

HRESULT STDMETHODCALLTYPE dlalloc::ReleaseBuffer(void *buf)
{
    /* put buffer on spare heap */
    heap[spare++] = buf;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE dlalloc::Commit()
{
    index = 0;
    spare = 0;
    memset(pool, 0, sizeof(void *) * POOLSIZE);
    memset(heap, 0, sizeof(void *) * POOLSIZE);

    return S_OK;
}

HRESULT STDMETHODCALLTYPE dlalloc::Decommit()
{
    unsigned i;

    for (i=0; i<index; i++)
        if (pool[i])
            free(pool[i]);

    index = 0;
    memset(pool, 0, sizeof(void *) * POOLSIZE);

    return S_OK;
}
