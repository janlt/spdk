#include <errno.h>

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
#include "ApiBase.h"
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

static int SyncIoTest(BdevCpp::SyncApi *api, const char *file_name) {
    int rc = 0;
    char buf[25000];
    char cmp_buf[25000];

    int fd = api->open(file_name, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        cerr << "open: " << file_name << " failed errno: " << errno << endl;
        return -1;
    }

    for (int i = 0 ; i < 10 ; i++) {
        size_t io_size = 512*(2*i + 1);
        ::memset(buf, 'a' + i, io_size);
        rc = api->write(fd, buf, io_size);
        if (rc < 0) {
            cerr << "write sync failed rc: " << rc << " errno: " << errno << endl;
            break;
        }
    }

    rc = api->lseek(fd, 0, SEEK_SET);
    if (rc < 0) {
        cerr << "week sync failed rc: " << rc << " errno: " << errno << endl;
        return rc;
    }

    for (int i = 0 ; i < 10 ; i++) {
        size_t io_size = 512*(2*i + 1);
        ::memset(cmp_buf, 'a' + i, io_size);
        rc = api->read(fd, buf, io_size);
        if (rc < 0) {
            cerr << "read sync failed rc: " << rc << " errno: " << errno << endl;
            rc = -1;
            break;
        }
        if (memcmp(buf, cmp_buf, io_size)) {
            cerr << "Corrupted data after read i: " << i << " io_size: " << io_size << endl;
            rc = -1;
            break;
        }
    }

    return rc;
}

static int AsyncIoTest(BdevCpp::AsyncApi *aApi, const char *file_name) {
    return 0;
}

int main(int argc, char **argv) {
    int rc = 0;
    BdevCpp::Options options;
    BdevCpp::Options spdk_options;

    if (boost::filesystem::exists(spdk_conf)) {
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

    BdevCpp::Api api(options);
    BdevCpp::SyncApi *syncApi = api.getSyncApi();
    BdevCpp::AsyncApi *asyncApi = api.getAsyncApi();

    const char *sync_file_name = "testsync";
    if (argc > 1)
        sync_file_name = argv[1];

    rc = SyncOpenCloseTest(syncApi, sync_file_name);

    const char *async_file_name = "testasync";
    if (argc > 2)
        async_file_name = argv[2];

    rc = AsyncOpenCloseTest(asyncApi, async_file_name);

    if (rc < 0)
        return rc;

    rc = SyncIoTest(syncApi, sync_file_name);
    cout << "SyncIoTest rc: " << rc << endl;

    rc = AsyncIoTest(asyncApi, async_file_name);
    cout << "AsyncIoTest rc: " << rc << endl;

    api.QuiesceIO(true);
    return rc;
}
