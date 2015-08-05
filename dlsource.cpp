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

#include "dlutil.h"
#include "dlsource.h"

/* virtual base class for data sources */
dlsource::dlsource()
{ }

dlsource::~dlsource()
{ }

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

size_t dlsource::eof()
{
    return 1;
}

size_t dlsource::error()
{
    return 0;
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

size_t dlfile::read(unsigned char *buffer, size_t bufsize)
{
    size_t read = fread(buffer, 1, bufsize, file);
    if (read<0)
        dlerror("error: failed to read from input file \"%s\"", filename);

    return read;
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

size_t dlfile::eof()
{
    return feof(file);
}

size_t dlfile::error()
{
    return ferror(file);
}

/* network socket source class */
dlsock::dlsock()
{
    sock = -1;
    multicast = NULL;
    interface = NULL;
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

    /* increase the receive buffer size of the socket */
    int recvbufsize = 131071;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvbufsize, sizeof(recvbufsize)) == -1)
        dlerror("failed to set socket receive buffer size");
    recvbufsize = 0;
    socklen_t optlen = sizeof(recvbufsize);
    getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvbufsize, &optlen);
    dlmessage("using socket receive buffer of %d bytes", recvbufsize);

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
    if (multicast)
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

size_t dlsock::read(unsigned char *buffer, size_t bufsize)
{
    size_t read = recvfrom(sock, buffer, bufsize, 0, NULL, 0);
    if (read<0)
        dlerror("error: failed to read from socket");
    else if (read==0)
        dlmessage("zero sized packet received");

    return read;
}

size_t dlsock::eof()
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

size_t dltcpsock::read(unsigned char *buffer, size_t bufsize)
{
    size_t read = recv(send_sock, buffer, sizeof(buffer), 0);
    if (read<0)
        dlerror("error: failed to read from socket");
    else if (read==0)
        dlmessage("connection closed by encoder");

    return read;
}
