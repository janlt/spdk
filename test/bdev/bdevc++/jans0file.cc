/*
 * jans0file.cc
 *
 *  Created on: Jun 10, 2020
 *      Author: jan.lisowiec@intel.com
 */

#include <libaio.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include <iostream>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "spdk/stdinc.h"
#include "spdk/cpuset.h"
#include "spdk/queue.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/event.h"
#include "spdk/ftl.h"
#include "spdk/conf.h"
#include "spdk/env.h"

#include "bdev/Status.h"
#include "config/Config.h"
#include "config/Options.h"
#include "common/Logger.h"
#include "common/BlockingPoller.h"
#include "common/Poller.h"
#include "io/IoPoller.h"
#include "io/Io2Poller.h"
#include "bdev/SpdkIoEngine.h"
#include "io/Rqst.h"
#include "bdev/SpdkDevice.h"
#include "bdev/SpdkConf.h"
#include "bdev/SpdkBdev.h"
#include "bdev/SpdkIoBuf.h"
#include "bdev/SpdkCore.h"
#include "FileEmu.h"
#include "api/FutureBase.h"
#include "api/Future.h"
#include "api/ApiBase.h"
#include "api/SyncApi.h"
#include "api/AsyncApi.h"
#include "api/Api.h"

#include "jans0file.h"

using namespace std;
namespace po = boost::program_options;

static BdevCpp::Api *api = 0;
static BdevCpp::SyncApi *syncApi = 0;
static BdevCpp::AsyncApi *asyncApi = 0;
static bool initialized = false;
static mutex initMutex;

static AsyncIoContext io_contexts[1 << 16];

static unsigned short hash_io_context(io_context_t ctx) {
    unsigned short sc = (reinterpret_cast<uint64_t>(ctx) >> 12);
    //cout <<"ctx: " << ctx << " sc: " << sc << endl << flush;
    return sc;
}

static int sync_file_fds[1 << 16];
static int async_file_fds[1 << 16];

int jans_async_open(const char *name, int flags, mode_t mode) {
    if (initialized == false)
        ModInit();
    int ret = asyncApi->open(name, flags, mode);
    if (ret >= 0)
        async_file_fds[ret] = ret;
    return ret;
}

int jans_async_close(int fd) {
    if (initialized == false)
        ModInit();
    if (fd >= 0)
        async_file_fds[fd] = -1;
    return asyncApi->close(fd);
}

//static int old_fd_count = 0;
//static int new_fd_count = 0;
//static int on_fd_count = 0;

uint64_t jans_async_lseek(int fd, off_t pos, int whence) {
    if (initialized == false)
        ModInit();

    //if (!((on_fd_count++)%(1 << 16)))
        //cout << "ofdc " << old_fd_count << " nfdc " << new_fd_count << endl;

    if (async_file_fds[fd] == -1 && sync_file_fds[fd] == -1) {
        //old_fd_count++;
        return lseek(fd, pos, whence);
    }
    //new_fd_count++;
    return asyncApi->lseek(fd, pos, whence);
}

int jans_async_fsync(int fd) {
    if (initialized == false)
        ModInit();

    //if (!((on_fd_count++)%(1 << 16)))
        //cout << "ofdc " << old_fd_count << " nfdc " << new_fd_count << endl;

    if (async_file_fds[fd] == -1 && sync_file_fds[fd] == -1) {
        //old_fd_count++;
        return fsync(fd);
    }
    //new_fd_count++;
    return asyncApi->fsync(fd);
}

int jans_io_setup(int maxevents, io_context_t *ctxp) {
    if (initialized == false)
        ModInit();
    for (int i = 0 ; i < maxevents ; i++)
        ctxp[i] = reinterpret_cast<io_context *>(i + 10);
    return 0;
}

int jans_async_unlink(const char *pathname) {
    if (initialized == false)
        ModInit();
    return asyncApi->unlink(pathname);
}

int jans_sync_unlink(const char *pathname) {
    if (initialized == false)
        ModInit();
    return syncApi->unlink(pathname);
}

int jans_sync_rename(const char *oldpath, const char *newpath) {
    if (initialized == false)
        ModInit();
    return syncApi->rename(oldpath, newpath);
}

