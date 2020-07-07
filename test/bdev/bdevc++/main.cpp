#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include <iostream>
#include <memory>
#include <thread>
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
#include "Api.h"

using namespace std;
namespace po = boost::program_options;

const char *spdk_conf = "config.spdk";

typedef struct _IoStats {
    double write_mbsec;
    double read_mbsec;
    double write_iops;
    double read_iops;
    uint64_t bytes_written;
    uint64_t bytes_read;
    uint64_t write_ios;
    uint64_t read_ios;
} IoStats;

typedef enum _eTestType {
    eReadTest = 0,
    eWriteTest = 1
} eTestType;

static int SyncOpenCloseTest( BdevCpp::SyncApi *api, const char *file_name) {
    int fd = api->open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }
    int rc =  api->close(fd);
    if (rc < 0)
        cerr << "close sync failed rc: " << rc << endl;

    return rc;
}

static int AsyncOpenCloseTest(BdevCpp::AsyncApi *api, const char *file_name) {
    int fd = api->open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }
    int rc =  api->close(fd);
    if (rc < 0)
        cerr << "close sync failed rc: " << rc << endl;

    return rc;
}

static time_t printTimeNow(const char *msg) {
    char time_buf[128];
    time_t now = time(0);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S.000", localtime(&now));
    cout << msg << " " << time_buf << endl;
    return now;
}

static size_t calcIoSize(size_t blkSize, size_t idx, size_t min_mult, size_t max_mult) {
    size_t m = idx;
    if (idx < min_mult)
        m = min_mult;
    if (idx > max_mult)
        m = max_mult;
    return  blkSize * m;
}

static off_t getSyncFileSize(BdevCpp::SyncApi *api, int fd) {
    off_t pos = api->lseek(fd, 0, SEEK_CUR);
    off_t fsize = api->lseek(fd, 0, SEEK_END);
    off_t ret = api->lseek(fd, pos, SEEK_SET);
    return ret < 0 ? ret : fsize;
}

static off_t getAsyncFileSize(BdevCpp::AsyncApi *api, int fd) {
    off_t pos = api->lseek(fd, 0, SEEK_CUR);
    off_t fsize = api->lseek(fd, 0, SEEK_END);
    off_t ret = api->lseek(fd, pos, SEEK_SET);
    return ret < 0 ? ret : fsize;
}

#define MAX_STACK_IO_SIZE 12000000

static int SyncWriteIoTest(BdevCpp::SyncApi *api,
        const char *file_name, int mode, int check,
        int loop_count, int out_loop_count, 
        size_t max_iosize_mult, size_t min_iosize_mult,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        IoStats *st) {
    int rc = 0;
    char *io_buf = new char[MAX_STACK_IO_SIZE];
    char *io_cmp_buf = new char[MAX_STACK_IO_SIZE];
    if (!io_buf || !io_cmp_buf) {
        cerr << "Can't alloc buffer" << endl;
        return -1;
    }

    int fd = !mode ? api->open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT) : 
        ::open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    if (print_file_size == true)
        cout << "Sync file: " << file_name << " size: " << getSyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    time_t stime = printTimeNow("Start sync test ");

    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
    uint64_t write_ios = 0;
    uint64_t read_ios = 0;
    uint64_t pos = 0;
    uint64_t prev_pos = pos;

    for (int j = 0 ; j < out_loop_count ; j++) {
        prev_pos = pos;
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
            if (check)
                ::memset(io_buf, 'a' + i%20, io_size);
            rc = !mode ? api->write(fd, io_buf, io_size) : ::write(fd, io_buf, io_size);
            if (rc < 0) {
                cerr << "write sync failed rc: " << rc << " errno: " << errno << endl;
                break;
            }

            rc = !mode ? api->fsync(fd) : ::fsync(fd);
            if (rc < 0) {
                cerr << "fsync failed rc: " << rc << " errno: " << errno << endl;
                break;
            }

            pos += io_size;
            write_ios++;
            bytes_written += io_size;
        }

        if (!rc && check) {
            rc = !mode ? api->lseek(fd, static_cast<off_t>(prev_pos), SEEK_SET) :
                    ::lseek(fd, static_cast<off_t>(prev_pos), SEEK_SET);
            if (rc < 0) {
                cerr << "lseek failed rc: " << rc << " errno: " << errno << endl;
                return rc;
            }

            for (int i = 0 ; i < loop_count ; i++) {
                size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
                ::memset(io_cmp_buf, 'a' + i%20, io_size);

                rc = !mode ? api->read(fd, io_buf, io_size) : ::read(fd, io_buf, io_size);
                if (rc < 0) {
                    cerr << "read sync failed rc: " << rc << " errno: " << errno << endl;
                    rc = -1;
                    break;
                }
                read_ios++;
                bytes_read += io_size;

                if (memcmp(io_buf, io_cmp_buf, io_size)) {
                    cerr << "Corrupted data after sync read i: " << i << " io_size: " << io_size << endl;
                    rc = -1;
                    break;
                }
            }

            rc = !mode ? api->lseek(fd, static_cast<off_t>(pos), SEEK_SET) :
                    ::lseek(fd, static_cast<off_t>(pos), SEEK_SET);
            if (rc < 0) {
                cerr << "lseek failed rc: " << rc << " errno: " << errno << endl;
                return rc;
            }
        }
    }

    time_t etime = printTimeNow("End sync test ");
    double t_diff = difftime(etime, stime);
    if (!t_diff)
        t_diff = 1;
    st->write_mbsec = static_cast<double>(bytes_written)/t_diff;
    st->write_mbsec /= (1024 * 1024);
    st->read_mbsec = static_cast<double>(bytes_read)/t_diff;
    st->read_mbsec /= (1024 * 1024);
    st->write_iops = static_cast<double>(write_ios)/t_diff;
    st->read_iops = static_cast<double>(read_ios)/t_diff;
    st->bytes_written = bytes_written;
    st->bytes_read = bytes_read;
    st->write_ios = write_ios;
    st->read_ios = read_ios;

    if (print_file_size == true)
        cout << "Sync file: " << file_name << " size: " << getSyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    rc = !mode ? api->close(fd) : ::close(fd);

    if (unlink_file == true) {
        rc = !mode ? api->unlink(file_name) : ::unlink(file_name);
        if (rc < 0)
            cerr << "Unlink file: " << file_name << " failed" << endl;
    }

    if (io_buf && io_cmp_buf) {
        delete [] io_buf;
        delete [] io_cmp_buf;
    }

    return rc;
}

