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
    virtual size_t read(unsigned char *buffer, size_t bufsize) = 0;

    /* source metadata */
    virtual const char *name();
    virtual size_t size();
    virtual size_t pos();
    virtual size_t eof();
    virtual size_t error();

protected:
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
    virtual size_t read(unsigned char *buffer, size_t bufsize);

    /* source metadata */
    virtual const char *name();
    virtual size_t size();
    virtual size_t pos();
    virtual size_t eof();
    virtual size_t error();

protected:
    /* file and buffer variables */
    const char *filename;
    FILE *file;
};

/* network socket source class */
class dlsock : public dlsource
{
public:
    dlsock();
    ~dlsock();

    /* source operators */
    virtual int open(const char *port);
    virtual int rewind();
    virtual size_t read(unsigned char *buffer, size_t bufsize);

    /* source metadata */
    virtual size_t eof();

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
    virtual size_t read(unsigned char *buffer, size_t bufsize);

protected:
    int send_sock;
};

#endif
