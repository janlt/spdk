#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <libaio.h>
#include <fcntl.h>
#include <string.h>
#include <getopt.h>
#include <time.h>

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

static timespec io_time_diff(timespec start, timespec end) {
    timespec temp;
    if ((end.tv_nsec-start.tv_nsec) < 0) {
        temp.tv_sec = end.tv_sec-start.tv_sec - 1;
        temp.tv_nsec = 1000000000 + end.tv_nsec - start.tv_nsec;
    } else {
        temp.tv_sec = end.tv_sec - start.tv_sec;
        temp.tv_nsec = end.tv_nsec - start.tv_nsec;
    }
    return temp;
}

static int dt_read_count = 1;
static int dt_write_count = 1;

static timespec io_diff_read_t;
static timespec io_diff_write_t;

static timespec io_avg_read_t;
static timespec io_avg_write_t;

#define IO_LAT_AVG_INCR 256
#define IO_LAT_AVG_INTERVAL (1 << 16)

#define JANS_CLOCK_MODE CLOCK_MONOTONIC

inline void io_update_read_avg(int dt_read_count) {
    if (!(dt_read_count%IO_LAT_AVG_INCR)) {
        io_avg_read_t.tv_nsec += (io_diff_read_t.tv_nsec/IO_LAT_AVG_INCR);
        io_avg_read_t.tv_sec += (io_diff_read_t.tv_sec/IO_LAT_AVG_INCR);
        io_diff_read_t.tv_sec = 0;
        io_diff_read_t.tv_nsec = 0;
    }
}

inline void io_update_write_avg(int dt_write_count) {
    if (!(dt_write_count%IO_LAT_AVG_INCR)) {
        io_avg_write_t.tv_nsec += (io_diff_write_t.tv_nsec/IO_LAT_AVG_INCR);
        io_avg_write_t.tv_sec += (io_diff_write_t.tv_sec/IO_LAT_AVG_INCR);
        io_diff_write_t.tv_sec = 0;
        io_diff_write_t.tv_nsec = 0;
    }
}

inline void io_get_read_avg(int &dt_read_count) {
    if (!((dt_read_count++)%IO_LAT_AVG_INTERVAL)) {
        timespec tmp_t;
        int tmp_count = dt_read_count/IO_LAT_AVG_INCR;
        tmp_t.tv_sec = io_avg_read_t.tv_sec/tmp_count;
        tmp_t.tv_nsec = io_avg_read_t.tv_nsec/tmp_count;

        cout << "io_read_lat.sec: " << tmp_t.tv_sec << " io_read_lat.nsec: " << tmp_t.tv_nsec << endl;
        dt_read_count = 1;
        io_diff_read_t.tv_sec = 0;
        io_diff_read_t.tv_nsec = 0;
        io_avg_read_t.tv_sec = 0;
        io_avg_read_t.tv_nsec = 0;
    }
}

