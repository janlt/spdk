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

using namespace std;
namespace po = boost::program_options;

const char *spdk_conf = "config.spdk";

int main(int argc, char **argv) {
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

    BdevCpp::SpdkCore *spc = new BdevCpp::SpdkCore(options.io);
    if ( spc->isBdevFound() == true ) {
        cout << "Io functionality is enabled" << endl;
    } else {
        cerr << "Io functionality is disabled" << endl;
        return -1;
    }

    if (spc->isBdevFound() == false) {
        cerr << "Bdev not found" << endl;
        return -1;
    }

    BdevCpp::IoPoller *spio = new BdevCpp::IoPoller(spc);

    spc->setPoller(spio);
    if (spc->isSpdkReady() == true) {
        spc->startSpdk();
        spc->waitReady(); // synchronize until SpdkCore is done initializing SPDK framework
    }

    return 0;
}
