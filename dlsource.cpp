/*
 * Description: object interfaces to source data streams.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2015 4i2i Communications Ltd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/select.h>

#include "dlutil.h"
#include "dlsource.h"

/* virtual base class for data sources */
dlsource::dlsource()
{
    bufsize = 128*1024; // 64K is max UDP datagram size.
    buffer = (unsigned char *)malloc(bufsize);
    bufptr = buffer;
    bytesleft = 0;
    time_out = 0;
    timed_out = 0;
}

dlsource::~dlsource()
{
    if (buffer)
        free(buffer);
}

const char *dlsource::name()
{
    return "unknown";
}

size_t dlsource::size()
{
    return 0;
}

size_t dlsource::pos()
{
    return 0;
}

bool dlsource::eof()
{
    return 1;
}

bool dlsource::error()
{
    return 0;
}

bool dlsource::timeout()
{
    return timed_out;
}

void dlsource::set_timeout(int timeout_usec)
{
    time_out = timeout_usec;
}

/* resize buffer if size is larger than the current size */
void dlsource::checksize(size_t size)
{
    if (size>bufsize) {
        size_t offset = bufptr - buffer;
        buffer = (unsigned char *) realloc(buffer, size);
        bufsize = size;
        bufptr = buffer + offset;
    }
}

/* file source class */
dlfile::dlfile()
{
    filename = NULL;
    file = NULL;
}

dlfile::~dlfile()
{
    if (file)
        fclose(file);
}

int dlfile::open(const char *f)
{
    /* attach the input file */
    filename = f;

    /* open the input file */
    file = fopen(filename, "rb");
    if (!file)
        dlerror("error: failed to open input file \"%s\"", filename);

    return 0;
}

int dlfile::rewind()
{
    int r = fseek(file, 0, SEEK_SET);
    if (r<0)
        dlerror("failed to seek in file \"%s\"", filename);

    return r;
}

filetype_t dlfile::autodetect()
{
    /* determine the file type from the filename suffix */
    if (strstr(filename, ".m2v")!=NULL || strstr(filename, ".M2V")!=NULL)
        return M2V;
    else if (strstr(filename, ".265")!=NULL || strstr(filename, ".h265")!=NULL || strstr(filename, ".hevc")!=NULL)
        return HEVC;
    else if (strstr(filename, ".ts")!=NULL || strstr(filename, ".trp")!=NULL || strstr(filename, ".mpg")!=NULL)
        return TS;

    return YUV;
}

size_t dlfile::read(unsigned char *buf, size_t bytes)
{
    size_t read = fread(buf, 1, bytes, file);
    if (read<0)
        dlerror("error: failed to read from input file \"%s\"", filename);

    return read;
}

const unsigned char* dlfile::read(size_t *bytes)
{
    /* check internal buffer is large enough */
    checksize(*bytes);

    size_t read = fread(buffer, 1, *bytes, file);
    if (read<0)
        dlerror("error: failed to read from input file \"%s\"", filename);
    *bytes = read;

    return buffer;
}

const char *dlfile::name()
{
    return filename;
}

size_t dlfile::size()
{
    /* stat the input file */
    struct stat stat;
    fstat(fileno(file), &stat);

    return stat.st_size;
}

size_t dlfile::pos()
{
    return ftello(file);
}

bool dlfile::eof()
{
    return feof(file);
}

bool dlfile::error()
{
    return ferror(file);
}

/* memory mapped file source class */
dlmmap::dlmmap()
{
    ptr = NULL;
}

dlmmap::~dlmmap()
{
    munmap(addr, length);
}

int dlmmap::open(const char *f)
{
    /* open the input file */
    dlfile::open(f);

    /* memory map the open file */
    length = dlfile::size();
    addr = (unsigned char *)mmap(NULL, length, PROT_READ, MAP_PRIVATE, fileno(file), 0);
    if (!addr)
        dlerror("error: failed to memory map input file \"%s\"", filename);
    ptr = addr;

    return 0;
}

int dlmmap::rewind()
{
    ptr = addr;

    return 0;
}