static int SyncPwriteIoTest(BdevCpp::SyncApi *api,
        const char *file_name, int check,
        int loop_count, int out_loop_count,
        size_t max_iosize_mult, size_t min_iosize_mult,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        IoStats *st) {
    int rc = 0;
    char *io_buf = new char[MAX_STACK_IO_SIZE];
    char *io_cmp_buf = new char[MAX_STACK_IO_SIZE];
    if (!io_buf || !io_cmp_buf) {
        cerr << "Can't alloc buffer" << endl;
        return -1;
    }

    int fd = api->open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    if (print_file_size == true)
        cout << "Sync file: " << file_name << " size: " << getSyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    time_t stime = printTimeNow("Start psync test ");

    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
    uint64_t write_ios = 0;
    uint64_t read_ios = 0;
    uint64_t pos = 0;
    uint64_t check_pos = pos;

    for (int j = 0 ; j < out_loop_count ; j++) {
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
            if (check)
                ::memset(io_buf, 'a' + i%20, io_size);
            rc = api->pwrite(fd, io_buf, io_size, static_cast<off_t>(pos));
            if (rc < 0) {
                cerr << "pwrite sync failed rc: " << rc << " errno: " << errno << endl;
                break;
            }

            pos += io_size;
            write_ios++;
            bytes_written += io_size;
        }

        if (rc >= 0 && check) {
            for (int i = 0 ; i < loop_count ; i++) {
                size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
                ::memset(io_cmp_buf, 'a' + i%20, io_size);

                rc = api->pread(fd, io_buf, io_size, static_cast<off_t>(check_pos));
                if (rc < 0) {
                    cerr << "pread sync failed rc: " << rc << " errno: " << errno << endl;
                    rc = -1;
                    break;
                }

                check_pos += io_size;
                read_ios++;
                bytes_read += io_size;

                if (::memcmp(io_buf, io_cmp_buf, io_size)) {
                    cerr << "Corrupted data after sync read i: " << i << " io_size: " << io_size << endl;
                    rc = -1;
                    break;
                }
            }
        }
    }

    time_t etime = printTimeNow("End psync test ");
    double t_diff = difftime(etime, stime);
    if (!t_diff)
        t_diff = 1;
    st->write_mbsec = static_cast<double>(bytes_written)/t_diff;
    st->write_mbsec /= (1024 * 1024);
    st->read_mbsec = static_cast<double>(bytes_read)/t_diff;
    st->read_mbsec /= (1024 * 1024);
    st->write_iops = static_cast<double>(write_ios)/t_diff;
    st->read_iops = static_cast<double>(read_ios)/t_diff;
    st->bytes_written = bytes_written;
    st->bytes_read = bytes_read;
    st->write_ios = write_ios;
    st->read_ios = read_ios;

    if (print_file_size == true)
        cout << "Sync file: " << file_name << " size: " << getSyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    if (io_buf && io_cmp_buf) {
        delete [] io_buf;
        delete [] io_cmp_buf;
    }

    api->close(fd);

    if (unlink_file == true) {
        rc = api->unlink(file_name);
        if (rc < 0)
            cerr << "Unlink file: " << file_name << " failed" << endl;
    }

    return rc;
}

static int SyncReadIoTest(BdevCpp::SyncApi *api,
        const char *file_name, int mode, int check,
        int loop_count, int out_loop_count,
        size_t max_iosize_mult, size_t min_iosize_mult,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        IoStats *st) {
    int rc = 0;
    char *io_buf = new char[MAX_STACK_IO_SIZE];
    if (!io_buf) {
        cerr << "Can't alloc buffer" << endl;
        return -1;
    }

    int fd = !mode ? api->open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT) :
        ::open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    if (print_file_size == true)
        cout << "Sync file: " << file_name << " size: " << getSyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    time_t stime = printTimeNow("Start sync test ");

    uint64_t bytes_read = 0;
    uint64_t read_ios = 0;

    for (int j = 0 ; j < out_loop_count ; j++) {
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
            rc = !mode ? api->read(fd, io_buf, io_size) : ::read(fd, io_buf, io_size);
            if (rc < 0) {
                cerr << "read sync failed rc: " << rc << " errno: " << errno << endl;
                break;
            }

            read_ios++;
            bytes_read += io_size;
        }
    }

    time_t etime = printTimeNow("End read sync test ");
    double t_diff = difftime(etime, stime);
    if (!t_diff)
        t_diff = 1;
    st->write_mbsec = 0;
    st->write_mbsec = 0;
    st->read_mbsec = static_cast<double>(bytes_read)/t_diff;
    st->read_mbsec /= (1024 * 1024);
    st->write_iops = 0;
    st->read_iops = static_cast<double>(read_ios)/t_diff;
    st->bytes_written = 0;
    st->bytes_read = bytes_read;
    st->write_ios = 0;
    st->read_ios = read_ios;

    if (print_file_size == true)
        cout << "Sync file: " << file_name << " size: " << getSyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    rc = !mode ? api->close(fd) : ::close(fd);

    if (unlink_file == true) {
        rc = api->unlink(file_name);
        if (rc < 0)
            cerr << "Unlink file: " << file_name << " failed" << endl;
    }

    if (io_buf)
        delete [] io_buf;

    return rc;
}

static int SyncPreadIoTest(BdevCpp::SyncApi *api,
        const char *file_name, int check,
        int loop_count, int out_loop_count,
        size_t max_iosize_mult, size_t min_iosize_mult,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        IoStats *st) {
    int rc = 0;
    char *io_buf = new char[MAX_STACK_IO_SIZE];
    if (!io_buf) {
        cerr << "Can't alloc buffer" << endl;
        return -1;
    }

    int fd = api->open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    if (print_file_size == true)
        cout << "Sync file: " << file_name << " size: " << getSyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    time_t stime = printTimeNow("Start psync test ");

    uint64_t bytes_read = 0;
    uint64_t read_ios = 0;
    uint64_t pos = 0;

    for (int j = 0 ; j < out_loop_count ; j++) {
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
            rc = api->pread(fd, io_buf, io_size, static_cast<off_t>(pos));
            if (rc < 0) {
                cerr << "pread sync failed rc: " << rc << " errno: " << errno << endl;
                break;
            }

            pos += io_size;
            read_ios++;
            bytes_read += io_size;
        }
    }

    time_t etime = printTimeNow("End pread sync test ");
    double t_diff = difftime(etime, stime);
    if (!t_diff)
        t_diff = 1;
    st->write_mbsec = 0;
    st->write_mbsec = 0;
    st->read_mbsec = static_cast<double>(bytes_read)/t_diff;
    st->read_mbsec /= (1024 * 1024);
    st->write_iops = 0;
    st->read_iops = static_cast<double>(read_ios)/t_diff;
    st->bytes_written = 0;
    st->bytes_read = bytes_read;
    st->write_ios = 0;
    st->read_ios = read_ios;

    if (print_file_size == true)
        cout << "Sync file: " << file_name << " size: " << getSyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    if (io_buf)
        delete [] io_buf;

    api->close(fd);

    if (unlink_file == true) {
        rc = api->unlink(file_name);
        if (rc < 0)
            cerr << "Unlink file: " << file_name << " failed" << endl;
    }

    return rc;
}

//
// Async IO test
//
const size_t maxWriteFutures = 256;
const size_t maxReadFutures = 256;

