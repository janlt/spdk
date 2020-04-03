#include <iostream>
#include <memory>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "config/Config.h"
#include "config/Options.h"

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
    }

    return 0;
}
