#ifndef DLSOURCE_H
#define DLSOURCE_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "dlutil.h"

/* virtual base class for data sources */
class dlsource
{
public:
    dlsource();
    virtual ~dlsource();

    /* source operators */
    virtual int open(const char *filename) = 0;
    virtual int rewind() = 0;
    /* copy to buffer read */
    virtual size_t read(unsigned char *buf, size_t bytes) = 0;
    /* zero copy read (depending on implementation) */
    virtual const unsigned char *read(size_t *bytes) = 0;

    /* source metadata */
    virtual const char *name();
    virtual size_t size();
    virtual size_t pos();
    virtual bool eof();
    virtual bool error();
    virtual bool timeout();

    /* source configuration */
    virtual void set_timeout(int timeout_usec);

protected:
    /* memory buffer management */
    /*const*/ unsigned char *buffer, *bufptr;
    unsigned bufsize, bytesleft;
    void checksize(size_t size);

    /* timeout parameters */
    int time_out; // in usecs.
    int timed_out;
};

/* file source class */
class dlfile : public dlsource
{
public:
    dlfile();
    ~dlfile();

    /* source operators */
    virtual int open(const char *filename);
    virtual int rewind();
    virtual size_t read(unsigned char *buf, size_t bytes);
    virtual const unsigned char *read(size_t* bytes);

    /* source metadata */
    virtual const char *name();
    virtual size_t size();
    virtual size_t pos();
    virtual bool eof();
    virtual bool error();

protected:
    /* file and buffer variables */
    const char *filename;
    FILE *file;
};

/* memory mapped file souce class */
class dlmmap : public dlfile
{
public:
    dlmmap();
    ~dlmmap();

    /* source operators */
    virtual int open(const char *filename);
    virtual int rewind();
    virtual size_t read(unsigned char *buf, size_t bytes);
    virtual const unsigned char *read(size_t *bytes);

    /* source metadata */
    virtual size_t size();
    virtual size_t pos();
    virtual bool eof();
    virtual bool error();

protected:
    /* memory map variables */
    unsigned char *addr, *ptr;
    size_t length;
};

/* network socket source class */
class dlsock : public dlsource
{
public:
    dlsock();
    dlsock(const char *address);                        /* multicast */
    dlsock(const char *address, const char *interface); /* multi-homed multicast */
    ~dlsock();

    /* source operators */
    virtual int open(const char *port);
    virtual int rewind();
    virtual size_t read(unsigned char *buf, size_t bytes);
    virtual const unsigned char *read(size_t *bytes);

    /* source metadata */
    virtual bool eof();

protected:
    int sock;
    socklen_t addr_len;
    struct sockaddr_in name, sender;
    const char *multicast, *interface;
};

/* network tcp socket source class */
class dltcpsock : public dlsock
{
public:
    dltcpsock();
    ~dltcpsock();

    /* source operators */
    virtual int open(const char *port);
    virtual size_t read(unsigned char *buf, size_t bytes);
    virtual const unsigned char *read(size_t *bytes);

protected:
    int send_sock;
};

#endif