static int AsyncIoCompleteWrites(BdevCpp::AsyncApi *api,
        size_t &write_ios,
        size_t &bytes_written,
        size_t &num_write_futures,
        BdevCpp::FutureBase *write_futures[]) {
    int rc = 0;

    size_t curr_idx = num_write_futures;
    for (size_t i = 0 ; i < curr_idx ; i++) {
        char *data;
        size_t dataSize;

        if (!write_futures[i])
            continue;

        int w_rc = write_futures[i]->get(data, dataSize);
        if (w_rc < 0) {
            cerr << "WriteFuture get failed rc: " << w_rc << endl;
            rc = -1;
            break;
        }
        write_futures[i]->sink();

        write_ios++;
        bytes_written += dataSize;
    }
    num_write_futures = 0;

    return rc;
}

static int AsyncIoCompleteReads(BdevCpp::AsyncApi *api,
        size_t &read_ios,
        size_t &bytes_read,
        size_t &num_read_futures,
        BdevCpp::FutureBase *read_futures[],
        char *io_buffers[],
        char *io_cmp_buffers[],
        size_t io_sizes[]) {
    int rc = 0;

    size_t curr_idx = num_read_futures;
    for (size_t i = 0 ; i < curr_idx ; i++) {
        char *data;
        size_t dataSize;

        if (!read_futures[i])
            continue;

        int r_rc = read_futures[i]->get(data, dataSize);
        if (r_rc < 0) {
            cerr << "ReadFuture get failed rc: " << r_rc << endl;
            rc = -1;
            break;
        }

        read_ios++;
        bytes_read += dataSize;

        if (io_cmp_buffers && ::memcmp(io_buffers[i], io_cmp_buffers[i], io_sizes[i])) {
            cerr << "Corrupted data after read at idx: " << i << " io_size: " << io_sizes[i] << endl;
            rc = -1;
            break;
        }
        read_futures[i]->sink();
    }
    num_read_futures = 0;

    return rc;
}

static int AsyncWriteIoTest(BdevCpp::AsyncApi *api,
        const char *file_name, int check,
        int loop_count, int out_loop_count, 
        size_t max_iosize_mult, size_t min_iosize_mult, size_t max_queued,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        IoStats *st) {
    BdevCpp::FutureBase *write_futures[maxWriteFutures];
    size_t num_write_futures = 0;
    BdevCpp::FutureBase *read_futures[maxReadFutures];
    size_t num_read_futures = 0;

    char *io_buffers[maxWriteFutures];
    char *io_cmp_buffers[maxReadFutures];
    size_t io_sizes[maxReadFutures];

    int rc = 0;

    int fd = api->open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    for (size_t i = 0 ; i < maxWriteFutures ; i++)
        io_buffers[i] = new char[MAX_STACK_IO_SIZE];
    for (size_t i = 0 ; i < maxReadFutures ; i++)
        io_cmp_buffers[i] = new char[MAX_STACK_IO_SIZE];

    if (print_file_size == true)
        cout << "Async file: " << file_name << " size: " << getAsyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    time_t stime = printTimeNow("Start write (verify) async test ");

    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
    uint64_t write_ios = 0;
    uint64_t read_ios = 0;
    uint64_t pos = 0;
    uint64_t check_pos = pos;

    for (int j = 0 ; j < out_loop_count ; j++) {
        check_pos = pos;
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
            if (check)
                ::memset(io_buffers[num_write_futures], 'a' + i%20, io_size);

            write_futures[num_write_futures] = api->write(fd, pos, io_buffers[num_write_futures], io_size);
            num_write_futures++;
            pos += io_size;

            if (!write_futures[num_write_futures - 1]) {
                rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures);
                if (rc < 0)
                    break;
                continue;
            }

            if (num_write_futures >= max_queued || num_write_futures >= maxWriteFutures)
                rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures);
        }

        rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures);

        if (!rc && check) {
            for (int i = 0 ; i < loop_count ; i++) {
                size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
                ::memset(io_cmp_buffers[num_read_futures], 'a' + i%20, io_size);
                io_sizes[num_read_futures] = io_size;

                read_futures[num_read_futures] = api->read(fd, check_pos, io_buffers[num_read_futures], io_size);
                num_read_futures++;
                check_pos += io_size;

                if (!read_futures[num_read_futures - 1]) {
                    rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, io_cmp_buffers, io_sizes);
                    if (rc < 0)
                        break;
                    continue;
                }

                if (num_read_futures > max_queued || num_read_futures >= maxReadFutures)
                    rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, io_cmp_buffers, io_sizes);
            }

            rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, io_cmp_buffers, io_sizes);
        }
    }

    time_t etime = printTimeNow("End async test");
    double t_diff = difftime(etime, stime);
    if (!t_diff)
        t_diff = 1;
    st->write_mbsec = static_cast<double>(bytes_written)/t_diff;
    st->write_mbsec /= (1024 * 1024);
    st->read_mbsec = static_cast<double>(bytes_read)/t_diff;
    st->read_mbsec /= (1024 * 1024);
    st->write_iops = static_cast<double>(write_ios)/t_diff;
    st->read_iops = static_cast<double>(read_ios)/t_diff;
    st->bytes_written = bytes_written;
    st->bytes_read = bytes_read;
    st->write_ios = write_ios;
    st->read_ios = read_ios;

    if (print_file_size == true)
        cout << "Async file: " << file_name << " size: " << getAsyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    rc = api->close(fd);

    if (unlink_file == true) {
        rc = api->unlink(file_name);
        if (rc < 0)
            cerr << "Unlink file: " << file_name << " failed" << endl;
    }

    for (size_t i = 0 ; i < maxWriteFutures ; i++)
        delete [] io_buffers[i];
    for (size_t i = 0 ; i < maxReadFutures ; i++)
        delete [] io_cmp_buffers[i];

    return rc;
}