inline void io_get_write_avg(int &dt_write_count) {
    if (!((dt_write_count++)%IO_LAT_AVG_INTERVAL)) {
        timespec tmp_t;
        int tmp_count = dt_write_count/IO_LAT_AVG_INCR;
        tmp_t.tv_sec = io_avg_write_t.tv_sec/tmp_count;
        tmp_t.tv_nsec = io_avg_write_t.tv_nsec/tmp_count;

        cout << "io_write_lat.sec: " << tmp_t.tv_sec << " io_write_lat.nsec: " << tmp_t.tv_nsec << endl;
        dt_write_count = 1;
        io_diff_write_t.tv_sec = 0;
        io_diff_write_t.tv_nsec = 0;
        io_avg_write_t.tv_sec = 0;
        io_avg_write_t.tv_nsec = 0;
    }

}

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
        size_t max_iosize_mult,
        size_t min_iosize_mult,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        bool avg_lat,
        IoStats *st) {
    int rc = 0;
    char *io_buf_nal = new char[MAX_STACK_IO_SIZE];
    char *io_buf = reinterpret_cast<char *>(reinterpret_cast<uint64_t>(io_buf_nal) & ~0xfff);
    char *io_cmp_buf = new char[MAX_STACK_IO_SIZE];
    if (!io_buf || !io_cmp_buf) {
        cerr << "Can't alloc buffer" << endl;
        return -1;
    }

    timespec start_t, end_t, diff_t;

    int fd = !mode ? api->open(file_name, O_RDWR | O_CREAT | O_DIRECT, S_IRUSR | S_IWUSR) :
        ::open(file_name, O_RDWR | O_CREAT | O_DIRECT, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    if (print_file_size == true)
        cout << "Sync file: " << file_name << " size: " << getSyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = !mode ? api->stat(file_name, &st) : ::stat(file_name, &st);
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

            if (avg_lat == true)
                clock_gettime(JANS_CLOCK_MODE, &start_t);

            rc = !mode ? api->write(fd, io_buf, io_size) : ::write(fd, io_buf, io_size);
            if (rc < 0) {
                cerr << "write sync failed rc: " << rc << " errno: " << errno << endl;
                break;
            }

            if (avg_lat == true) {
                clock_gettime(JANS_CLOCK_MODE, &end_t);
                diff_t = io_time_diff(start_t, end_t);

                io_diff_write_t.tv_sec += diff_t.tv_sec;
                io_diff_write_t.tv_nsec += diff_t.tv_nsec;

                io_update_write_avg(dt_write_count);
                io_get_write_avg(dt_write_count);
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
        rc = !mode ? api->stat(file_name, &st) : ::stat(file_name, &st);
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
        delete [] io_buf_nal;
        delete [] io_cmp_buf;
    }

    return rc > 0 ? 0 : rc;
}

//
// Sync IO tests
//
static int SyncPwriteIoTest(BdevCpp::SyncApi *api,
        const char *file_name, int mode, int check,
        int loop_count, int out_loop_count,
        size_t max_iosize_mult,
        size_t min_iosize_mult,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        bool avg_lat,
        IoStats *st) {
    int rc = 0;
    char *io_buf_nal = new char[MAX_STACK_IO_SIZE];
    char *io_buf = reinterpret_cast<char *>(reinterpret_cast<uint64_t>(io_buf_nal) & ~0xfff);
    char *io_cmp_buf = new char[MAX_STACK_IO_SIZE];
    if (!io_buf || !io_cmp_buf) {
        cerr << "Can't alloc buffer" << endl;
        return -1;
    }

    int fd = !mode ? api->open(file_name, O_RDWR | O_CREAT | O_DIRECT, S_IRUSR | S_IWUSR) :
        ::open(file_name, O_RDWR | O_CREAT | O_DIRECT, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    timespec start_t, end_t, diff_t;

    if (print_file_size == true)
        cout << "Sync file: " << file_name << " size: " << getSyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = !mode ? api->stat(file_name, &st) : ::stat(file_name, &st);
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

            if (avg_lat == true)
                clock_gettime(JANS_CLOCK_MODE, &start_t);

            rc = !mode ? api->pwrite(fd, io_buf, io_size, static_cast<off_t>(pos)) : ::pwrite(fd, io_buf, io_size, static_cast<off_t>(pos));
            if (rc < 0) {
                cerr << "pwrite sync failed rc: " << rc << " errno: " << errno << endl;
                break;
            }

            if (avg_lat == true) {
                clock_gettime(JANS_CLOCK_MODE, &end_t);
                diff_t = io_time_diff(start_t, end_t);

                io_diff_write_t.tv_sec += diff_t.tv_sec;
                io_diff_write_t.tv_nsec += diff_t.tv_nsec;

                io_update_write_avg(dt_write_count);
                io_get_write_avg(dt_write_count);
            }

            pos += io_size;
            write_ios++;
            bytes_written += io_size;
        }

        if (rc >= 0 && check) {
            for (int i = 0 ; i < loop_count ; i++) {
                size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
                ::memset(io_cmp_buf, 'a' + i%20, io_size);

                rc = !mode ? api->pread(fd, io_buf, io_size, static_cast<off_t>(check_pos)) : ::pread(fd, io_buf, io_size, static_cast<off_t>(check_pos));
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
        rc = !mode ? api->stat(file_name, &st) : ::stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    if (io_buf && io_cmp_buf) {
        delete [] io_buf_nal;
        delete [] io_cmp_buf;
    }

    if (!mode)
        api->close(fd);
    else
        ::close(fd);

    if (unlink_file == true) {
        rc = !mode ? api->unlink(file_name) : ::unlink(file_name);
        if (rc < 0)
            cerr << "Unlink file: " << file_name << " failed" << endl;
    }

    return rc > 0 ? 0 : rc;
}

static int SyncReadIoTest(BdevCpp::SyncApi *api,
        const char *file_name, int mode, int check,
        int loop_count, int out_loop_count,
        size_t max_iosize_mult, size_t min_iosize_mult,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        bool avg_lat,
        IoStats *st) {
    int rc = 0;
    char *io_buf_nal = new char[MAX_STACK_IO_SIZE];
    char *io_buf = reinterpret_cast<char *>(reinterpret_cast<uint64_t>(io_buf_nal) & ~0xfff);
    if (!io_buf) {
        cerr << "Can't alloc buffer" << endl;
        return -1;
    }

    int fd = !mode ? api->open(file_name, O_RDWR | O_CREAT | O_DIRECT, S_IRUSR | S_IWUSR) :
        ::open(file_name, O_RDWR | O_CREAT | O_DIRECT, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    timespec start_t, end_t, diff_t;

    if (print_file_size == true)
        cout << "Sync file: " << file_name << " size: " << getSyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = !mode ? api->stat(file_name, &st) : ::stat(file_name, &st);
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

            if (avg_lat == true)
                clock_gettime(JANS_CLOCK_MODE, &start_t);

            rc = !mode ? api->read(fd, io_buf, io_size) : ::read(fd, io_buf, io_size);
            if (rc < 0) {
                cerr << "read sync failed rc: " << rc << " errno: " << errno << endl;
                break;
            }

            if (avg_lat == true) {
                clock_gettime(JANS_CLOCK_MODE, &end_t);
                diff_t = io_time_diff(start_t, end_t);

                io_diff_read_t.tv_sec += diff_t.tv_sec;
                io_diff_read_t.tv_nsec += diff_t.tv_nsec;

                io_update_read_avg(dt_read_count);
                io_get_read_avg(dt_read_count);
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
        rc = !mode ? api->stat(file_name, &st) : ::stat(file_name, &st);
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

    if (io_buf)
        delete [] io_buf_nal;

    return rc > 0 ? 0 : rc;
}

static int SyncPreadIoTest(BdevCpp::SyncApi *api,
        const char *file_name, int mode, int check,
        int loop_count, int out_loop_count,
        size_t max_iosize_mult, size_t min_iosize_mult,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        bool avg_lat,
        IoStats *st) {
    int rc = 0;
    char *io_buf_nal = new char[MAX_STACK_IO_SIZE];
    char *io_buf = reinterpret_cast<char *>(reinterpret_cast<uint64_t>(io_buf_nal) & ~0xfff);
    if (!io_buf) {
        cerr << "Can't alloc buffer" << endl;
        return -1;
    }

    int fd = !mode ? api->open(file_name, O_RDWR | O_CREAT | O_DIRECT, S_IRUSR | S_IWUSR) :
        ::open(file_name, O_RDWR | O_CREAT | O_DIRECT, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    timespec start_t, end_t, diff_t;

    if (print_file_size == true)
        cout << "Sync file: " << file_name << " size: " << getSyncFileSize(api, fd) << endl;

    if (stat_file == true) {
        struct stat st;
        rc = !mode ? api->stat(file_name, &st) : ::stat(file_name, &st);
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

            if (avg_lat == true)
                clock_gettime(JANS_CLOCK_MODE, &start_t);

            rc = !mode ? api->pread(fd, io_buf, io_size, static_cast<off_t>(pos)) : ::pread(fd, io_buf, io_size, static_cast<off_t>(pos));
            if (rc < 0) {
                cerr << "pread sync failed rc: " << rc << " errno: " << errno << endl;
                break;
            }

            if (avg_lat == true) {
                clock_gettime(JANS_CLOCK_MODE, &end_t);
                diff_t = io_time_diff(start_t, end_t);

                io_diff_read_t.tv_sec += diff_t.tv_sec;
                io_diff_read_t.tv_nsec += diff_t.tv_nsec;

                io_update_read_avg(dt_read_count);
                io_get_read_avg(dt_read_count);
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
        rc = !mode ? api->stat(file_name, &st) : ::stat(file_name, &st);
        if (rc < 0)
            cerr << "Stat file: " << file_name << " failed" << endl;
        else
            cout << "Stat file: st.st_size: " << st.st_size <<
                    " st.st_blocks: " << st.st_blocks <<
                    " st.st_blksize: " << st.st_blksize << endl;
    }

    if (io_buf)
        delete [] io_buf_nal;

    if (!mode)
        api->close(fd);
    else
        ::close(fd);

    if (unlink_file == true) {
        rc = !mode ? api->unlink(file_name) : ::unlink(file_name);
        if (rc < 0)
            cerr << "Unlink file: " << file_name << " failed" << endl;
    }

    return rc > 0 ? 0 : rc;
}

//
// Async IO tests
//
const size_t maxWriteFutures = 2048;
const size_t maxReadFutures = 2048;

static int AsyncIoCompleteWrites(BdevCpp::AsyncApi *api,
        size_t &write_ios,
        size_t &bytes_written,
        size_t &num_write_futures,
        BdevCpp::FutureBase *write_futures[],
        int poll_block_mode,
        bool avg_lat) {
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
        size_t io_sizes[],
        int poll_block_mode,
        bool avg_lat) {
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
        size_t max_iosize_mult,
        size_t min_iosize_mult,
        size_t max_queued,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        int poll_block_mode,
        bool avg_lat,
        IoStats *st) {
    BdevCpp::FutureBase *write_futures[maxWriteFutures];
    size_t num_write_futures = 0;
    BdevCpp::FutureBase *read_futures[maxReadFutures];
    size_t num_read_futures = 0;

    char *io_buffers[maxWriteFutures];
    char *io_cmp_buffers[maxReadFutures];
    size_t io_sizes[maxReadFutures];

    int rc = 0;

    int fd = api->open(file_name, O_RDWR | O_CREAT | O_DIRECT, S_IRUSR | S_IWUSR);
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

    bool poll_block = poll_block_mode ? true : false;

    for (int j = 0 ; j < out_loop_count ; j++) {
        check_pos = pos;
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
            if (check)
                ::memset(io_buffers[num_write_futures], 'a' + i%20, io_size);

            write_futures[num_write_futures] = api->write(fd, pos, io_buffers[num_write_futures], io_size, poll_block);
            num_write_futures++;
            pos += io_size;

            if (!write_futures[num_write_futures - 1]) {
                rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures, poll_block_mode, avg_lat);
                if (rc < 0)
                    break;
                continue;
            }

            if (num_write_futures >= max_queued || num_write_futures >= maxWriteFutures)
                rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures, poll_block_mode, avg_lat);
        }

        rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures, poll_block_mode, avg_lat);

        if (!rc && check) {
            for (int i = 0 ; i < loop_count ; i++) {
                size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
                ::memset(io_cmp_buffers[num_read_futures], 'a' + i%20, io_size);
                io_sizes[num_read_futures] = io_size;

                read_futures[num_read_futures] = api->read(fd, check_pos, io_buffers[num_read_futures], io_size, poll_block);
                num_read_futures++;
                check_pos += io_size;

                if (!read_futures[num_read_futures - 1]) {
                    rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, io_cmp_buffers, io_sizes, poll_block_mode, avg_lat);
                    if (rc < 0)
                        break;
                    continue;
                }

                if (num_read_futures > max_queued || num_read_futures >= maxReadFutures)
                    rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, io_cmp_buffers, io_sizes, poll_block_mode, avg_lat);
            }

            rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, io_cmp_buffers, io_sizes, poll_block_mode, avg_lat);
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

std::atomic<uint64_t> fd_pos;

static int AsyncWriteFdIoTest(BdevCpp::AsyncApi *api,
        int fd,
        int loop_count, int out_loop_count, 
        size_t max_iosize_mult,
        size_t min_iosize_mult,
        size_t max_queued,
        int poll_block_mode,
        bool avg_lat,
        IoStats *st) {
    BdevCpp::FutureBase *write_futures[maxWriteFutures];
    size_t num_write_futures = 0;

    char *io_buffers[maxWriteFutures];

    int rc = 0;

    for (size_t i = 0 ; i < maxWriteFutures ; i++)
        io_buffers[i] = new char[MAX_STACK_IO_SIZE];

    time_t stime = printTimeNow("Start write (verify) async fd test ");

    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
    uint64_t write_ios = 0;
    uint64_t read_ios = 0;

    bool poll_block = poll_block_mode ? true : false;

    for (int j = 0 ; j < out_loop_count ; j++) {
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
            write_futures[num_write_futures] = api->write(fd, fd_pos, io_buffers[num_write_futures], io_size, poll_block);
            num_write_futures++;
            fd_pos += io_size;

            if (!write_futures[num_write_futures - 1]) {
                rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures, poll_block_mode, avg_lat);
                if (rc < 0)
                    break;
                continue;
            }

            if (num_write_futures >= max_queued || num_write_futures >= maxWriteFutures)
                rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures, poll_block_mode, avg_lat);
        }

        rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures, poll_block_mode, avg_lat);
    }

    time_t etime = printTimeNow("End async fd test");
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

    for (size_t i = 0 ; i < maxWriteFutures ; i++)
        delete [] io_buffers[i];

    return rc;
}