int jans_async_rename(const char *oldpath, const char *newpath) {
    if (initialized == false)
        ModInit();
    return asyncApi->rename(oldpath, newpath);
}

int jans_io_destroy(io_context_t ctx) {
    if (initialized == false)
        ModInit();
    unsigned short ctx_idx = hash_io_context(ctx);
    io_contexts[ctx_idx].freeFuts = CONTEXT_MAX_FUTS;
    io_contexts[ctx_idx].empty = true;
    io_contexts[ctx_idx].full = false;
    io_contexts[ctx_idx].head = 0;
    io_contexts[ctx_idx].tail = 0;
    for (long i = 0 ; i < CONTEXT_MAX_FUTS ; i++){
        io_contexts[ctx_idx].futs[i].fut = 0;
    }

    return 0;
}

void jans_io_prep_pread(struct iocb *iocb, int fd, void *buf, size_t count, long long offset) {
    memset(iocb, 0, sizeof(*iocb));
    iocb->aio_fildes = fd;
    iocb->aio_lio_opcode = IO_CMD_PREAD;
    iocb->aio_reqprio = 0;
    iocb->u.c.buf = buf;
    iocb->u.c.nbytes = count;
    iocb->u.c.offset = offset;
}

void jans_io_prep_pwrite(struct iocb *iocb, int fd, void *buf, size_t count, long long offset) {
    memset(iocb, 0, sizeof(*iocb));
    iocb->aio_fildes = fd;
    iocb->aio_lio_opcode = IO_CMD_PWRITE;
    iocb->aio_reqprio = 0;
    iocb->u.c.buf = buf;
    iocb->u.c.nbytes = count;
    iocb->u.c.offset = offset;
}

#if 1
//static int tot_submit = 0;
//static int submit_count = 0;
//static unsigned long long tot_data = 0;

// standard, lokcing, working with standard getevents locking
int jans_io_submit(io_context_t ctx, long nr, struct iocb *ios[]) {
    if (initialized == false)
        ModInit();

    unsigned short ctx_idx = hash_io_context(ctx);

    unique_lock<mutex> wlock(io_contexts[ctx_idx].opMutex);
    //cout << "jans_submit ctx: " << ctx << " ctx_idx: " << ctx_idx << endl << flush;

    if (io_contexts[ctx_idx].full == true || nr > io_contexts[ctx_idx].freeFuts)
        return -1;

    const long t = io_contexts[ctx_idx].tail;
    long h = io_contexts[ctx_idx].head;

    long ret = 0;
    for ( ;; ) {
        //cout << "jans_io_submit ios: " << ios[ret] << " size: " << 
	    //ios[ret]->u.c.nbytes << " ctx_idx: " << ctx_idx << endl << flush;
        io_contexts[ctx_idx].futs[h].opcode = ios[ret]->aio_lio_opcode;
        io_contexts[ctx_idx].futs[h].user_data = ios[ret];
        //io_contexts[ctx_idx].futs[h].data_size = ios[ret]->u.c.nbytes;
        //io_contexts[ctx_idx].futs[h].data = reinterpret_cast<char *>(ios[ret]->u.c.buf);
	//tot_data += ios[ret]->u.c.nbytes;

        switch (ios[ret]->aio_lio_opcode) {
        case IO_CMD_PREAD:
            io_contexts[ctx_idx].futs[h].fut = asyncApi->read(ios[ret]->aio_fildes,
                    static_cast<uint64_t>(ios[ret]->u.c.offset),
                    reinterpret_cast<char *>(ios[ret]->u.c.buf),
                    ios[ret]->u.c.nbytes, false);
                    //ios[ret]->u.c.nbytes);
            break;
        case IO_CMD_PWRITE:
            io_contexts[ctx_idx].futs[h].fut = asyncApi->write(ios[ret]->aio_fildes,
                    static_cast<uint64_t>(ios[ret]->u.c.offset),
                    reinterpret_cast<const char *>(ios[ret]->u.c.buf),
                    ios[ret]->u.c.nbytes, false);
                    //ios[ret]->u.c.nbytes);
            break;
        default:
            break;
        }

        if (!io_contexts[ctx_idx].futs[h].fut) {
            io_contexts[ctx_idx].futs[h].opcode = 0;
            io_contexts[ctx_idx].futs[h].user_data = 0;
            break;
        }

        ret++;	
        io_contexts[ctx_idx].empty = false;
        io_contexts[ctx_idx].freeFuts--;
	h++;
	h %= CONTEXT_MAX_FUTS;
	if (h == t) {
            io_contexts[ctx_idx].full = true;
            break;
        }

	if (ret >= nr)
            break;
    }
    io_contexts[ctx_idx].head = h;

    wlock.unlock();
    io_contexts[ctx_idx].opCv.notify_all();

    //tot_submit += nr;
    //if (!((submit_count++)%(1 << 16)))
        //cout << "JANS tot_submit " << tot_submit << " tot data: " << tot_data << endl;

    return ret;
}
#endif