static int AsyncSyncWriteIoTest(BdevCpp::AsyncApi *api,
        const char *file_name, int check,
        int loop_count, int out_loop_count,
        size_t max_iosize_mult, size_t min_iosize_mult, size_t max_queued,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        IoStats *st) {
    BdevCpp::FutureBase *write_futures[maxWriteFutures];
    size_t num_write_futures = 0;
    BdevCpp::FutureBase *read_futures[maxReadFutures];
    size_t num_read_futures = 0;
    char *io_buf = new char[2500000];
    char *io_cmp_buf = new char[2500000];
    if (!io_buf || !io_cmp_buf) {
        cerr << "Can't alloc buffer" << endl;
        return -1;
    }

    char *io_buffers[maxWriteFutures];
    char *io_cmp_buffers[maxReadFutures];
    size_t io_sizes[maxReadFutures];

    int rc = 0;

    int fd = api->open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    for (size_t i = 0 ; i < maxWriteFutures ; i++)
        io_buffers[i] = new char[MAX_STACK_IO_SIZE];
    for (size_t i = 0 ; i < maxReadFutures ; i++)
        io_cmp_buffers[i] = new char[MAX_STACK_IO_SIZE];

    if (print_file_size == true)
        cout << "Async file: " << file_name << " size: " << getAsyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    time_t stime = printTimeNow("Start write (verify) async test ");

    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
    uint64_t write_ios = 0;
    uint64_t read_ios = 0;
    uint64_t pos = 0;
    uint64_t check_pos = pos;

    bool failed = false;
    for (int j = 0 ; j < out_loop_count ; j++) {
        check_pos = pos;
        int alternate = 0;
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
            if (check) {
                ::memset(io_buffers[num_write_futures], 'a' + i%20, io_size);
                ::memset(io_buf, 'a' + i%20, io_size);
            }

            if (alternate) {
                write_futures[num_write_futures] = api->write(fd, pos, io_buffers[num_write_futures], io_size);
                num_write_futures++;
            } else {
                int s_rc = api->pwrite(fd, io_buf, io_size, static_cast<off_t>(pos));
                if (s_rc < 0) {
                    cerr << "pwrite failed rc: " << s_rc << endl;
                    failed = true;
                    break;
                }
                write_ios++;
                bytes_written += io_size;
                pos += io_size;
                alternate = !alternate ? 1 : 0;
                continue;
            }

            pos += io_size;
            alternate = !alternate ? 1 : 0;

            if (!write_futures[num_write_futures - 1]) {
                rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures);
                if (rc < 0)
                    break;
                continue;
            }

            if (num_write_futures >= max_queued || num_write_futures >= maxWriteFutures)
                rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures);
        }

        if (failed == true)
            break;

        rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures);

        if (!rc && check) {
            int alternate = 0;
            for (int i = 0 ; i < loop_count ; i++) {
                size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
                ::memset(io_cmp_buffers[num_read_futures], 'a' + i%20, io_size);
                ::memset(io_cmp_buf, 'a' + i%20, io_size);
                io_sizes[num_read_futures] = io_size;

                if (alternate) {
                    read_futures[num_read_futures] = api->read(fd, check_pos, io_buffers[num_read_futures], io_size);
                    num_read_futures++;
                } else {
                    int s_rc = api->pread(fd, io_buf, io_size, static_cast<off_t>(check_pos));
                    if (s_rc < 0) {
                        cerr << "pread failed rc: " << s_rc << endl;
                        failed = true;
                        break;
                    }
                    read_ios++;
                    bytes_read += io_size;
                    alternate = !alternate ? 1 : 0;
                    check_pos += io_size;

                    if (::memcmp(io_buf, io_cmp_buf, io_size)) {
                        cerr << "Corrupted data after read at idx: " << i << " io_size: " << io_size << endl;
                        rc = -1;
                        failed = true;
                        break;
                    }
                    continue;
                }

                check_pos += io_size;
                alternate = !alternate ? 1 : 0;

                if (!read_futures[num_read_futures - 1]) {
                    rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, io_cmp_buffers, io_sizes);
                    if (rc < 0)
                        break;
                    continue;
                }

                if (num_read_futures > max_queued || num_read_futures >= maxReadFutures)
                    rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, io_cmp_buffers, io_sizes);
            }

            rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, io_cmp_buffers, io_sizes);
        }

        if (failed == true)
            break;
    }

    time_t etime = printTimeNow("End async test");
    double t_diff = difftime(etime, stime);
    if (!t_diff)
        t_diff = 1;
    st->write_mbsec = static_cast<double>(bytes_written)/t_diff;
    st->write_mbsec /= (1024 * 1024);
    st->read_mbsec = static_cast<double>(bytes_read)/t_diff;
    st->read_mbsec /= (1024 * 1024);
    st->write_iops = static_cast<double>(write_ios)/t_diff;
    st->read_iops = static_cast<double>(read_ios)/t_diff;
    st->bytes_written = bytes_written;
    st->bytes_read = bytes_read;
    st->write_ios = write_ios;
    st->read_ios = read_ios;

    if (print_file_size == true)
        cout << "Async file: " << file_name << " size: " << getAsyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    rc = api->close(fd);

    if (unlink_file == true) {
        rc = api->unlink(file_name);
        if (rc < 0)
            cerr << "Unlink file: " << file_name << " failed" << endl;
    }

    for (size_t i = 0 ; i < maxWriteFutures ; i++)
        delete [] io_buffers[i];
    for (size_t i = 0 ; i < maxReadFutures ; i++)
        delete [] io_cmp_buffers[i];

    if (io_cmp_buf && io_buf) {
        delete [] io_buf;
        delete [] io_cmp_buf;
    }

    return rc;
}

static int AsyncReadIoTest(BdevCpp::AsyncApi *api,
        const char *file_name, int check,
        int loop_count, int out_loop_count,
        size_t max_iosize_mult, size_t min_iosize_mult, size_t max_queued,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        IoStats *st) {
    BdevCpp::FutureBase *read_futures[maxReadFutures];
    size_t num_read_futures = 0;

    char *io_buffers[maxReadFutures];
    size_t io_sizes[maxReadFutures];

    int rc = 0;

    int fd = api->open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    for (size_t i = 0 ; i < maxReadFutures ; i++)
        io_buffers[i] = new char[MAX_STACK_IO_SIZE];

    if (print_file_size == true)
        cout << "Async file: " << file_name << " size: " << getAsyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    time_t stime = printTimeNow("Start read async test ");

    uint64_t bytes_read = 0;
    uint64_t read_ios = 0;
    uint64_t pos = 0;

    for (int j = 0 ; j < out_loop_count ; j++) {
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);

            read_futures[num_read_futures] = api->read(fd, pos, io_buffers[num_read_futures], io_size);
            num_read_futures++;
            pos += io_size;

            if (!read_futures[num_read_futures - 1]) {
                rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes);
                if (rc < 0)
                    break;
                continue;
            }

            if (num_read_futures >= max_queued || num_read_futures >= maxReadFutures)
                rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes);
        }

        rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes);
    }

    time_t etime = printTimeNow("End async test");
    double t_diff = difftime(etime, stime);
    if (!t_diff)
        t_diff = 1;
    st->write_mbsec = 0;
    st->write_mbsec = 0;
    st->read_mbsec = static_cast<double>(bytes_read)/t_diff;
    st->read_mbsec /= (1024 * 1024);
    st->write_iops = 0;
    st->read_iops = static_cast<double>(read_ios)/t_diff;
    st->bytes_written = 0;
    st->bytes_read = bytes_read;
    st->write_ios = 0;
    st->read_ios = read_ios;

    if (print_file_size == true)
        cout << "Async file: " << file_name << " size: " << getAsyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    rc = api->close(fd);

    if (unlink_file == true) {
        rc = api->unlink(file_name);
        if (rc < 0)
            cerr << "Unlink file: " << file_name << " failed" << endl;
    }

    for (size_t i = 0 ; i < maxWriteFutures ; i++)
        delete [] io_buffers[i];

    return rc;
}