static int AsyncSyncWriteIoTest(BdevCpp::AsyncApi *api,
        const char *file_name, int check,
        int loop_count, int out_loop_count,
        size_t max_iosize_mult,
        size_t min_iosize_mult,
        size_t max_queued,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        int poll_block_mode,
        bool avg_lat,
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

    int fd = api->open(file_name, O_RDWR | O_CREAT | O_DIRECT, S_IRUSR | S_IWUSR);
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

    bool poll_block = poll_block_mode ? true : false;

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
                write_futures[num_write_futures] = api->write(fd, pos, io_buffers[num_write_futures], io_size, poll_block);
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
                rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures, poll_block_mode, avg_lat);
                if (rc < 0)
                    break;
                continue;
            }

            if (num_write_futures >= max_queued || num_write_futures >= maxWriteFutures)
                rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures, poll_block_mode, avg_lat);
        }

        if (failed == true)
            break;

        rc = AsyncIoCompleteWrites(api, write_ios, bytes_written, num_write_futures, write_futures, poll_block_mode, avg_lat);

        if (!rc && check) {
            int alternate = 0;
            for (int i = 0 ; i < loop_count ; i++) {
                size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
                ::memset(io_cmp_buffers[num_read_futures], 'a' + i%20, io_size);
                ::memset(io_cmp_buf, 'a' + i%20, io_size);
                io_sizes[num_read_futures] = io_size;

                if (alternate) {
                    read_futures[num_read_futures] = api->read(fd, check_pos, io_buffers[num_read_futures], io_size, poll_block);
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
                    rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, io_cmp_buffers, io_sizes, poll_block_mode, avg_lat);
                    if (rc < 0)
                        break;
                    continue;
                }

                if (num_read_futures > max_queued || num_read_futures >= maxReadFutures)
                    rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, io_cmp_buffers, io_sizes, poll_block_mode, avg_lat);
            }

            rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, io_cmp_buffers, io_sizes, poll_block_mode, avg_lat);
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