/* read with copy */
size_t dlmmap::read(unsigned char *buf, size_t bytes)
{
    if (ptr+bytes>addr+length)
        bytes = addr+length-ptr;

    /* kind of defeats the point of mmap */
    memcpy(buf, ptr, bytes);
    ptr += bytes;

    return bytes;
}

/* zero copy read using memory mapped pointer */
const unsigned char *dlmmap::read(size_t *bytes)
{
    const unsigned char *ret = ptr;
    if (ptr+*bytes>addr+length)
        *bytes = addr+length-ptr;
    ptr += *bytes;

    return ret;
}

size_t dlmmap::size()
{
    return length;
}

size_t dlmmap::pos()
{
    return ptr-addr;
}

bool dlmmap::eof()
{
    return ptr>=addr+length;
}

bool dlmmap::error()
{
    return 0;
}

/* network socket source class */
dlsock::dlsock()
{
    sock = -1;
    multicast = NULL;
    interface = NULL;
}

dlsock::dlsock(const char *a)
{
    sock = -1;
    multicast = a;
    interface = NULL;
}

dlsock::dlsock(const char *a, const char *i)
{
    sock = -1;
    multicast = a;
    interface = i;
}

dlsock::~dlsock()
{
    if (sock>=0)
        close(sock);
}

int dlsock::open(const char *port)
{
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock<0)
        dlerror("error creating socket");

    name.sin_family = AF_INET;
    name.sin_port = htons(atoi(port));
    name.sin_addr.s_addr = multicast? inet_addr(multicast) : INADDR_ANY;

    /* assign a name to the socket */
    if (bind(sock, (struct sockaddr *)&name, sizeof name) < 0)
        dlerror("failed to bind socket");
    addr_len = sizeof(name);
    if (getsockname(sock, (struct sockaddr *)&name, &addr_len) < 0)
        dlerror("failed to get name of socket\n");

    /* find the default receive buffer size of the socket */
    int defbufsize = 0;
    socklen_t optlen = sizeof(defbufsize);
    getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &defbufsize, &optlen);

    /* increase the receive buffer size of the socket
     *
     * use these to check linux has enough space:
     *
     * cat /proc/sys/net/core/rmem_max
     * cat /proc/sys/net/core/wmem_max
     *
     * if not you can add these lines to /etc/sysctl.conf
     *
     * net.core.rmem_max=52428800
     * net.core.wmem_max=52428800
     * net.core.rmem_default=52428800
     * net.core.wmem_default=52428800
     *
     * although the read buffer size is the one of interest here.
     *
     * */
    int recvbufsize = 16772100;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvbufsize, sizeof(recvbufsize)) == -1)
        dlerror("failed to set socket receive buffer size");
    recvbufsize = 0;
    getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvbufsize, &optlen);
    dlmessage("increasing socket receive buffer size from %d to %d bytes", defbufsize, recvbufsize); /* this will be x2 above, this a kernel fudge factor */

    /* join a multicast group */
    struct ip_mreqn mc_req;
    memset(&mc_req, 0, sizeof(mc_req));
    if (multicast) {
        /* construct an IGMP join request structure */
        mc_req.imr_multiaddr.s_addr = inet_addr(multicast);
        mc_req.imr_address.s_addr = interface? inet_addr(interface) : htonl(INADDR_ANY);

        /* send an ADD MEMBERSHIP message via setsockopt */
        if ((setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (void*) &mc_req, sizeof(mc_req))) < 0)
            dlerror("failed to send add membership message");
    }

    /* user feedback */
    if (multicast && interface)
        dlmessage("listening for multicast data from encoder on group %s on interface %s and port %s", multicast, interface, port);
    else if (multicast)
        dlmessage("listening for multicast data from encoder on group %s and port %s", multicast, port);
    else
        dlmessage("listening for data from encoder on port %s", port);

    return 0;
}

int dlsock::rewind()
{
    /* can't rewind a network stream */
    return -1;
}

filetype_t dlsock::autodetect()
{
    return TS;
}