static int AsyncSyncReadIoTest(BdevCpp::AsyncApi *api,
        const char *file_name, int check,
        int loop_count, int out_loop_count,
        size_t max_iosize_mult, size_t min_iosize_mult, size_t max_queued,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        IoStats *st) {
    BdevCpp::FutureBase *read_futures[maxReadFutures];
    size_t num_read_futures = 0;
    char *io_buf = new char[MAX_STACK_IO_SIZE];
    if (!io_buf) {
        cerr << "Can't alloc buffer" << endl;
        return -1;
    }

    char *io_buffers[maxReadFutures];
    size_t io_sizes[maxReadFutures];

    int rc = 0;

    int fd = api->open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    for (size_t i = 0 ; i < maxReadFutures ; i++)
        io_buffers[i] = new char[MAX_STACK_IO_SIZE];

    if (print_file_size == true)
        cout << "Async file: " << file_name << " size: " << getAsyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    time_t stime = printTimeNow("Start read async test ");

    uint64_t bytes_read = 0;
    uint64_t read_ios = 0;
    uint64_t pos = 0;

    bool failed = false;
    for (int j = 0 ; j < out_loop_count ; j++) {
        int alternate = 0;
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);

            if (alternate) {
                read_futures[num_read_futures] = api->read(fd, pos, io_buffers[num_read_futures], io_size);
                num_read_futures++;
            } else {
                int s_rc = api->pread(fd, io_buf, io_size, static_cast<off_t>(pos));
                if (s_rc < 0) {
                    cerr << "pread failed rc: " << s_rc << endl;
                    failed = true;
                    break;
                }
                read_ios++;
                bytes_read += io_size;
                pos += io_size;
                alternate = !alternate ? 1 : 0;
                continue;
            }

            pos += io_size;
            alternate = !alternate ? 1 : 0;

            if (!read_futures[num_read_futures - 1]) {
                rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes);
                if (rc < 0)
                    break;
                continue;
            }

            if (num_read_futures >= max_queued || num_read_futures >= maxReadFutures)
                rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes);
        }

        rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes);

        if (failed == true)
            break;
    }

    time_t etime = printTimeNow("End async test");
    double t_diff = difftime(etime, stime);
    if (!t_diff)
        t_diff = 1;
    st->write_mbsec = 0;
    st->write_mbsec = 0;
    st->read_mbsec = static_cast<double>(bytes_read)/t_diff;
    st->read_mbsec /= (1024 * 1024);
    st->write_iops = 0;
    st->read_iops = static_cast<double>(read_ios)/t_diff;
    st->bytes_written = 0;
    st->bytes_read = bytes_read;
    st->write_ios = 0;
    st->read_ios = read_ios;

    if (print_file_size == true)
        cout << "Async file: " << file_name << " size: " << getAsyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = api->stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    rc = api->close(fd);

    if (unlink_file == true) {
        rc = api->unlink(file_name);
        if (rc < 0)
            cerr << "Unlink file: " << file_name << " failed" << endl;
    }

    for (size_t i = 0 ; i < maxWriteFutures ; i++)
        delete [] io_buffers[i];

    if (io_buf)
        delete [] io_buf;

    return rc;
}

static void usage(const char *prog)
{
    cout <<
        "Usage %s [options]\n"
        "  -c, --spdk-conf                SPDK config file (config.spdk - default)\n"
        "  -s, --sync-file                Sync file name (testsync - default)\n"
        "  -a, --async-file               Async file name (testasync - default)\n"
        "  -m, --loop-count               Inner IO loop count (10 - default)\n"
        "  -n, --outer-loop-count         Outer IO loop count (10 - default)\n"
        "  -q, --max-queued               Max queued async IOs (8 - default)\n"
        "  -z, --max-iosize-mult          Max IO size 512 multiplier (64 - default)\n"
        "  -w, --min-iosize-mult          Min IO size 512 multiplier (8 - default)\n"
        "  -t, --num-files                Number of files to write/read concurrently\n"
        "  -l, --legacy-newstack-mode     Use legacy (1) or new stack (0 - default) file IO routines\n"
        "  -i, --integrity-check          Run integrity check (false - default)\n"
        "  -O, --open-close-test          Run open/clsoe test\n"
        "  -S, --sync-test                Run IO sync (read/write) test\n"
        "  -P, --psync-test               Run IO psync (pwrite/pread) test\n"
        "  -A, --async-test               Run IO async test\n"
        "  -M, --async-sync-test          Run IO same file async/sync test\n"
        "  -R, --read-test                Run read test\n"
        "  -W, --write-test               Run write test (default)\n"
        "  -L, --print-file-size          Print file size after test's end\n"
        "  -Y, --stat-file                Print file stat\n"
        "  -Z, --unlink-file              Unlink file after test\n"
        "  -d, --debug                    Debug on (false - default)\n"
        "  -h, --help                     Print this help\n" << prog << endl;
}

static int mainGetopt(int argc, char *argv[],
        string &spdk_conf,
        string &sync_file,
        string &async_file,
        int &loop_count,
        int &out_loop_count,
        size_t &max_queued,
        size_t &max_iosize_mult,
        size_t &min_iosize_mult,
        size_t &num_files,
        int &legacy_newstack_mode,
        bool &integrity_check,
        bool &open_close_test,
        bool &sync_test,
        bool &psync_test,
        bool &async_test,
        bool &async_sync_test,
        bool &print_file_size,
        bool &stat_file,
        bool &unlink_file,
        eTestType &test_type,
        bool &debug)
{
    int c = 0;
    int ret = -1;

    while (1) {
        static char short_options[] = "c:s:a:m:n:l:i:z:t:w:q:dhOSPAMRWLZY";
        static struct option long_options[] = {
            {"spdk-conf",               1, 0, 'c'},
            {"sync-file",               1, 0, 's'},
            {"async-file",              1, 0, 'a'},
            {"loop-count",              1, 0, 'm'},
            {"outer-loop-count",        1, 0, 'n'},
            {"legacy-newstack-mode",    1, 0, 'l'},
            {"integrity-check",         1, 0, 'i'},
            {"max-queued",              1, 0, 'q'},
            {"max-iosize-mult",         1, 0, 'z'},
            {"min-iosize-mult",         1, 0, 'w'},
            {"num-files",               1, 0, 't'},
            {"open-close-test",         0, 0, 'O'},
            {"sync-test",               0, 0, 'S'},
            {"psync-test",              0, 0, 'P'},
            {"async-test",              0, 0, 'A'},
            {"async-sync-test",         0, 0, 'M'},
            {"read-test",               0, 0, 'R'},
            {"write-test",              0, 0, 'W'},
            {"print-file-size",         0, 0, 'L'},
            {"unlink-file",             0, 0, 'Z'},
            {"stat-file",               0, 0, 'Y'},
            {"debug",                   0, 0, 'd'},
            {"help",                    0, 0, 'h'},
            {0, 0, 0, 0}
        };

        if ( (c = getopt_long(argc, argv, short_options, long_options, NULL)) == -1 ) {
            break;
        }

        ret = 0;

        switch ( c ) {
        case 'c':
            spdk_conf = optarg;
            break;

        case 's':
            sync_file = optarg;
            break;

        case 'a':
            async_file = optarg;
            break;

        case 'm':
            loop_count = atoi(optarg);
            break;

        case 'n':
            out_loop_count = atoi(optarg);
            break;

        case 'q':
            max_queued = static_cast<size_t>(atoi(optarg));
            break;

        case 'z':
            max_iosize_mult = static_cast<size_t>(atoi(optarg));
            break;

        case 't':
            num_files = static_cast<size_t>(atoi(optarg));
            break;

        case 'w':
            min_iosize_mult = static_cast<size_t>(atoi(optarg));
            break;

        case 'l':
            legacy_newstack_mode = atoi(optarg);
            break;

        case 'i':
            integrity_check = atoi(optarg);
            break;

        case 'O':
            open_close_test = true;
            break;

        case 'S':
            sync_test = true;
            break;

        case 'P':
            psync_test = true;
            break;

        case 'A':
            async_test = true;
            break;

        case 'M':
            async_sync_test = true;
            break;

        case 'R':
            test_type = eReadTest;
            break;

        case 'W':
            test_type = eWriteTest;
            break;

        case 'L':
            print_file_size = true;
            break;

        case 'Y':
            stat_file = true;
            break;

        case 'Z':
            unlink_file = true;
            break;

        case 'd':
            debug = true;
            break;

        case 'h':
            usage(argv[0]);
            break;

        default:
            ret = -1;
            break;
        }
    }

    return ret;
}