static int AsyncReadFdIoTest(BdevCpp::AsyncApi *api,
        int fd,
        int loop_count, int out_loop_count,
        size_t max_iosize_mult,
        size_t min_iosize_mult,
        size_t max_queued,
        int poll_block_mode,
        bool avg_lat,
        IoStats *st) {
    BdevCpp::FutureBase *read_futures[maxReadFutures];
    size_t num_read_futures = 0;

    char *io_buffers[maxReadFutures];
    size_t io_sizes[maxReadFutures];

    int rc = 0;

    for (size_t i = 0 ; i < maxReadFutures ; i++)
        io_buffers[i] = new char[MAX_STACK_IO_SIZE];

    time_t stime = printTimeNow("Start read async fd test ");

    uint64_t bytes_read = 0;
    uint64_t read_ios = 0;

    bool poll_block = poll_block_mode ? true : false;

    for (int j = 0 ; j < out_loop_count ; j++) {
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);

            read_futures[num_read_futures] = api->read(fd, fd_pos, io_buffers[num_read_futures], io_size, poll_block);
            num_read_futures++;
            fd_pos += io_size;

            if (!read_futures[num_read_futures - 1]) {
                rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes, poll_block_mode, avg_lat);
                if (rc < 0)
                    break;
                continue;
            }

            if (num_read_futures >= max_queued || num_read_futures >= maxReadFutures)
                rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes, poll_block_mode, avg_lat);
        }

        rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes, poll_block_mode, avg_lat);
    }

    time_t etime = printTimeNow("End async fd test");
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

    for (size_t i = 0 ; i < maxWriteFutures ; i++)
        delete [] io_buffers[i];

    return rc;
}

static int AsyncReadIoTest(BdevCpp::AsyncApi *api,
        const char *file_name, int check,
        int loop_count, int out_loop_count,
        size_t max_iosize_mult,
        size_t min_iosize_mult,
        size_t max_queued,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        int poll_block_mode,
        bool avg_lat,
        IoStats *st) {
    BdevCpp::FutureBase *read_futures[maxReadFutures];
    size_t num_read_futures = 0;

    char *io_buffers[maxReadFutures];
    size_t io_sizes[maxReadFutures];

    int rc = 0;

    int fd = api->open(file_name, O_RDWR | O_CREAT | O_DIRECT, S_IRUSR | S_IWUSR);
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

    bool poll_block = poll_block_mode ? true : false;

    for (int j = 0 ; j < out_loop_count ; j++) {
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);

            read_futures[num_read_futures] = api->read(fd, pos, io_buffers[num_read_futures], io_size, poll_block);
            num_read_futures++;
            pos += io_size;

            if (!read_futures[num_read_futures - 1]) {
                rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes, poll_block_mode, avg_lat);
                if (rc < 0)
                    break;
                continue;
            }

            if (num_read_futures >= max_queued || num_read_futures >= maxReadFutures)
                rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes, poll_block_mode, avg_lat);
        }

        rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes, poll_block_mode, avg_lat);
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
        size_t max_iosize_mult,
        size_t min_iosize_mult,
        size_t max_queued,
        bool print_file_size,
        bool stat_file,
        bool unlink_file,
        int poll_block_mode,
        bool avg_lat,
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

    int fd = api->open(file_name, O_RDWR | O_CREAT | O_DIRECT, S_IRUSR | S_IWUSR);
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

    bool poll_block = poll_block_mode ? true : false;

    bool failed = false;
    for (int j = 0 ; j < out_loop_count ; j++) {
        int alternate = 0;
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);

            if (alternate) {
                read_futures[num_read_futures] = api->read(fd, pos, io_buffers[num_read_futures], io_size, poll_block);
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
                rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes, poll_block_mode, avg_lat);
                if (rc < 0)
                    break;
                continue;
            }

            if (num_read_futures >= max_queued || num_read_futures >= maxReadFutures)
                rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes, poll_block_mode, avg_lat);
        }

        rc = AsyncIoCompleteReads(api, read_ios, bytes_read, num_read_futures, read_futures, io_buffers, 0, io_sizes, poll_block_mode, avg_lat);

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

