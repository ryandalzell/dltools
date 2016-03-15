#ifndef DLSOURCE_H
#define DLSOURCE_H

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <vector>

#include "dlutil.h"

/* type of token returned when attaching format decoders */
typedef int dltoken_t;

/* virtual base class for data sources */
class dlsource
{
public:
    dlsource();
    virtual ~dlsource();

    /* source operators */
    virtual int open(const char *filename) = 0;
    virtual int rewind(dltoken_t token=0) = 0;
    virtual filetype_t autodetect() = 0;
    virtual dltoken_t attach() = 0;

    /* copy to buffer read */
    virtual size_t read(unsigned char *buf, size_t bytes, dltoken_t token=0) = 0;
    /* zero copy read (depending on implementation) */
    virtual const unsigned char *read(size_t *bytes, dltoken_t token=0) = 0;

    /* source metadata */
    virtual const char *description() { return "unknown"; }
    virtual const char *name();
    virtual size_t size();
    virtual off_t pos(dltoken_t token=0);
    virtual bool eof(dltoken_t token=0);
    virtual bool error(dltoken_t token=0);
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
    virtual int rewind(dltoken_t token=0);
    virtual filetype_t autodetect();
    virtual dltoken_t attach();
    virtual size_t read(unsigned char *buf, size_t bytes, dltoken_t token=0);
    virtual const unsigned char *read(size_t* bytes, dltoken_t token=0);

    /* source metadata */
    virtual const char *description() { return "file"; }
    virtual const char *name();
    virtual size_t size();
    virtual off_t pos(dltoken_t token);
    virtual bool eof(dltoken_t token);
    virtual bool error(dltoken_t token);

protected:
    /* file and buffer variables */
    const char *filename;
    std::vector<int> file;

    /* status flags */
    std::vector<int> eof_flag, error_flag;
};

/* memory mapped file souce class */
class dlmmap : public dlfile
{
public:
    dlmmap();
    ~dlmmap();

    /* source operators */
    virtual int open(const char *filename);
    virtual int rewind(dltoken_t token=0);
    virtual size_t read(unsigned char *buf, size_t bytes, dltoken_t token=0);
    virtual const unsigned char *read(size_t *bytes, dltoken_t token=0);

    /* source metadata */
    virtual size_t size();
    virtual off_t pos(dltoken_t token);
    virtual bool eof(dltoken_t token);
    virtual bool error(dltoken_t token);

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
    virtual int rewind(dltoken_t token=0);
    virtual filetype_t autodetect();
    virtual dltoken_t attach();
    virtual size_t read(unsigned char *buf, size_t bytes, dltoken_t token=0);
    virtual const unsigned char *read(size_t *bytes, dltoken_t token=0);

    /* source metadata */
    virtual const char *description() { return "udp"; }
    virtual bool eof(dltoken_t token);

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
    virtual size_t read(unsigned char *buf, size_t bytes, dltoken_t token=0);
    virtual const unsigned char *read(size_t *bytes, dltoken_t token=0);

    /* source metadata */
    virtual const char *description() { return "tcp"; }

protected:
    int send_sock;
};

#endif
