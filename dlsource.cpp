/*
 * Description: object interfaces to source data streams.
 * Author     : Ryan Dalzell
 * Copyright  : (c) 2015 4i2i Communications Ltd.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <fcntl.h>

#include "dlutil.h"
#include "dlsource.h"

using namespace std;

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

off_t dlsource::pos(dltoken_t t)
{
    return 0;
}

bool dlsource::eof(dltoken_t t)
{
    return 1;
}

bool dlsource::error(dltoken_t t)
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
}

dlfile::~dlfile()
{
    for (unsigned i=0; i<file.size(); i++)
        close(file[i]);
}

int dlfile::open(const char *name)
{
    /* attach the input file */
    filename = name;

    /* open the input file FIXME just use attach() */
    int f = ::open(filename, O_RDONLY | O_LARGEFILE);
    if (f<0)
        dlerror("error: failed to open input file \"%s\"", filename);
    file.push_back(f);
    eof_flag.push_back(0);
    error_flag.push_back(0);

    return 0;
}

int dlfile::rewind(dltoken_t t)
{
    int r = lseek(file[t], 0, SEEK_SET);
    if (r<0)
        dlerror("failed to seek in file \"%s\"", filename);

    return r;
}

filetype_t dlfile::autodetect()
{
    /* determine the file type from the filename suffix */
    if (strstr(filename, ".m2v")!=NULL || strstr(filename, ".M2V")!=NULL)
        return M2V;
    else if (strstr(filename, ".264")!=NULL || strstr(filename, ".h264")!=NULL || strstr(filename, ".avc")!=NULL)
        return AVC;
    else if (strstr(filename, ".265")!=NULL || strstr(filename, ".h265")!=NULL || strstr(filename, ".hevc")!=NULL)
        return HEVC;
    else if (strstr(filename, ".ts")!=NULL || strstr(filename, ".trp")!=NULL || strstr(filename, ".mpg")!=NULL)
        return TS;

    return YUV;
}

dltoken_t dlfile::attach()
{
    /* open the input file again */
    int f = ::open(filename, O_RDONLY | O_LARGEFILE);
    if (f<0)
        dlerror("error: failed to open input file \"%s\"", filename);
    file.push_back(f);
    eof_flag.push_back(0);
    error_flag.push_back(0);

    return (dltoken_t) file.size()-1;
}

size_t dlfile::read(unsigned char *buf, size_t bytes, dltoken_t t)
{
    size_t read = ::read(file[t], buf, bytes);
    if (read<0)
        dlerror("error: failed to read from input file \"%s\"", filename);
    else if (read==0)
        eof_flag[t] = 1;

    return read;
}

const unsigned char* dlfile::read(size_t *bytes, dltoken_t t)
{
    /* check internal buffer is large enough */
    checksize(*bytes);

    size_t read = ::read(file[t], buffer, *bytes);
    if (read<0)
        dlerror("error: failed to read from input file \"%s\"", filename);
    else if (read==0)
        eof_flag[t] = 1;
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
    fstat(file[0], &stat);

    return stat.st_size;
}

off_t dlfile::pos(dltoken_t t)
{
    off_t r = lseek(file[t], 0, SEEK_CUR);
    if (r<0)
        error_flag[t] = 1;
    return r;
}

bool dlfile::eof(dltoken_t t)
{
    return eof_flag[t];
}

bool dlfile::error(dltoken_t t)
{
    return error_flag[t];
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
    if (length==0)
        dlexit("error: input file is empty");
    addr = (unsigned char *)mmap(NULL, length, PROT_READ, MAP_PRIVATE, file[0], 0);
    if (addr==MAP_FAILED)
        dlerror("error: failed to memory map input file \"%s\"", filename);
    ptr = addr;

    return 0;
}

int dlmmap::rewind(dltoken_t t)
{
    ptr = addr;

    return 0;
}

/* read with copy */
size_t dlmmap::read(unsigned char *buf, size_t bytes, dltoken_t t)
{
    if (ptr+bytes>addr+length)
        bytes = addr+length-ptr;

    /* kind of defeats the point of mmap */
    memcpy(buf, ptr, bytes);
    ptr += bytes;

    return bytes;
}

/* zero copy read using memory mapped pointer */
const unsigned char *dlmmap::read(size_t *bytes, dltoken_t t)
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

off_t dlmmap::pos(dltoken_t t)
{
    return ptr-addr;
}

bool dlmmap::eof(dltoken_t t)
{
    return ptr>=addr+length;
}

bool dlmmap::error(dltoken_t t)
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

int dlsock::rewind(dltoken_t t)
{
    /* can't rewind a network stream */
    return -1;
}

filetype_t dlsock::autodetect()
{
    /* a timeout is not useful information here */
    set_timeout(0);

    /* try to determine data type from contents */
    int bytes = read(buffer, bufsize);
    for (int i=0; i<bytes; i++) {
        if (buffer[i]==0x47 && (i+bytes<188 || buffer[i+188]==0x47)) {
            //dlmessage("found transport packet in network stream");
            return TS;
        }

        if (buffer[i]==0x00 && buffer[i+1]==0x00 && buffer[i+2]==0x01 && buffer[i+3]==0xb3) {
            //dlmessage("found mpeg2 start code in network stream");
            return M2V;
        }

        if (buffer[i]==0x00 && buffer[i+1]==0x00 && buffer[i+2]==0x01) {
            /* a bit prone to false positives */
            //dlmessage("found hevc start code in network stream");
            return HEVC;
        }
    }

    /* can't rewind network stream after autodetect, but then again, there's plenty of data */

    return OTHER;
}

dltoken_t dlsock::attach()
{
    return (dltoken_t) 0;
}

size_t dlsock::read(unsigned char *buf, size_t bytes, dltoken_t t)
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

const unsigned char *dlsock::read(size_t *bytes, dltoken_t t)
{
    /* check internal buffer is large enough */
    checksize(*bytes);

    size_t read = dlsock::read(buffer, *bytes);
    if (read!=*bytes)
        *bytes = read;

    return buffer;
}

bool dlsock::eof(dltoken_t token)
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

size_t dltcpsock::read(unsigned char *buf, size_t bytes, dltoken_t t)
{
    size_t read = recv(send_sock, buf, bytes, 0);
    if (read<0)
        dlerror("error: failed to read from socket");
    else if (read==0)
        dlmessage("connection closed by encoder");

    return read;
}

const unsigned char* dltcpsock::read(size_t* bytes, dltoken_t t)
{
    /* check internal buffer is large enough */
    checksize(*bytes);

    size_t read = dltcpsock::read(buffer, *bytes);
    if (read!=*bytes)
        *bytes = read;

    return buffer;

}
