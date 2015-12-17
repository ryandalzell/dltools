#ifndef DLFORMAT_H
#define DLFORMAT_H

#include <vector>

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
    virtual size_t read(unsigned char *buf, size_t bytes, int mux=0);
    /* zero copy read (depending on implementation) */
    virtual const unsigned char *read(size_t *bytes, int mux=0);

    /* return most recent timestamp */
    virtual long long get_timestamp(int pid);

    /* expose source interfaces */
    dlsource *get_source();

    /* format metadata */
    virtual const char *description() { return "raw"; }

protected:
    /* data source */
    dlsource *source;

};

/* elementary stream format decoder class */
class dlestream : public dlformat
{
public:
    /* copy to buffer read */
    //virtual size_t read(unsigned char *buf, size_t bytes, int mux=0);
    /* zero copy read (depending on implementation) */
    //virtual const unsigned char *read(size_t *bytes, int mux=0);

    /* format metadata */
    virtual const char *description() { return "elementary stream"; }
};

/* transport stream format decoder class */
class dltstream : public dlformat
{
public:
    dltstream();
    virtual ~dltstream();

    /* register pids NOTE non-virtual */
    void register_pid(int pid);

    /* format operators */
    virtual int attach(dlsource *source);

    /* copy to buffer read */
    virtual size_t read(unsigned char *buf, size_t bytes, int pid=0);
    /* zero copy read (depending on implementation) */
    virtual const unsigned char *read(size_t *bytes, int pid=0);

    /* return most recent pts */
    virtual long long get_timestamp(int pid);

    /* format metadata */
    virtual const char *description() { return "transport stream"; }

protected:
    struct pid_t {
        int pid;
        size_t bytes;
        unsigned char *data;
        size_t size;
        long long pts;
    };
    std::vector<pid_t> pids;
    pid_t *find_pid(int pid);

private:
    size_t process(size_t bytes, dltstream::pid_t &pid, bool start=false);
};

#endif