int jans_io_cancel(io_context_t ctx, struct iocb *iocb, struct io_event *evt) {
    return 0;
}

// copy ready slots for no locking processing (not working with blocking futures)
static int jans_io_getevents_first(io_context_t ctx_id,
        long min_nr,
        long nr,
        struct io_event *events,
        struct timespec *timeout, AsyncIoEntry *io_entries, int *num_entries) {
    unsigned short ctx_idx = hash_io_context(ctx_id);

    unique_lock<mutex> wlock(io_contexts[ctx_idx].opMutex);

    const chrono::milliseconds tout(500);
    if (io_contexts[ctx_idx].opCv.wait_for(wlock, tout, [ctx_idx] { return !io_contexts[ctx_idx].empty; }) == false) {
        wlock.unlock();
        return 0;
    }

    //cout << "jans_getevents ctx_id: " << ctx_id << " ctx_idx: " << ctx_idx << endl << flush;
    if (io_contexts[ctx_idx].empty == true)
        return 0;

    long t = io_contexts[ctx_idx].tail;
    const long h = io_contexts[ctx_idx].head;
    
    long ret_futs = 0;
    if (io_contexts[ctx_idx].empty == true)
        return ret_futs;

    for ( ;; ) {
        if (!io_contexts[ctx_idx].futs[t].fut) {
            cout << "jans_io_getevents CORRUPTION t: " << t << " h: " << h << endl << flush;
            break;
        }
        io_entries[*num_entries].fut = io_contexts[ctx_idx].futs[t].fut;
        io_entries[*num_entries].opcode = io_contexts[ctx_idx].futs[t].opcode;
        io_entries[*num_entries].data = io_contexts[ctx_idx].futs[t].data;
        io_entries[*num_entries].data_size = io_contexts[ctx_idx].futs[t].data_size;
        io_entries[*num_entries].user_data = io_contexts[ctx_idx].futs[t].user_data;
        io_contexts[ctx_idx].futs[t].fut = 0;
        io_contexts[ctx_idx].futs[t].user_data = 0;
        (*num_entries)++;

	ret_futs++;
        io_contexts[ctx_idx].full = false;
        io_contexts[ctx_idx].freeFuts++;
	t++;
	t %= CONTEXT_MAX_FUTS;
	if (t == h) {
            io_contexts[ctx_idx].empty = true;
            break;
        }
	if (ret_futs >= nr)
            break;
    }
    io_contexts[ctx_idx].tail = t;

    return ret_futs;
}

//static int ev_count = 1;
//static int avg_io_qd = 0;

