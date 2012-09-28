#ifndef DLALLOC_H
#define DLALLOC_H

#define POOLSIZE 256

/* custom memory allocator for dltools */
class dlalloc : public IDeckLinkMemoryAllocator
{
public:
    dlalloc();
    ~dlalloc();

    /* implementation of IUnknown */
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) {return E_NOINTERFACE;}
    virtual ULONG STDMETHODCALLTYPE   AddRef();
    virtual ULONG STDMETHODCALLTYPE   Release();

    /* implementation of IDeckLinkMemoryAllocator */
    virtual HRESULT STDMETHODCALLTYPE AllocateBuffer(unsigned int size, void **buf);
    virtual HRESULT STDMETHODCALLTYPE ReleaseBuffer(void *buf);
    virtual HRESULT STDMETHODCALLTYPE Commit();
    virtual HRESULT STDMETHODCALLTYPE Decommit();

private:
    unsigned refcnt;
    void *pool[POOLSIZE];
    unsigned index;
    void *heap[POOLSIZE];
    unsigned spare;
};

#endif // DLALLOC_H