const size_t maxWriteAios = 2048;
const size_t maxReadAios = 2048;

static int AsyncAioIoCompleteWrites(io_context_t *ctx,
        size_t &write_ios,
        size_t &bytes_written,
        size_t &num_write_aios,
        struct iocb *write_aios[],
        struct timespec *timeout,
        bool avg_lat,
        timespec &start_t, timespec &end_t, timespec &diff_t) {
    int rc = 0;
    struct io_event events[maxWriteAios];

    size_t curr_aios = 0;
    for ( ;; ) {
        if (curr_aios >= num_write_aios)
            break;

        int w_rc = io_getevents(*ctx, 1, num_write_aios, events, timeout);
        if (w_rc < 0) {
            cerr << "io_getevents failed errno: " << errno << endl;
            rc = -1;
            break;
        }

        if (avg_lat == true) {
            clock_gettime(JANS_CLOCK_MODE, &end_t);
            diff_t = io_time_diff(start_t, end_t);

            io_diff_write_t.tv_sec += diff_t.tv_sec;
            io_diff_write_t.tv_nsec += diff_t.tv_nsec;

            io_update_write_avg(dt_write_count);
            io_get_write_avg(dt_write_count);
        }

        for (size_t i = curr_aios ; i < curr_aios + w_rc ; i++) {
            write_ios++;
            bytes_written += write_aios[i]->u.c.nbytes;
        }

        curr_aios += w_rc;
    }
    num_write_aios = 0;

    return rc;
}

static int AsyncAioIoCompleteReads(io_context_t *ctx,
        size_t &read_ios,
        size_t &bytes_read,
        size_t &num_read_aios,
        struct iocb *read_aios[],
        char *io_buffers[],
        char *io_cmp_buffers[],
        size_t io_sizes[],
        struct timespec *timeout,
        bool avg_lat,
        timespec &start_t, timespec &end_t, timespec &diff_t) {
    int rc = 0;
    struct io_event events[maxWriteAios];

    size_t curr_aios = 0;
    for ( ;; ) {
        if (curr_aios >= num_read_aios)
            break;

        int r_rc = io_getevents(*ctx, 1, num_read_aios, events, timeout);
        if (r_rc < 0) {
            cerr << "io_getevents failed errno: " << errno << endl;
            rc = -1;
            break;
        }

        if (avg_lat == true) {
            clock_gettime(JANS_CLOCK_MODE, &end_t);
            diff_t = io_time_diff(start_t, end_t);

            io_diff_write_t.tv_sec += diff_t.tv_sec;
            io_diff_write_t.tv_nsec += diff_t.tv_nsec;

            io_update_write_avg(dt_write_count);
            io_get_write_avg(dt_write_count);
        }

        for (size_t i = curr_aios ; i < curr_aios + r_rc ; i++) {
            read_ios++;
            bytes_read += read_aios[i]->u.c.nbytes;

            if (io_cmp_buffers && ::memcmp(io_buffers[i], io_cmp_buffers[i], io_sizes[i])) {
                cerr << "Corrupted data after io_getevents at idx: " << i << " io_size: " << io_sizes[i] << endl;
                rc = -1;
                break;
            }
        }

        curr_aios += r_rc;
    }
    num_read_aios = 0;

    return rc;
}

static int AsyncAioWriteIoTest(const char *file_name, int check,
        int loop_count, int out_loop_count,
        size_t max_iosize_mult,
        size_t min_iosize_mult,
        size_t max_queued,
        bool avg_lat,
        IoStats *st) {
    io_context_t ctx;
    struct iocb *write_aios[maxWriteAios];
    size_t num_write_aios = 0;
    struct iocb *read_aios[maxReadAios];
    size_t num_read_aios = 0;

    char *io_buffers[maxWriteFutures];
    char *io_cmp_buffers[maxReadFutures];
    size_t io_sizes[maxReadFutures];
    struct timespec tout = {1, 11111111};

    int rc = 0;
    timespec start_t, end_t, diff_t;

    int fd = ::open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    max_queued = max_queued > maxWriteAios ? maxWriteAios : max_queued;

    rc = io_setup(max_queued, &ctx);
    if (rc < 0) {
        cerr << "io_submit failed errno: " << errno << endl;
        return rc;
    }

    for (size_t i = 0 ; i < max_queued ; i++) {
        write_aios[i] = new struct iocb;
        read_aios[i] = new struct iocb;
    }

    for (size_t i = 0 ; i < maxWriteAios ; i++)
        io_buffers[i] = new char[MAX_STACK_IO_SIZE];
    for (size_t i = 0 ; i < maxReadAios ; i++)
        io_cmp_buffers[i] = new char[MAX_STACK_IO_SIZE];

    time_t stime = printTimeNow("Start write (verify) Aio async test ");

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
                ::memset(io_buffers[num_write_aios], 'a' + i%20, io_size);

            ::memset(write_aios[num_write_aios], '\0', sizeof(struct iocb));
            io_prep_pwrite(write_aios[num_write_aios], fd, io_buffers[num_write_aios], io_size, pos);

            if (avg_lat == true)
                clock_gettime(JANS_CLOCK_MODE, &start_t);

            rc = io_submit(ctx, 1, &write_aios[num_write_aios]);
            if (rc != 1) {
                cerr << "io_submit failed errno: " << errno << endl;
                break;
            }
            num_write_aios++;
            pos += io_size;

            if (num_write_aios >= max_queued || num_write_aios >= maxWriteAios) {
                rc = AsyncAioIoCompleteWrites(&ctx, write_ios, bytes_written, num_write_aios, write_aios, &tout,
                        avg_lat, start_t, end_t, diff_t);
                if (rc < 0)
                    break;
            }
        }

        rc = AsyncAioIoCompleteWrites(&ctx, write_ios, bytes_written, num_write_aios, write_aios, &tout,
                avg_lat, start_t, end_t, diff_t);

        if (!rc && check) {
            for (int i = 0 ; i < loop_count ; i++) {
                size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);
                ::memset(io_cmp_buffers[num_read_aios], 'a' + i%20, io_size);
                io_sizes[num_read_aios] = io_size;

                ::memset(read_aios[num_read_aios], '\0', sizeof(struct iocb));
                io_prep_pread(read_aios[num_read_aios], fd, io_cmp_buffers[num_read_aios], io_size, check_pos);
                rc = io_submit(ctx, 1, &read_aios[num_read_aios]);
                if (rc != 1) {
                    cerr << "io_submit failed errno: " << errno << endl;
                    break;
                }
                num_read_aios++;
                check_pos += io_size;

                if (num_read_aios > max_queued || num_read_aios >= maxReadAios) {
                    rc = AsyncAioIoCompleteReads(&ctx, read_ios, bytes_read, num_read_aios, read_aios,
                            io_buffers, io_cmp_buffers, io_sizes, &tout,
                            avg_lat, start_t, end_t, diff_t);
                    if (rc < 0)
                        break;
                }
            }

            rc = AsyncAioIoCompleteReads(&ctx, read_ios, bytes_read, num_read_aios, read_aios,
                    io_buffers, io_cmp_buffers, io_sizes, &tout,
                    avg_lat, start_t, end_t, diff_t);
        }
    }

    time_t etime = printTimeNow("End Aio async test");
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

    io_destroy(ctx);

    rc = ::close(fd);

    for (size_t i = 0 ; i < maxWriteAios ; i++)
        delete [] io_buffers[i];
    for (size_t i = 0 ; i < maxReadAios ; i++)
        delete [] io_cmp_buffers[i];

    for (size_t i = 0 ; i < max_queued ; i++) {
        delete write_aios[i];
        delete read_aios[i];
    }

    return rc;
}