struct ThreadArgs {
    BdevCpp::AsyncApi *api;
    string file;
    bool int_check;
    int l_cnt;
    int out_l_cnt;
    size_t max_ios_m; 
    size_t min_ios_m;
    size_t m_q;
    bool print_file_size;
    bool stat_file;
    bool unlink_file;
    IoStats stats;
    int rc;
};

static void AsyncWriteIoTestRunner(ThreadArgs *targs) {
    int rc = AsyncWriteIoTest(targs->api, targs->file.c_str(),
        targs->int_check, targs->l_cnt, targs->out_l_cnt, 
        targs->max_ios_m, targs->min_ios_m, targs->m_q, targs->print_file_size, targs->stat_file, targs->unlink_file, &targs->stats);
    targs->rc = rc;
}

static void AsyncSyncWriteIoTestRunner(ThreadArgs *targs) {
    int rc = AsyncSyncWriteIoTest(targs->api, targs->file.c_str(),
        targs->int_check, targs->l_cnt, targs->out_l_cnt,
        targs->max_ios_m, targs->min_ios_m, targs->m_q, targs->print_file_size, targs->stat_file, targs->unlink_file, &targs->stats);
    targs->rc = rc;
}

static void AsyncReadIoTestRunner(ThreadArgs *targs) {
    int rc = AsyncReadIoTest(targs->api, targs->file.c_str(),
        targs->int_check, targs->l_cnt, targs->out_l_cnt,
        targs->max_ios_m, targs->min_ios_m, targs->m_q, targs->print_file_size, targs->stat_file, targs->unlink_file, &targs->stats);
    targs->rc = rc;
}

static void AsyncSyncReadIoTestRunner(ThreadArgs *targs) {
    int rc = AsyncSyncReadIoTest(targs->api, targs->file.c_str(),
        targs->int_check, targs->l_cnt, targs->out_l_cnt,
        targs->max_ios_m, targs->min_ios_m, targs->m_q, targs->print_file_size, targs->stat_file, targs->unlink_file, &targs->stats);
    targs->rc = rc;
}