int jans_io_getevents(io_context_t ctx_id,
        long min_nr,
        long nr,
        struct io_event *events,
        struct timespec *timeout) {
    if (initialized == false)
        ModInit();

    AsyncIoEntry futs[CONTEXT_MAX_FUTS];
    int num_futs = 0;
    int ret = jans_io_getevents_first(ctx_id, min_nr, nr, events, timeout, futs, &num_futs);

    long ret_futs = 0;

#if 0
    if (num_futs > 0) {
        avg_io_qd += num_futs;
        if (!((ev_count++)%(1 << 16))) {
	    cout << "avg_qd: " << avg_io_qd/ev_count << endl;
            ev_count = 1;
            avg_io_qd = 0;
        }
    }
#endif

    for (int i = 0 ; i < num_futs ; i++) {
        if (!futs[i].fut)
            continue;

        switch (futs[i].opcode) {
        case IO_CMD_PREAD:
            ret = futs[i].fut->get(futs[i].data,
                    futs[i].data_size, timeout ? timeout->tv_nsec : 10000000);
                    //futs[i].data_size, timeout ? timeout->tv_nsec : 1000000);
                    //futs[i].data_size, 1000000000);
            break;
        case IO_CMD_PWRITE:
            ret = futs[i].fut->get(futs[i].data,
                    futs[i].data_size, timeout ? timeout->tv_nsec : 100000000);
                    //futs[i].data_size, timeout ? timeout->tv_nsec : 1000000);
                    //futs[i].data_size, 1000000000);
            break;
        default:
            ret = 0;
            break;
        }

	if (ret == -10) {
            ret_futs = 0;
	    break;
	}
	if (ret < 0) {
            cout << "Fut ret: " << ret << " num_futs: " << num_futs << endl;
            break;
	}

        events[ret_futs].obj = reinterpret_cast<struct iocb *>(futs[i].user_data);
        events[ret_futs].res = futs[i].data_size;
        //cout << "jans_getevents ctx_idx: " << hash_io_context(ctx_id) << " size: " << futs[i].data_size << endl << flush;
        events[ret_futs].res2 = 0;

        futs[i].fut->sink();

	ret_futs++;
    }

    return ret_futs;
}

int jans_sync_open(const char *name, int flags, mode_t mode) {
    if (initialized == false)
        ModInit();
    int ret = syncApi->open(name, flags, mode);
    if (ret >= 0)
        sync_file_fds[ret] = ret;
    return ret; 
}

int jans_sync_close(int fd) {
    if (initialized == false)
        ModInit();
    if (fd >= 0)
        sync_file_fds[fd] = -1;
    return syncApi->close(fd);
}

uint64_t jans_sync_lseek(int fd, off_t pos, int whence) {
    if (initialized == false)
        ModInit();

    //if (!((on_fd_count++)%(1 << 16)))
        //cout << "ofdc " << old_fd_count << " nfdc " << new_fd_count << endl;

    if (async_file_fds[fd] == -1 && sync_file_fds[fd] == -1) {
        //old_fd_count++;
        return lseek(fd, pos, whence);
    }
    //new_fd_count++;
    return syncApi->lseek(fd, pos, whence);
}

//static int tot_pread = 0;
//static int pread_count = 0;
//static unsigned long long tot_pread_data = 0;

ssize_t jans_pread(int fd, void *buf, size_t count, off_t offset) {
    if (initialized == false)
        ModInit();

    //tot_pread++;
    //tot_pread_data += count;
    //if (!((pread_count++)%(1 << 16)))
        //cout << "JANS tot_pread " << tot_pread << " tot_data: " << tot_pread_data << endl;
	
    //if (!((on_fd_count++)%(1 << 16)))
        //cout << "ofdc " << old_fd_count << " nfdc " << new_fd_count << endl;

    if (async_file_fds[fd] == -1 && sync_file_fds[fd] == -1) {
        //old_fd_count++;
        return pread(fd, buf, count, offset);
    }
    //new_fd_count++;
    return syncApi->pread(fd, reinterpret_cast<char *>(buf), count, offset);
}

//static int tot_pwrite = 0;
//static int pwrite_count = 0;
//static unsigned long long tot_pwrite_data = 0;

ssize_t jans_pwrite(int fd, const void *buf, size_t count, off_t offset) {
    if (initialized == false)
        ModInit();

    //tot_pwrite++;
    //tot_pwrite_data += count;
    //if (!((pwrite_count++)%(1 << 16)))
        //cout << "JANS tot_pwrite " << tot_pwrite << " tot_data: " << tot_pwrite_data << endl;

    //if (!((on_fd_count++)%(1 << 16)))
        //cout << "ofdc " << old_fd_count << " nfdc " << new_fd_count << endl;

    if (async_file_fds[fd] == -1 && sync_file_fds[fd] == -1) {
        //old_fd_count++;
        return pwrite(fd, buf, count, offset);
    }
    //new_fd_count++;
    return syncApi->pwrite(fd, reinterpret_cast<const char *>(buf), count, offset);
}

