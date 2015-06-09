//  Copyright (c) 2007-2012 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/hpx_init.hpp>
#include <hpx/include/iostreams.hpp>

// This application will just sit and wait for being terminated from the
// console window for the specified amount of time. This is useful for testing
// the heartbeat tool which connects and disconnects to a running application.
int hpx_main(boost::program_options::variables_map& vm)
{
    double const runfor = vm["runfor"].as<double>();

    hpx::cout << "Heartbeat Console, waiting for";
    if (runfor > 0)
        hpx::cout << " " << runfor << "[s].\n" << hpx::flush;
    else
        hpx::cout << "ever.\n" << hpx::flush;

                   
    int port = boost::lexical_cast<std::size_t>(hpx::get_config_entry("hpx.parcel.port", 0));
    int agas = boost::lexical_cast<std::size_t>(hpx::get_config_entry("hpx.agas.port", 0));
    hpx::cout << "Using port number " << port << " and agas " << agas << std::endl;

    uint64_t nranks = hpx::get_num_localities().get();

    hpx::util::high_resolution_timer t;
    bool terminate = false;
    while (!terminate && (runfor <= 0 || t.elapsed() < runfor))
    {
        hpx::this_thread::suspend(boost::chrono::milliseconds(1000));
        uint64_t nranks2 = hpx::get_num_localities().get();
        hpx::cout << "." << nranks2 <<  hpx::flush;
        std::cout << std::flush;
        if (nranks2!=nranks) {
            hpx::cout << "\nRanks changed from " << nranks << " to " << nranks2 << std::endl << std::flush;
            if (nranks2==1 && nranks==2) {
              // last client disconnected, so just exit
              // terminate = true;
            }
            nranks=nranks2;
        }
    }

    hpx::cout <<"\nSleeping before shutdown" << std::endl;
    hpx::this_thread::sleep_for(boost::chrono::seconds(1));
    hpx::cout << "\n" << hpx::flush;
    return hpx::finalize();
}

int main(int argc, char* argv[])
{
    // Configure application-specific options.
    boost::program_options::options_description
       desc_commandline("Usage: " HPX_APPLICATION_STRING " [options]");

    using boost::program_options::value;
    desc_commandline.add_options()
        ( "runfor", value<double>()->default_value(600.0),
          "time to wait before this application exits ([s], default: 600)")
        ;

    return hpx::init(desc_commandline, argc, argv);
}
