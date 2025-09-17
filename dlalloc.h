#ifndef DLALLOC_H
#define DLALLOC_H

#define POOLSIZE 256

/* custom video buffer for dltools */
class dlvideobuf : public IDeckLinkVideoBuffer
{
private:
    unsigned refcnt;
    void *buf;
    unsigned index;

public:
    dlvideobuf(void *ptr) : refcnt(0), buf(ptr) {}
    dlvideobuf(void *ptr, unsigned index) : refcnt(0), buf(ptr), index(index) {}
    ~dlvideobuf() {free(buf);}

    /* implementation of IUnknown */
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) {return E_NOINTERFACE;}
    virtual ULONG STDMETHODCALLTYPE   AddRef();
    virtual ULONG STDMETHODCALLTYPE   Release();

    /* implementation of IDeckLinkVideoBuffer */
    virtual HRESULT GetBytes(void** buffer) {*buffer = buf; return S_OK;}
    virtual HRESULT StartAccess(BMDBufferAccessFlags flags) {return S_OK;}
    virtual HRESULT EndAccess(BMDBufferAccessFlags flags) {return S_OK;}
};

/* custom memory allocator for dltools */
class dlalloc : public IDeckLinkVideoBufferAllocator
{
public:
    dlalloc();
    ~dlalloc();

    /* implementation */
    void init(uint32_t bufferSize);

    /* implementation of IUnknown */
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) {return E_NOINTERFACE;}
    virtual ULONG STDMETHODCALLTYPE   AddRef();
    virtual ULONG STDMETHODCALLTYPE   Release();

    /* implementation of IDeckLinkMemoryAllocator */
    virtual HRESULT STDMETHODCALLTYPE AllocateVideoBuffer(IDeckLinkVideoBuffer **allocated);
    //virtual HRESULT STDMETHODCALLTYPE ReleaseVideoBuffer(void *buf);

private:
    unsigned refcnt;
    uint32_t bufsize;
    dlvideobuf *pool[POOLSIZE];
    unsigned index;
};

#endif // DLALLOC_H