static int AsyncAioReadIoTest(const char *file_name, int check,
        int loop_count, int out_loop_count,
        size_t max_iosize_mult,
        size_t min_iosize_mult,
        size_t max_queued,
        bool avg_lat,
        IoStats *st) {
    io_context_t ctx;
    struct iocb *read_aios[maxReadAios];
    size_t num_read_aios = 0;

    char *io_buffers[maxWriteFutures];
    size_t io_sizes[maxReadFutures];
    struct timespec tout = {1, 11111111};

    int rc = 0;
    timespec start_t, end_t, diff_t;

    int fd = ::open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    max_queued = max_queued > maxReadAios ? maxReadAios : max_queued;

    rc = io_setup(max_queued, &ctx);
    if (rc < 0) {
        cerr << "io_submit failed errno: " << errno << endl;
        return rc;
    }

    for (size_t i = 0 ; i < max_queued ; i++)
        read_aios[i] = new struct iocb;

    for (size_t i = 0 ; i < maxWriteAios ; i++)
        io_buffers[i] = new char[MAX_STACK_IO_SIZE];

    time_t stime = printTimeNow("Start read Aio async test ");

    uint64_t bytes_read = 0;
    uint64_t read_ios = 0;
    uint64_t pos = 0;

    for (int j = 0 ; j < out_loop_count ; j++) {
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(512, i, min_iosize_mult, max_iosize_mult);

            ::memset(read_aios[i], '\0', sizeof(struct iocb));
            io_prep_pread(read_aios[i], fd, io_buffers[num_read_aios], io_size, pos);

            if (avg_lat == true)
                clock_gettime(JANS_CLOCK_MODE, &start_t);

            rc = io_submit(ctx, 1, &read_aios[i]);
            if (rc != 1) {
                cerr << "io_submit failed errno: " << errno << endl;
                break;
            }

            if (avg_lat == true) {
                clock_gettime(JANS_CLOCK_MODE, &end_t);
                diff_t = io_time_diff(start_t, end_t);

                io_diff_write_t.tv_sec += diff_t.tv_sec;
                io_diff_write_t.tv_nsec += diff_t.tv_nsec;

                io_update_write_avg(dt_write_count);
                io_get_write_avg(dt_write_count);
            }

            num_read_aios++;
            pos += io_size;

            if (num_read_aios >= max_queued || num_read_aios >= maxReadAios) {
                rc = AsyncAioIoCompleteReads(&ctx, read_ios, bytes_read, num_read_aios, read_aios, io_buffers, 0, io_sizes, &tout,
                        avg_lat, start_t, end_t, diff_t);
                if (rc < 0)
                    break;
            }
        }

        rc = AsyncAioIoCompleteReads(&ctx, read_ios, bytes_read, num_read_aios, read_aios, io_buffers, 0, io_sizes, &tout,
                avg_lat, start_t, end_t, diff_t);
    }

    time_t etime = printTimeNow("End Aio async test");
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

    io_destroy(ctx);

    rc = ::close(fd);

    for (size_t i = 0 ; i < maxWriteAios ; i++)
        delete [] io_buffers[i];

    for (size_t i = 0 ; i < max_queued ; i++)
        delete read_aios[i];

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
        "  -b, --poll-block-mode          Poll (default 1) vs. block mode async API\n"
        "  -e, --print-avg-latency        Print average I/O latency\n"
        "  -I, --aio-test                 Run AIO test\n"
        "  -O, --open-close-test          Run open/clsoe test\n"
        "  -S, --sync-test                Run IO sync (read/write) test\n"
        "  -P, --psync-test               Run IO psync (pwrite/pread) test\n"
        "  -A, --async-test               Run IO async test\n"
        "  -M, --async-sync-test          Run IO same file async/sync test\n"
        "  -N, --async-fd-test            Run IO one file async test\n"
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
        bool &async_fd_test,
        bool &print_file_size,
        bool &stat_file,
        bool &unlink_file,
        int &poll_block_mode,
        bool &avg_lat,
        eTestType &test_type,
        bool &aio_test,
        bool &debug)
{
    int c = 0;
    int ret = -1;

    while (1) {
        static char short_options[] = "c:s:a:m:n:l:i:z:t:w:b:q:dehOSPAIMNRWLZY";
        static struct option long_options[] = {
            {"spdk-conf",               1, 0, 'c'},
            {"sync-file",               1, 0, 's'},
            {"async-file",              1, 0, 'a'},
            {"loop-count",              1, 0, 'm'},
            {"outer-loop-count",        1, 0, 'n'},
            {"legacy-newstack-mode",    1, 0, 'l'},
            {"integrity-check",         1, 0, 'i'},
            {"poll-block-mode",         1, 0, 'b'},
            {"print-avg-latency",       1, 0, 'e'},
            {"max-queued",              1, 0, 'q'},
            {"max-iosize-mult",         1, 0, 'z'},
            {"min-iosize-mult",         1, 0, 'w'},
            {"num-files",               1, 0, 't'},
            {"open-close-test",         0, 0, 'O'},
            {"sync-test",               0, 0, 'S'},
            {"psync-test",              0, 0, 'P'},
            {"aio-test",                0, 0, 'I'},
            {"async-test",              0, 0, 'A'},
            {"async-sync-test",         0, 0, 'M'},
            {"async-fd-test",           0, 0, 'N'},
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

        case 'b':
            poll_block_mode = atoi(optarg);
            break;

        case 'e':
            avg_lat = true;
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

        case 'I':
            aio_test = true;
            break;

        case 'M':
            async_sync_test = true;
            break;

        case 'N':
            async_fd_test = true;
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
    BdevCpp::ApiBase *api;
    string file;
    int mode;
    int fd;
    bool int_check;
    int l_cnt;
    int out_l_cnt;
    size_t max_ios_m; 
    size_t min_ios_m;
    size_t m_q;
    bool print_file_size;
    bool stat_file;
    bool unlink_file;
    int poll_block_mode;
    bool avg_lat;
    IoStats stats;
    int rc;
};


static void SyncPwriteIoTestRunner(ThreadArgs *targs) {
    int rc = SyncPwriteIoTest(dynamic_cast<BdevCpp::SyncApi *>(targs->api), targs->file.c_str(),
        targs->mode, targs->int_check, targs->l_cnt, targs->out_l_cnt,
        targs->max_ios_m, targs->min_ios_m, targs->print_file_size, targs->stat_file, targs->unlink_file, targs->avg_lat, &targs->stats);
    targs->rc = rc;
}

static void AsyncWriteIoTestRunner(ThreadArgs *targs) {
    int rc = AsyncWriteIoTest(dynamic_cast<BdevCpp::AsyncApi *>(targs->api), targs->file.c_str(),
        targs->int_check, targs->l_cnt, targs->out_l_cnt, 
        targs->max_ios_m, targs->min_ios_m, targs->m_q, targs->print_file_size, targs->stat_file, targs->unlink_file, targs->poll_block_mode, targs->avg_lat, &targs->stats);
    targs->rc = rc;
}

static void AsyncWriteFdIoTestRunner(ThreadArgs *targs) {
    int rc = AsyncWriteFdIoTest(dynamic_cast<BdevCpp::AsyncApi *>(targs->api), targs->fd,
        targs->l_cnt, targs->out_l_cnt, 
        targs->max_ios_m, targs->min_ios_m, targs->m_q, targs->poll_block_mode, targs->avg_lat, &targs->stats);
    targs->rc = rc;
}

static void AsyncSyncWriteIoTestRunner(ThreadArgs *targs) {
    int rc = AsyncSyncWriteIoTest(dynamic_cast<BdevCpp::AsyncApi *>(targs->api), targs->file.c_str(),
        targs->int_check, targs->l_cnt, targs->out_l_cnt,
        targs->max_ios_m, targs->min_ios_m, targs->m_q, targs->print_file_size, targs->stat_file, targs->unlink_file, targs->poll_block_mode, targs->avg_lat, &targs->stats);
    targs->rc = rc;
}

static void AsyncAioWriteIoTestRunner(ThreadArgs *targs) {
    int rc = AsyncAioWriteIoTest(targs->file.c_str(),
        targs->int_check, targs->l_cnt, targs->out_l_cnt,
        targs->max_ios_m, targs->min_ios_m, targs->m_q, targs->avg_lat, &targs->stats);
    targs->rc = rc;
}

static void AsyncReadIoTestRunner(ThreadArgs *targs) {
    int rc = AsyncReadIoTest(dynamic_cast<BdevCpp::AsyncApi *>(targs->api), targs->file.c_str(),
        targs->int_check, targs->l_cnt, targs->out_l_cnt,
        targs->max_ios_m, targs->min_ios_m, targs->m_q, targs->print_file_size, targs->stat_file, targs->unlink_file, targs->poll_block_mode, targs->avg_lat, &targs->stats);
    targs->rc = rc;
}

static void AsyncReadFdIoTestRunner(ThreadArgs *targs) {
    int rc = AsyncReadFdIoTest(dynamic_cast<BdevCpp::AsyncApi *>(targs->api), targs->fd,
        targs->l_cnt, targs->out_l_cnt,
        targs->max_ios_m, targs->min_ios_m, targs->m_q, targs->poll_block_mode, targs->avg_lat, &targs->stats);
    targs->rc = rc;
}

static void AsyncSyncReadIoTestRunner(ThreadArgs *targs) {
    int rc = AsyncSyncReadIoTest(dynamic_cast<BdevCpp::AsyncApi *>(targs->api), targs->file.c_str(),
        targs->int_check, targs->l_cnt, targs->out_l_cnt,
        targs->max_ios_m, targs->min_ios_m, targs->m_q, targs->print_file_size, targs->stat_file, targs->unlink_file, targs->poll_block_mode, targs->avg_lat, &targs->stats);
    targs->rc = rc;
}

static void AsyncAioReadIoTestRunner(ThreadArgs *targs) {
    int rc = AsyncAioReadIoTest(targs->file.c_str(),
        targs->int_check, targs->l_cnt, targs->out_l_cnt,
        targs->max_ios_m, targs->min_ios_m, targs->m_q, targs->avg_lat, &targs->stats);
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
    bool async_fd_test = false;
    bool aio_test = false;
    int poll_block_mode = 1; // poll
    bool avg_lat = false;
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
            async_fd_test,
            print_file_size,
            stat_file,
            unlink_file,
            poll_block_mode,
            avg_lat,
            test_type,
            aio_test,
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
    if (aio_test == true)
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
                avg_lat,
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
            if (num_files == 1) {
                IoStats iostats;
                memset(&iostats, '\0', sizeof(iostats));
                rc = SyncPwriteIoTest(syncApi, sync_file.c_str(),
                    legacy_newstack_mode, integrity_check,
                    loop_count, out_loop_count,
                    max_iosize_mult,
                    min_iosize_mult,
                    print_file_size,
                    stat_file,
                    unlink_file,
                    avg_lat,
                    &iostats);
                if (rc)
                    cerr << "SyncPwriteIoTest failed rc: " << rc << endl;
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
                    string sync_f = sync_file + to_string(i);
                    thread_args[i] = new ThreadArgs{syncApi, sync_f, legacy_newstack_mode, -1, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file, poll_block_mode, avg_lat};
                    threads[i] = new thread(&SyncPwriteIoTestRunner, thread_args[i]);
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

        if (async_test == true) {
            if (num_files == 1) {
                IoStats iostats;
                memset(&iostats, '\0', sizeof(iostats));
                rc = AsyncWriteIoTest(asyncApi, async_file.c_str(),
                    integrity_check, loop_count, out_loop_count,
                    max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file, poll_block_mode, avg_lat, &iostats);
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
                    thread_args[i] = new ThreadArgs{asyncApi, async_f, -1, -1, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file, poll_block_mode, avg_lat};
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
                    max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file, poll_block_mode, avg_lat, &iostats);
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
                    thread_args[i] = new ThreadArgs{asyncApi, async_f, -1, -1, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, poll_block_mode, unlink_file, avg_lat};
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

        if (async_fd_test == true) {
           fd_pos = 0;
           int fd = asyncApi->open(async_file.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT);
           if (fd < 0) {
               cerr << "open: " << async_file << " failed errno: " << errno << endl;
               return -1;
           }

           if (num_files == 1) {
                IoStats iostats;
                memset(&iostats, '\0', sizeof(iostats));

                rc = AsyncWriteFdIoTest(asyncApi, fd,
                    loop_count, out_loop_count,
                    max_iosize_mult, min_iosize_mult, max_queued, poll_block_mode, avg_lat, &iostats);
                if (rc)
                    cerr << "AsyncWriteFdIoTest failed rc: " << rc << endl;
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
                    thread_args[i] = new ThreadArgs{asyncApi, async_f, -1, fd, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, poll_block_mode, unlink_file, avg_lat};
                    threads[i] = new thread(&AsyncWriteFdIoTestRunner, thread_args[i]);
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

           (void )asyncApi->close(fd);
        }

        if (aio_test == true) {
            if (num_files == 1) {
                IoStats iostats;
                memset(&iostats, '\0', sizeof(iostats));
                rc = AsyncAioWriteIoTest(async_file.c_str(),
                    integrity_check,
                    loop_count, out_loop_count,
                    max_iosize_mult,
                    min_iosize_mult,
                    max_queued,
                    avg_lat,
                    &iostats);
                if (rc)
                    cerr << "AsyncAioWriteIoTest failed rc: " << rc << endl;
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
                    thread_args[i] = new ThreadArgs{0, async_f, -1, -1, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued, print_file_size, false, false, -1, avg_lat};
                    threads[i] = new thread(&AsyncAioWriteIoTestRunner, thread_args[i]);
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
                avg_lat,
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
                legacy_newstack_mode, integrity_check,
                loop_count, out_loop_count,
                max_iosize_mult,
                min_iosize_mult,
                print_file_size,
                stat_file,
                unlink_file,
                avg_lat,
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
                    max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file, poll_block_mode, avg_lat, &iostats);
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
                    thread_args[i] = new ThreadArgs{asyncApi, async_f, -1, -1, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file, poll_block_mode, avg_lat};
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
                    max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file, poll_block_mode, avg_lat, &iostats);
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
                    thread_args[i] = new ThreadArgs{asyncApi, async_f, -1, -1, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file, poll_block_mode, avg_lat};
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

        if (async_fd_test == true) {
            fd_pos = 0;
            int fd = asyncApi->open(async_file.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT);
            if (fd < 0) {
                cerr << "open: " << async_file << " failed errno: " << errno << endl;
                return -1;
            }

            if (num_files == 1) {
                IoStats iostats;
                memset(&iostats, '\0', sizeof(iostats));
                rc = AsyncReadFdIoTest(asyncApi, fd,
                    loop_count, out_loop_count,
                    max_iosize_mult, min_iosize_mult, max_queued, poll_block_mode, avg_lat, &iostats);
                if (rc)
                    cerr << "AsyncReadFdIoTest failed rc: " << rc << endl;
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
                    thread_args[i] = new ThreadArgs{asyncApi, async_f, -1, fd, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued, print_file_size, stat_file, unlink_file, poll_block_mode, avg_lat};
                    threads[i] = new thread(&AsyncReadFdIoTestRunner, thread_args[i]);
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

            (void )asyncApi->close(fd);
        }


        if (aio_test == true) {
            if (num_files == 1) {
                IoStats iostats;
                memset(&iostats, '\0', sizeof(iostats));
                rc = AsyncAioReadIoTest(async_file.c_str(),
                    integrity_check,
                    loop_count, out_loop_count,
                    max_iosize_mult,
                    min_iosize_mult,
                    max_queued,
                    avg_lat,
                    &iostats);
                if (rc)
                    cerr << "AsyncAioReadIoTest failed rc: " << rc << endl;
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
                    thread_args[i] = new ThreadArgs{0, async_f, -1, -1, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued, print_file_size, false, false, -1, avg_lat};
                    threads[i] = new thread(&AsyncAioReadIoTestRunner, thread_args[i]);
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
