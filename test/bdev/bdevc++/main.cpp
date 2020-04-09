#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include <iostream>
#include <memory>
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

static void printTimeNow(const char *msg) {
    char time_buf[128];
    time_t now = time(0);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S.000", localtime(&now));
    cout << msg << " " << time_buf << endl;
}

static char io_buf[2500000];
static char io_cmp_buf[2500000];
static int SyncIoTest(BdevCpp::SyncApi *api,
        const char *file_name, int mode, int check,
        int loop_count, int out_loop_count) {
    int rc = 0;

    int fd = !mode ? api->open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT) : 
        ::open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    printTimeNow("Start sync test ");

    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
    uint64_t write_ios = 0;
    uint64_t read_ios = 0;

    for (int j = 0 ; j < out_loop_count ; j++) {
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = 512*(2*(i%20) + 1);
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

        if (check) {
            rc = !mode ? api->lseek(fd, 0, SEEK_SET) : ::lseek(fd, 0, SEEK_SET);
            if (rc < 0) {
                cerr << "lseek failed rc: " << rc << " errno: " << errno << endl;
                return rc;
            }

            for (int i = 0 ; i < loop_count ; i++) {
                size_t io_size = 512*(2*(i%20) + 1);
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

    printTimeNow("End sync test ");
    cout << "bytes_written " << bytes_written << " bytes_read " << bytes_read <<
            " write_ios " << write_ios << " read_ios " << read_ios << endl;

    rc = !mode ? api->close(fd) : ::close(fd);

    return rc;
}

static int AsyncIoTest(BdevCpp::AsyncApi *api,
        const char *file_name, int check,
        int loop_count, int out_loop_count) {
    int rc = 0;
    char buf[25000];
    char cmp_buf[25000];

    int fd = api->open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR | O_SYNC | O_DIRECT);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    const size_t maxWriteFutures = 2014;
    BdevCpp::FutureBase *write_futures[maxWriteFutures];
    size_t num_write_futures = 0;
    const size_t maxReadFutures = 2014;
    BdevCpp::FutureBase *read_futures[maxReadFutures];
    size_t num_read_futures = 0;

    printTimeNow("Start async test ");

    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
    uint64_t write_ios = 0;
    uint64_t read_ios = 0;
    uint64_t pos = 0;
    uint64_t check_pos = pos;

    bool get_out = false;
    for (int j = 0 ; j < out_loop_count ; j++) {
        check_pos = pos;
        for (int i = 0 ; i < loop_count ; i++) {
            size_t io_size = 512*(2*(i%20) + 1);
            ::memset(buf, 'a' + i%20, io_size);
            BdevCpp::FutureBase *wfut = api->write(fd, pos, buf, io_size);
            pos += io_size;
            if (!wfut || num_write_futures >= maxWriteFutures) {
                for ( ; num_write_futures > 0 ; num_write_futures--) {
                    char *data;
                    size_t dataSize;
                    int w_rc = dynamic_cast<BdevCpp::WriteFuture *>(write_futures[num_write_futures - 1])->get(data, dataSize);
                    if (w_rc < 0) {
                        cerr << "WriteFuture get failed rc: " << w_rc << endl;
                        get_out = true;
                        break;
                    }
                }
            }
            if (get_out == true)
                break;
            if (!wfut) {
                cerr << "write async failed rc: " << rc << " errno: " << errno << endl;
                break;
            }
            write_futures[num_write_futures] = wfut;

            write_ios++;
            bytes_written += io_size;
        }

        if (check) {
            for (int i = 0 ; i < loop_count ; i++) {
                size_t io_size = 512*(2*(i%20) + 1);
                ::memset(cmp_buf, 'a' + i%20, io_size);

                BdevCpp::FutureBase *rfut = api->read(fd, check_pos, buf, io_size);
                check_pos += io_size;
                if (!rfut || num_read_futures >= maxReadFutures) {
                    for ( ; num_read_futures > 0 ; num_read_futures--) {
                        char *data;
                        size_t dataSize;
                        int r_rc = dynamic_cast<BdevCpp::ReadFuture *>(read_futures[num_read_futures - 1])->get(data, dataSize);
                        if (r_rc < 0) {
                            cerr << "ReadFuture get failed rc: " << r_rc << endl;
                            get_out = true;
                            break;
                        }
                    }
                }
                if (!rfut) {
                    cerr << "read sync failed rc: " << rc << " errno: " << errno << endl;
                    rc = -1;
                    break;
                }
                read_futures[num_read_futures++] = rfut;

                read_ios++;
                bytes_read += io_size;
                if (memcmp(buf, cmp_buf, io_size)) {
                    cerr << "Corrupted data after read i: " << i << " io_size: " << io_size << endl;
                    rc = -1;
                    break;
                }
            }
        }
    }

    printTimeNow("End test ");
    cout << "bytes_written " << bytes_written << " bytes_read " << bytes_read <<
            " write_ios " << write_ios << " read_ios " << read_ios << endl;

    rc = api->close(fd);

    return rc;
}

static void usage(const char *prog)
{
    cout <<
        "Usage %s [options]\n"
        "  -c, --spdk-conf                SPDK config file\n"
        "  -s, --sync-file                Sync file name\n"
        "  -a, --async-file               Async file name\n"
        "  -m, --loop-count               Inner IO loop count\n"
        "  -n, --outer-loop-count         Outer IO loop count\n"
        "  -l, --legacy-newstack-mode     Use legacy (0) or new stack (1) file IO routines\n"
        "  -i, --integrity-check          Run integrity check\n"
        "  -O, --open-close-test          Run open/clsoe test\n"
        "  -S, --sync-test                Run IO sync test\n"
        "  -A, --async-test               Run IO async test\n"
        "  -d, --debug                    Debug on\n"
        "  -h, --help                     Print this help\n" << prog << endl;
}

static int mainGetopt(int argc, char *argv[],
        string &spdk_conf,
        string &sync_file,
        string &async_file,
        int &loop_count,
        int &out_loop_count,
        int &legacy_newstack_mode,
        bool &integrity_check,
        bool &open_close_test,
        bool &sync_test,
        bool &async_test,
        bool &debug)
{
    int c = 0;
    int ret = -1;

    while (1) {
        static char short_options[] = "c:s:a:m:n:l:i:dhOSA";
        static struct option long_options[] = {
            {"spdk-conf",               1, 0, 'c'},
            {"sync-file",               1, 0, 's'},
            {"async-file",              1, 0, 'a'},
            {"loop-count",              1, 0, 'm'},
            {"outer-loop-count",        1, 0, 'n'},
            {"legacy-newstack-mode",    1, 0, 'l'},
            {"integrity-check",         1, 0, 'i'},
            {"open-close-test",         0, 0, 'O'},
            {"sync-test",               0, 0, 'S'},
            {"async-test",              0, 0, 'A'},
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

        case 'l':
            legacy_newstack_mode = atoi(optarg);
            break;

        case 'i':
            integrity_check = atoi(optarg);
            break;

        case 'O':
            open_close_test = true;;
            break;

        case 'S':
            sync_test = true;;
            break;

        case 'A':
            async_test = true;;
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

int main(int argc, char **argv) {
    int rc = 0;
    BdevCpp::Options options;
    BdevCpp::Options spdk_options;
    bool debug = false;
    string spdk_conf_str(spdk_conf);
    int loop_count = 10;
    int out_loop_count = 10;
    const char *sync_file_name = "testsync";
    string sync_file(sync_file_name);
    const char *async_file_name = "testasync";
    string async_file(async_file_name);
    int legacy_newstack_mode = 0;
    bool integrity_check = false;

    bool open_close_test = false;
    bool sync_test = false;
    bool async_test = false;

    int ret = mainGetopt(argc, argv,
            spdk_conf_str,
            sync_file,
            async_file,
            loop_count,
            out_loop_count,
            legacy_newstack_mode,
            integrity_check,
            open_close_test,
            sync_test,
            async_test,
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

    if (sync_test == true) {
        rc = SyncIoTest(syncApi, sync_file.c_str(), legacy_newstack_mode, integrity_check, loop_count, out_loop_count);
        if (rc)
            cerr << "SyncIoTest failed rc: " << rc << endl;
    }

    if (async_test == true) {
        rc = AsyncIoTest(asyncApi, async_file.c_str(), integrity_check, loop_count, out_loop_count);
        if (rc)
            cerr << "AsyncIoTest failed rc: " << rc << endl;
    }

    api.QuiesceIO(true);
    return rc;
}
