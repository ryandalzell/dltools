#ifndef DLFORMAT_H
#define DLFORMAT_H

#include "dlutil.h"
#include "dlsource.h"

/* virtual base class for data format decoders */
class dlformat
{
public:
    dlformat();
    virtual ~dlformat();

    /* format operators */
    virtual int attach(dlsource *source);

    /* copy to buffer read */
    virtual size_t read(unsigned char *buf, size_t bytes);
    /* zero copy read (depending on implementation) */
    virtual const unsigned char *read(size_t *bytes);

    /* expose source interfaces */
    dlsource *get_source();

    /* format metadata */
    virtual const char *description() { return "raw"; }

protected:
    /* data source */
    dlsource *source;

    /* buffer variables */
    size_t size;
    unsigned char *data;

};

/* elementary stream format decoder class */
class dlestream : public dlformat
{
public:
    /* copy to buffer read */
    //virtual size_t read(unsigned char *buf, size_t bytes);
    /* zero copy read (depending on implementation) */
    //virtual const unsigned char *read(size_t *bytes);

    /* format metadata */
    virtual const char *description() { return "elementary stream"; }
};

/* transport stream format decoder class */
class dltstream : public dlformat
{
public:
    dltstream(int pid);
    virtual ~dltstream();

    /* format operators */
    virtual int attach(dlsource *source);

    /* copy to buffer read */
    virtual size_t read(unsigned char *buf, size_t bytes);
    /* zero copy read (depending on implementation) */
    virtual const unsigned char *read(size_t *bytes);

    /* format metadata */
    virtual const char *description() { return "transport stream"; }

protected:
    int pid;
};

#endif