int jans_fallocate(int fd, int mode, off_t offset, off_t len) {
    if (initialized == false)
        ModInit();
    off_t ret = syncApi->lseek(fd, offset + len, SEEK_SET);
    if ( ret >= 0)
        errno = 0;
    return ret < 0 ? ret : 0;
}

int jans_stat(const char *path, struct stat *buf) {
    if (initialized == false)
        ModInit();
    if (string(path) == "." || string(path) == "..")
        return ::stat(path, buf);
    int ret = syncApi->stat(path, buf);
    if (ret < 0)
        return ::stat(path, buf);
 
    buf->st_mode = S_IFREG;
    buf->st_blksize = 4096;
    return ret;
}

static const char *syncfilenames[] = {
        "ibdata1",            // systemtablespace
        "ib_logfile0",        // logfile1
        "ib_logfile1",        // logfile1
        "#ib_",               // doublewrite buffer
        "undo_001",           // undo tablespace1
        "undo_002",           // undo tablespace2
        "ibtmp1",             // temporary tablespace
        //"*.ibd",            // table and index files
        0
};

static bool jans_is_name_sync(const char *name) {
    for (const char *n = syncfilenames[0] ; n ; n++)
        if (!strcmp(n, name) || !strncmp(n, name, strlen(n)) ||
                (*n == '*' && !strcmp(&n[2], name)))
            return true;
    return false;
}

bool jans_is_filename_sync(const char *name) {
    if (initialized == false)
        ModInit();
    char sname[128];
    strcpy(sname, name);
    if (strlen(name) > 2)
        if (name[0] == '.' && name[1] =='/')
            strcpy(sname, &name[2]);
    return jans_is_name_sync(sname);
}

bool jans_is_filefd_sync(int fd) {
    if (initialized == false)
        ModInit();
    if (fd >= 0 && sync_file_fds[fd] == fd)
        return true;
    return false;
}

BdevCpp::Options options;
BdevCpp::Options spdk_options;
string spdk_conf_str("config.spdk");


int ModInit() {
    unique_lock<mutex> wlock(initMutex);

    if (initialized == true)
        return 0;

    string spdk_conf = spdk_conf_str;
    string usrlocal_spdk_conf = "/usr/local/mysql/" + spdk_conf_str;

    if (boost::filesystem::exists(spdk_conf.c_str()) != true)
        spdk_conf = usrlocal_spdk_conf;

    if (boost::filesystem::exists(spdk_conf.c_str())) {
        cout << "Io configuration file: " << spdk_conf << " ... " << endl;
        stringstream errorMsg;
        if (BdevCpp::readConfig(spdk_conf, spdk_options, errorMsg) == false) {
            cout << "Failed to read config file " << errorMsg.str() << endl;
            return -1;
        }
        options.io.allocUnitSize = spdk_options.io.allocUnitSize;
        options.io._devs = spdk_options.io._devs;
        options.io.devType = spdk_options.io.devType;
        options.io.name = spdk_options.io.name;
    } else {
        cerr << "No config found" << endl;
        return -1;
    }

    api = new BdevCpp::Api(options);
    syncApi = api->getSyncApi();
    asyncApi = api->getAsyncApi();

    for (int i = 0 ; i < (1 << 16) ; i++) {
        sync_file_fds[i] = -1;
        async_file_fds[i] = -1;
    }

    memset(io_contexts, '\0', sizeof(io_contexts));
    for (int i = 0 ; i < (1 << 16) ; i++) {
        io_contexts[i].full = false;
        io_contexts[i].empty = true;
        io_contexts[i].tail = 0;
        io_contexts[i].head = 0;
        io_contexts[i].freeFuts = CONTEXT_MAX_FUTS;
    }

    initialized = true;
    return 0;
}

void ModDeinit() {
    if (api)
        delete api;
}