int main(int argc, char **argv) {
    int rc = 0;
    BdevCpp::Options options;
    BdevCpp::Options spdk_options;
    bool debug = false;
    string spdk_conf_str(spdk_conf);
    int loop_count = 10;
    int out_loop_count = 10;
    size_t max_queued = maxWriteFutures - 2;
    size_t max_iosize_mult = 64;
    size_t min_iosize_mult = 8;
    size_t num_files = 1;
    const char *sync_file_name = "testsync";
    string sync_file(sync_file_name);
    const char *async_file_name = "testasync";
    string async_file(async_file_name);
    int legacy_newstack_mode = 0;
    bool integrity_check = false;
    bool print_file_size = false;
    bool stat_file = false;
    bool unlink_file = false;

    bool open_close_test = false;
    bool sync_test = false;
    bool psync_test = false;
    bool async_test = false;
    bool async_sync_test = false;
    eTestType test_type = eWriteTest;

    int ret = mainGetopt(argc, argv,
            spdk_conf_str,
            sync_file,
            async_file,
            loop_count,
            out_loop_count,
            max_queued,
            max_iosize_mult,
            min_iosize_mult,
            num_files,
            legacy_newstack_mode,
            integrity_check,
            open_close_test,
            sync_test,
            psync_test,
            async_test,
            async_sync_test,
            print_file_size,
            stat_file,
            unlink_file,
            test_type,
            debug);
    if (ret) {
        usage(argv[0]);
        return ret;
    }

    int test_type_cnt = 0;
    if (sync_test == true)
        test_type_cnt++;
    if (psync_test == true)
        test_type_cnt++;
    if (async_test == true)
        test_type_cnt++;
    if (async_sync_test == true)
        test_type_cnt++;
    if (test_type_cnt > 1) {
        usage(argv[0]);
        return -1;
    }

    if (boost::filesystem::exists(spdk_conf_str.c_str())) {
        cout << "Io configuration file: " << spdk_conf_str << " ... " << endl;
        stringstream errorMsg;
        if (BdevCpp::readConfig(spdk_conf_str.c_str(), spdk_options, errorMsg) == false) {
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

    BdevCpp::Api api(options);
    BdevCpp::SyncApi *syncApi = api.getSyncApi();
    BdevCpp::AsyncApi *asyncApi = api.getAsyncApi();

    if (open_close_test == true) {
        rc = SyncOpenCloseTest(syncApi, sync_file.c_str());
        if (rc) {
            cerr << "Sync open/close failed rc: " << rc << endl;
            return rc;
        }

        rc = AsyncOpenCloseTest(asyncApi, async_file.c_str());
        if (rc) {
            cerr << "Async open/close failed rc: " << rc << endl;
            return rc;
        }
    }

    if (min_iosize_mult < 1)
        min_iosize_mult = 1;

    if (max_queued > maxWriteFutures)
        max_queued = maxWriteFutures;

    if (num_files > 64)
        num_files = 64;

    if (test_type == eWriteTest) {
        if (sync_test == true) {
            IoStats iostats;
            memset(&iostats, '\0', sizeof(iostats));
            rc = SyncWriteIoTest(syncApi, sync_file.c_str(),
                legacy_newstack_mode, integrity_check,
                loop_count, out_loop_count,
                max_iosize_mult,
                min_iosize_mult,
                print_file_size,
                stat_file,
                unlink_file,
                &iostats);
            if (rc)
                cerr << "SyncWriteIoTest failed rc: " << rc << endl;
            else
                cout << "bytes_written: " << iostats.bytes_written << " bytes_read: " << iostats.bytes_read <<
                    " write_ios: " << iostats.write_ios << " read_ios: " << iostats.read_ios <<
                    " write MiB/sec: " << static_cast<float>(iostats.write_mbsec) << " read MiB/sec: " << static_cast<float>(iostats.read_mbsec) <<
                    " write IOPS: " << static_cast<float>(iostats.write_iops) << " read IOPS: " << static_cast<float>(iostats.read_iops) << endl;
        }

        if (psync_test == true) {
            IoStats iostats;
            memset(&iostats, '\0', sizeof(iostats));
            rc = SyncPwriteIoTest(syncApi, sync_file.c_str(),
                integrity_check,
                loop_count, out_loop_count,
                max_iosize_mult,
                min_iosize_mult,
                print_file_size,
                stat_file,
                unlink_file,
                &iostats);
            if (rc)
                cerr << "SyncPwriteIoTest failed rc: " << rc << endl;
            else
                cout << "bytes_written: " << iostats.bytes_written << " bytes_read: " << iostats.bytes_read <<
                    " write_ios: " << iostats.write_ios << " read_ios: " << iostats.read_ios <<
                    " write MiB/sec: " << static_cast<float>(iostats.write_mbsec) << " read MiB/sec: " << static_cast<float>(iostats.read_mbsec) <<
                    " write IOPS: " << static_cast<float>(iostats.write_iops) << " read IOPS: " << static_cast<float>(iostats.read_iops) << endl;
        }

        if (async_test == true) {
            if (num_files == 1) {
                IoStats iostats;
                memset(&iostats, '\0', sizeof(iostats));
                rc = AsyncWriteIoTest(asyncApi, async_file.c_str(),
                    integrity_check, loop_count, out_loop_count,
                    max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file, &iostats);
                if (rc)
                    cerr << "AsyncWriteIoTest failed rc: " << rc << endl;
                else
                    cout << "bytes_written: " << iostats.bytes_written << " bytes_read: " << iostats.bytes_read <<
                        " write_ios: " << iostats.write_ios << " read_ios: " << iostats.read_ios <<
                        " write MiB/sec: " << static_cast<float>(iostats.write_mbsec) << " read MiB/sec: " << static_cast<float>(iostats.read_mbsec) <<
                        " write IOPS: " << static_cast<float>(iostats.write_iops) << " read IOPS: " << static_cast<float>(iostats.read_iops) << endl;

            } else {
                IoStats iostats;
                memset(&iostats, '\0', sizeof(iostats));
                thread *threads[64];
                ThreadArgs *thread_args[64];
                for (size_t i = 0 ; i < num_files ; i++) {
                    string async_f = async_file + to_string(i);
                    thread_args[i] = new ThreadArgs{asyncApi, async_f, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file};
                    threads[i] = new thread(&AsyncWriteIoTestRunner, thread_args[i]);
                }

                for (size_t i = 0 ; i < num_files ; i++) {
                    threads[i]->join();
                    iostats.write_mbsec += thread_args[i]->stats.write_mbsec;
                    iostats.read_mbsec += thread_args[i]->stats.read_mbsec;
                    iostats.write_iops += thread_args[i]->stats.write_iops;
                    iostats.read_iops += thread_args[i]->stats.read_iops;
                    iostats.bytes_written += thread_args[i]->stats.bytes_written;
                    iostats.bytes_read += thread_args[i]->stats.bytes_read;
                    iostats.write_ios += thread_args[i]->stats.write_ios;
                    iostats.read_ios += thread_args[i]->stats.read_ios;
                }
                if (!rc)
                    cout << "bytes_written: " << iostats.bytes_written << " bytes_read: " << iostats.bytes_read <<
                        " write_ios: " << iostats.write_ios << " read_ios: " << iostats.read_ios <<
                        " write MiB/sec: " << static_cast<float>(iostats.write_mbsec) << " read MiB/sec: " << static_cast<float>(iostats.read_mbsec) <<
                        " write IOPS: " << static_cast<float>(iostats.write_iops) << " read IOPS: " << static_cast<float>(iostats.read_iops) << endl;
            }
        }

        if (async_sync_test == true) {
            if (num_files == 1) {
                IoStats iostats;
                memset(&iostats, '\0', sizeof(iostats));
                rc = AsyncSyncWriteIoTest(asyncApi, async_file.c_str(),
                    integrity_check, loop_count, out_loop_count,
                    max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file, &iostats);
                if (rc)
                    cerr << "AsyncSyncWriteIoTest failed rc: " << rc << endl;
                else
                    cout << "bytes_written: " << iostats.bytes_written << " bytes_read: " << iostats.bytes_read <<
                        " write_ios: " << iostats.write_ios << " read_ios: " << iostats.read_ios <<
                        " write MiB/sec: " << static_cast<float>(iostats.write_mbsec) << " read MiB/sec: " << static_cast<float>(iostats.read_mbsec) <<
                        " write IOPS: " << static_cast<float>(iostats.write_iops) << " read IOPS: " << static_cast<float>(iostats.read_iops) << endl;

            } else {
                IoStats iostats;
                memset(&iostats, '\0', sizeof(iostats));
                thread *threads[64];
                ThreadArgs *thread_args[64];
                for (size_t i = 0 ; i < num_files ; i++) {
                    string async_f = async_file + to_string(i);
                    thread_args[i] = new ThreadArgs{asyncApi, async_f, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file};
                    threads[i] = new thread(&AsyncSyncWriteIoTestRunner, thread_args[i]);
                }

                for (size_t i = 0 ; i < num_files ; i++) {
                    threads[i]->join();
                    iostats.write_mbsec += thread_args[i]->stats.write_mbsec;
                    iostats.read_mbsec += thread_args[i]->stats.read_mbsec;
                    iostats.write_iops += thread_args[i]->stats.write_iops;
                    iostats.read_iops += thread_args[i]->stats.read_iops;
                    iostats.bytes_written += thread_args[i]->stats.bytes_written;
                    iostats.bytes_read += thread_args[i]->stats.bytes_read;
                    iostats.write_ios += thread_args[i]->stats.write_ios;
                    iostats.read_ios += thread_args[i]->stats.read_ios;
                }
                if (!rc)
                    cout << "bytes_written: " << iostats.bytes_written << " bytes_read: " << iostats.bytes_read <<
                        " write_ios: " << iostats.write_ios << " read_ios: " << iostats.read_ios <<
                        " write MiB/sec: " << static_cast<float>(iostats.write_mbsec) << " read MiB/sec: " << static_cast<float>(iostats.read_mbsec) <<
                        " write IOPS: " << static_cast<float>(iostats.write_iops) << " read IOPS: " << static_cast<float>(iostats.read_iops) << endl;
            }
        }
    } else {
        if (sync_test == true) {
        IoStats iostats;
        memset(&iostats, '\0', sizeof(iostats));
        rc = SyncReadIoTest(syncApi, sync_file.c_str(),
            legacy_newstack_mode, integrity_check, 
            loop_count, out_loop_count, 
            max_iosize_mult,
            min_iosize_mult,
            print_file_size,
            stat_file,
            unlink_file,
            &iostats);
        if (rc)
            cerr << "SyncReadIoTest failed rc: " << rc << endl;
        else
            cout << "bytes_written: " << iostats.bytes_written << " bytes_read: " << iostats.bytes_read <<
                " write_ios: " << iostats.write_ios << " read_ios: " << iostats.read_ios << 
                " write MiB/sec: " << static_cast<float>(iostats.write_mbsec) << " read MiB/sec: " << static_cast<float>(iostats.read_mbsec) <<
                " write IOPS: " << static_cast<float>(iostats.write_iops) << " read IOPS: " << static_cast<float>(iostats.read_iops) << endl;
        }

        if (psync_test == true) {
        IoStats iostats;
        memset(&iostats, '\0', sizeof(iostats));
        rc = SyncPreadIoTest(syncApi, sync_file.c_str(),
            integrity_check,
            loop_count, out_loop_count,
            max_iosize_mult,
            min_iosize_mult,
            print_file_size,
            stat_file,
            unlink_file,
            &iostats);
        if (rc)
            cerr << "SyncPreadIoTest failed rc: " << rc << endl;
        else
            cout << "bytes_written: " << iostats.bytes_written << " bytes_read: " << iostats.bytes_read <<
                " write_ios: " << iostats.write_ios << " read_ios: " << iostats.read_ios <<
                " write MiB/sec: " << static_cast<float>(iostats.write_mbsec) << " read MiB/sec: " << static_cast<float>(iostats.read_mbsec) <<
                " write IOPS: " << static_cast<float>(iostats.write_iops) << " read IOPS: " << static_cast<float>(iostats.read_iops) << endl;
        }

        if (async_test == true) {
            if (num_files == 1) {
                IoStats iostats;
                memset(&iostats, '\0', sizeof(iostats));
                rc = AsyncReadIoTest(asyncApi, async_file.c_str(),
                    integrity_check, loop_count, out_loop_count,
                    max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file, &iostats);
                if (rc)
                    cerr << "AsyncReadIoTest failed rc: " << rc << endl;
                else
                    cout << "bytes_written: " << iostats.bytes_written << " bytes_read: " << iostats.bytes_read <<
                        " write_ios: " << iostats.write_ios << " read_ios: " << iostats.read_ios <<
                        " write MiB/sec: " << static_cast<float>(iostats.write_mbsec) << " read MiB/sec: " << static_cast<float>(iostats.read_mbsec) <<
                        " write IOPS: " << static_cast<float>(iostats.write_iops) << " read IOPS: " << static_cast<float>(iostats.read_iops) << endl;

            } else {
                IoStats iostats;
                memset(&iostats, '\0', sizeof(iostats));
                thread *threads[64];
                ThreadArgs *thread_args[64];
                for (size_t i = 0 ; i < num_files ; i++) {
                    string async_f = async_file + to_string(i);
                    thread_args[i] = new ThreadArgs{asyncApi, async_f, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file};
                    threads[i] = new thread(&AsyncReadIoTestRunner, thread_args[i]);
                }

                for (size_t i = 0 ; i < num_files ; i++) {
                    threads[i]->join();
                    iostats.write_mbsec += thread_args[i]->stats.write_mbsec;
                    iostats.read_mbsec += thread_args[i]->stats.read_mbsec;
                    iostats.write_iops += thread_args[i]->stats.write_iops;
                    iostats.read_iops += thread_args[i]->stats.read_iops;
                    iostats.bytes_written += thread_args[i]->stats.bytes_written;
                    iostats.bytes_read += thread_args[i]->stats.bytes_read;
                    iostats.write_ios += thread_args[i]->stats.write_ios;
                    iostats.read_ios += thread_args[i]->stats.read_ios;
                }
                if (!rc)
                    cout << "bytes_written: " << iostats.bytes_written << " bytes_read: " << iostats.bytes_read <<
                        " write_ios: " << iostats.write_ios << " read_ios: " << iostats.read_ios <<
                        " write MiB/sec: " << static_cast<float>(iostats.write_mbsec) << " read MiB/sec: " << static_cast<float>(iostats.read_mbsec) <<
                        " write IOPS: " << static_cast<float>(iostats.write_iops) << " read IOPS: " << static_cast<float>(iostats.read_iops) << endl;
            }
        }

        if (async_sync_test == true) {
            if (num_files == 1) {
                IoStats iostats;
                memset(&iostats, '\0', sizeof(iostats));
                rc = AsyncSyncReadIoTest(asyncApi, async_file.c_str(),
                    integrity_check, loop_count, out_loop_count,
                    max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file, &iostats);
                if (rc)
                    cerr << "AsyncSyncReadIoTest failed rc: " << rc << endl;
                else
                    cout << "bytes_written: " << iostats.bytes_written << " bytes_read: " << iostats.bytes_read <<
                        " write_ios: " << iostats.write_ios << " read_ios: " << iostats.read_ios <<
                        " write MiB/sec: " << static_cast<float>(iostats.write_mbsec) << " read MiB/sec: " << static_cast<float>(iostats.read_mbsec) <<
                        " write IOPS: " << static_cast<float>(iostats.write_iops) << " read IOPS: " << static_cast<float>(iostats.read_iops) << endl;

            } else {
                IoStats iostats;
                memset(&iostats, '\0', sizeof(iostats));
                thread *threads[64];
                ThreadArgs *thread_args[64];
                for (size_t i = 0 ; i < num_files ; i++) {
                    string async_f = async_file + to_string(i);
                    thread_args[i] = new ThreadArgs{asyncApi, async_f, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file};
                    threads[i] = new thread(&AsyncSyncReadIoTestRunner, thread_args[i]);
                }

                for (size_t i = 0 ; i < num_files ; i++) {
                    threads[i]->join();
                    iostats.write_mbsec += thread_args[i]->stats.write_mbsec;
                    iostats.read_mbsec += thread_args[i]->stats.read_mbsec;
                    iostats.write_iops += thread_args[i]->stats.write_iops;
                    iostats.read_iops += thread_args[i]->stats.read_iops;
                    iostats.bytes_written += thread_args[i]->stats.bytes_written;
                    iostats.bytes_read += thread_args[i]->stats.bytes_read;
                    iostats.write_ios += thread_args[i]->stats.write_ios;
                    iostats.read_ios += thread_args[i]->stats.read_ios;
                }
                if (!rc)
                    cout << "bytes_written: " << iostats.bytes_written << " bytes_read: " << iostats.bytes_read <<
                        " write_ios: " << iostats.write_ios << " read_ios: " << iostats.read_ios <<
                        " write MiB/sec: " << static_cast<float>(iostats.write_mbsec) << " read MiB/sec: " << static_cast<float>(iostats.read_mbsec) <<
                        " write IOPS: " << static_cast<float>(iostats.write_iops) << " read IOPS: " << static_cast<float>(iostats.read_iops) << endl;
            }
        }
    }

    api.QuiesceIO(true);
    return rc;
}