size_t dlsock::read(unsigned char *buf, size_t bytes)
{
    /* check internal buffer is large enough */
    checksize(bytes);

    timed_out = 0;
    if (time_out) {
        /* wait until socket is ready, with timeout */
        fd_set rfds;
        struct timeval tv = { 0, (long)time_out };
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        int r = select(sock+1, &rfds, NULL, NULL, &tv);
        if (r<0)
            dlerror("error: failed to select on network socket");
        if (r==0) {
            dlmessage("timeout on network socket read");
            timed_out = 1;
            return -1;
        }
    }

    /* fill internal buffer */
    if (bytes>bytesleft) {
        if (bytesleft==0)
            bufptr = buffer;
        else if (bytesleft>0 && bytesleft<bufsize) {
            memmove(buffer, bufptr, bytesleft);
            bufptr = buffer;
        }

        /* calculate number of bytes to refill buffer */
        size_t fill = bufsize-bytesleft;

        /* refill buffer */
        size_t read = recvfrom(sock, buffer, fill, MSG_TRUNC, NULL, 0);
        if (read<0)
            dlerror("error: failed to read from socket");
        else if (read==0)
            dlmessage("zero sized packet received");
        else if (read>fill) {
            dlmessage("warning: %d bytes discarded", read-fill);
            read = fill;
        }

        bytesleft += read;
    }

    /* copy to caller buffer */
    size_t read = mmin(bytes, bytesleft);
    memcpy(buf, bufptr, read);
    bufptr += read;
    bytesleft -= read;

    return read;
}

const unsigned char *dlsock::read(size_t *bytes)
{
    /* check internal buffer is large enough */
    checksize(*bytes);

    size_t read = dlsock::read(buffer, *bytes);
    if (read!=*bytes)
        *bytes = read;

    return buffer;
}

bool dlsock::eof()
{
    /* never at eof with an open socket */
    return sock>=0? 0 : 1;
}

/* network tcp socket source class */
dltcpsock::dltcpsock()
{
    send_sock = -1;
}

dltcpsock::~dltcpsock()
{
    if (sock>=0)
        close(sock);
}

int dltcpsock::open(const char *port)
{
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock<0) {
        dlerror("error creating socket");
        exit(10);
    }

    name.sin_family = AF_INET;
    name.sin_addr.s_addr = INADDR_ANY;
    name.sin_port = htons(atoi(port));

    /* bind the listening socket */
    if (bind(sock, (struct sockaddr *)&name, sizeof(name))<0) {
        dlerror("failed to bind socket");
        exit(10);
    }

    /* mark socket passive */
    listen(sock, 1);

    /* find the default receive buffer size of the socket */
    int defbufsize = 0;
    socklen_t optlen = sizeof(defbufsize);
    getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &defbufsize, &optlen);

    /* increase the receive buffer size of the socket */
    int recvbufsize = 1048576;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvbufsize, sizeof(recvbufsize)) == -1)
        dlerror("failed to set socket receive buffer size");
    recvbufsize = 0;
    getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvbufsize, &optlen);
    dlmessage("increasing socket receive buffer size from %d to %d bytes", defbufsize, recvbufsize); /* this will be x2 above, this a kernel fudge factor */

    /* user feedback */
    dlmessage("waiting for connection from encoder on port %s", port);

    /* wait for connection */
    socklen_t addrlen = sizeof(sender);
    send_sock = accept(sock, (struct sockaddr *)&sender, &addrlen);
    if (send_sock<0) {
        dlerror("error on connection");
        exit(10);
    }
    dlmessage("connection from encoder at %s", inet_ntoa(sender.sin_addr));

    return 0;
}

size_t dltcpsock::read(unsigned char *buf, size_t bytes)
{
    size_t read = recv(send_sock, buf, bytes, 0);
    if (read<0)
        dlerror("error: failed to read from socket");
    else if (read==0)
        dlmessage("connection closed by encoder");

    return read;
}

const unsigned char* dltcpsock::read(size_t* bytes)
{
    /* check internal buffer is large enough */
    checksize(*bytes);

    size_t read = dltcpsock::read(buffer, *bytes);
    if (read!=*bytes)
        *bytes = read;

    return buffer;

}
