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

static char io_buf[2500000];
static char io_cmp_buf[2500000];

static int SyncWriteIoTest(BdevCpp::SyncApi *api,
        const char *file_name, int mode, int check,
        int loop_count, int out_loop_count, 
        size_t max_iosize_mult, size_t min_iosize_mult) {
    int rc = 0;

    int fd = !mode ? api->open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT) : 
        ::open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    time_t stime = printTimeNow("Start sync test ");

    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
    uint64_t write_ios = 0;
    uint64_t read_ios = 0;

    for (int j = 0 ; j < out_loop_count ; j++) {
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(4096, i, min_iosize_mult, max_iosize_mult);
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

            write_ios++;
            bytes_written += io_size;
        }

        if (!rc && check) {
            rc = !mode ? api->lseek(fd, 0, SEEK_SET) : ::lseek(fd, 0, SEEK_SET);
            if (rc < 0) {
                cerr << "lseek failed rc: " << rc << " errno: " << errno << endl;
                return rc;
            }

            for (int i = 0 ; i < loop_count ; i++) {
                size_t io_size = calcIoSize(4096, i, min_iosize_mult, max_iosize_mult);
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

            rc = !mode ? api->lseek(fd, 0, SEEK_SET) : ::lseek(fd, 0, SEEK_SET);
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
    double write_mbsec = static_cast<double>(bytes_written)/t_diff;
    write_mbsec /= (1024 * 1024);
    double read_mbsec = static_cast<double>(bytes_read)/t_diff;
    read_mbsec /= (1024 * 1024);
    double write_iops = static_cast<double>(write_ios)/t_diff;
    double read_iops = static_cast<double>(read_ios)/t_diff;

    if (!rc)
        cout << "bytes_written: " << bytes_written << " bytes_read: " << bytes_read <<
            " write_ios: " << write_ios << " read_ios: " << read_ios << 
            " write MiB/sec: " << static_cast<float>(write_mbsec) << " read MiB/sec: " << static_cast<float>(read_mbsec) <<
            " write IOPS: " << static_cast<float>(write_iops) << " read IOPS: " << static_cast<float>(read_iops) << endl;
    else
        cerr << "Sync test failed rc " << rc << endl;

    rc = !mode ? api->close(fd) : ::close(fd);

    return rc;
}

//
// Async IO test
//
const size_t maxWriteFutures = 64;
const size_t maxReadFutures = 64;

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

        if (::memcmp(io_buffers[i], io_cmp_buffers[i], io_sizes[i])) {
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
        size_t max_iosize_mult, size_t min_iosize_mult, size_t max_queued) {
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
        io_buffers[i] = new char[250000];
    for (size_t i = 0 ; i < maxReadFutures ; i++)
        io_cmp_buffers[i] = new char[250000];

    time_t stime = printTimeNow("Start async test ");

    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
    uint64_t write_ios = 0;
    uint64_t read_ios = 0;
    uint64_t pos = 0;
    uint64_t check_pos = pos;

    for (int j = 0 ; j < out_loop_count ; j++) {
        check_pos = pos;
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = calcIoSize(4096, i, min_iosize_mult, max_iosize_mult);
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
                size_t io_size = calcIoSize(4096, i, min_iosize_mult, max_iosize_mult);
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
    double write_mbsec = static_cast<double>(bytes_written)/t_diff;
    write_mbsec /= (1024 * 1024);
    double read_mbsec = static_cast<double>(bytes_read)/t_diff;
    read_mbsec /= (1024 * 1024);
    double write_iops = static_cast<double>(write_ios)/t_diff;
    double read_iops = static_cast<double>(read_ios)/t_diff;
    if (!rc)
        cout << "bytes_written: " << bytes_written << " bytes_read: " << bytes_read <<
            " write_ios: " << write_ios << " read_ios: " << read_ios << 
            " write MiB/sec: " << static_cast<float>(write_mbsec) << " read MiB/sec: " << static_cast<float>(read_mbsec) <<
            " write IOPS: " << static_cast<float>(write_iops) << " read IOPS: " << static_cast<float>(read_iops) << endl;
    else
        cerr << "Test failed rc " << rc << endl;

    //rc = api->close(fd);

    for (size_t i = 0 ; i < maxWriteFutures ; i++)
        delete [] io_buffers[i];
    for (size_t i = 0 ; i < maxReadFutures ; i++)
        delete [] io_cmp_buffers[i];

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
        "  -q, --max-queued               Max queued async IOs (10 - default)\n"
        "  -z, --max-iosize-mult          Max IO size 4096 multiplier (8 - default)\n"
        "  -w, --min-iosize-mult          Min IO size 4096 multiplier (1 - default)\n"
        "  -t, --num-files                Number of files to write/read concurrently\n"
        "  -l, --legacy-newstack-mode     Use legacy (1) or new stack (0 - default) file IO routines\n"
        "  -i, --integrity-check          Run integrity check (false - default)\n"
        "  -O, --open-close-test          Run open/clsoe test\n"
        "  -S, --sync-test                Run IO sync test\n"
        "  -A, --async-test               Run IO async test\n"
        "  -R, --read-test                Run read test\n"
        "  -W, --write-test               Run write test (default)\n"
        "  -d, --debug                    Debug on (false - default)\n"
        "  -h, --help                     Print this help\n" << prog << endl;
}

typedef enum _eTestType {
    eWriteTest = 0,
    eReadTest  = 1
} eTestType;

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
        bool &async_test,
        eTestType &write_read,
        bool &debug)
{
    int c = 0;
    int ret = -1;

    while (1) {
        static char short_options[] = "c:s:a:m:n:l:i:z:t:w:q:dhOSARW";
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
            {"async-test",              0, 0, 'A'},
            {"read-test",               0, 0, 'R'},
            {"write-test",              0, 0, 'W'},
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

        case 'A':
            async_test = true;
            break;

        case 'W':
            write_read = eWriteTest;
            break;

        case 'R':
            write_read = eReadTest;
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
    int rc;
};

static void AsyncIoTestRunner(ThreadArgs *targs) {
    int rc = AsyncIoTest(targs->api, targs->file.c_str(), 
        targs->int_check, targs->l_cnt, targs->out_l_cnt, 
        targs->max_ios_m, targs->min_ios_m, targs->m_q);
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
    size_t max_iosize_mult = 8;
    size_t min_iosize_mult = 1;
    size_t num_files = 1;
    const char *sync_file_name = "testsync";
    string sync_file(sync_file_name);
    const char *async_file_name = "testasync";
    string async_file(async_file_name);
    int legacy_newstack_mode = 0;
    bool integrity_check = false;
    eTestType write_read = eWriteTest;

    bool open_close_test = false;
    bool sync_test = false;
    bool async_test = false;

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
            async_test,
            write_read,
            debug);
    if (ret) {
        usage(argv[0]);
        return ret;
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

    if (max_iosize_mult > 8)
        max_iosize_mult = 8;

    if (min_iosize_mult < 1)
        min_iosize_mult = 1;

    if (max_queued > maxWriteFutures)
        max_queued = maxWriteFutures;

    if (num_files > 64)
        num_files = 64;

    if (write_read == eWriteTest) {
        if (sync_test == true) {
            rc = SyncWriteIoTest(syncApi, sync_file.c_str(),
                legacy_newstack_mode, integrity_check,
                loop_count, out_loop_count,
                max_iosize_mult,
                min_iosize_mult);
            if (rc)
                cerr << "SyncWriteIoTest failed rc: " << rc << endl;
        }

        if (async_test == true) {
            if (num_files == 1) {
                rc = AsyncWriteIoTest(asyncApi, async_file.c_str(),
                    integrity_check, loop_count, out_loop_count,
                    max_iosize_mult, min_iosize_mult, max_queued);
                if (rc)
                    cerr << "AsyncWriteIoTest failed rc: " << rc << endl;
            } else {
                thread *threads[64];
                for (size_t i = 0 ; i < num_files ; i++) {
                    string async_f = async_file + to_string(i);
                    ThreadArgs *targs = new ThreadArgs{asyncApi, async_f, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued};
                    threads[i] = new thread(&AsyncWriteIoTestRunner, targs);
                }

                for (size_t i = 0 ; i < num_files ; i++) {
                    threads[i]->join();
                }
            }
        }
    } else {
        if (async_test == true) {
            if (num_files == 1) {
                rc = AsyncReadIoTest(asyncApi, async_file.c_str(),
                    integrity_check, loop_count, out_loop_count,
                    max_iosize_mult, min_iosize_mult, max_queued);
                if (rc)
                    cerr << "AsyncReadIoTest failed rc: " << rc << endl;
            } else {
                thread *threads[64];
                for (size_t i = 0 ; i < num_files ; i++) {
                    string async_f = async_file + to_string(i);
                    ThreadArgs *targs = new ThreadArgs{asyncApi, async_f, integrity_check,
                        loop_count, out_loop_count,
                        max_iosize_mult, min_iosize_mult, max_queued};
                    threads[i] = new thread(&AsyncReadIoTestRunner, targs);
                }

                for (size_t i = 0 ; i < num_files ; i++) {
                    threads[i]->join();
                }
            }
        }
    }

    api.QuiesceIO(true);
    return rc;
}
