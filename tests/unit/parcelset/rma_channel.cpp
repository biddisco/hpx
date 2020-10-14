//  Copyright (c) 2016 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/hpx.hpp>
#include <hpx/hpx_init.hpp>
#include <hpx/include/performance_counters.hpp>
#include <hpx/program_options.hpp>
#include <hpx/modules/testing.hpp>
//
#include <hpx/runtime/basename_registration.hpp>
#include <hpx/runtime/parcelset/rma/rma_object.hpp>
//
#include "rma_communicator.hpp"
//
#include <cstddef>
#include <iostream>
#include <string>
#include <utility>
#include <type_traits>
#include <vector>
#include <array>

///////////////////////////////////////////////////////////////////////////////
using namespace hpx::parcelset::policies;
using namespace hpx::parcelset::rma;

// For this example declare some arbitrary data structure we wish to use for RMA
struct dummy_data {
    std::array<char, 16384> data;
    //
    dummy_data() {};
};

// Create some boiler-plate template code that marks the struct as RMA eligible
// this isn't needed for POD types, but since we created a struct, we need it
HPX_IS_RMA_ELIGIBLE(dummy_data);

using communication_type = dummy_data;
// normally needed in a header file?
HPX_REGISTER_CHANNEL_DECLARATION(communication_type);
// normally needed in a cpp file
HPX_REGISTER_CHANNEL(communication_type, rma_communication);

// send data to up and recev it from down
void test_rdma_1(hpx::id_type loc)
{
    std::size_t rank = hpx::get_locality_id();
    std::size_t size = hpx::get_num_localities(hpx::launch::sync);

    // this rank create a communicator using a dummy data struct
    using comm_type = communicator<communication_type>;
    comm_type comm(rank, size);

    communication_type test_data;
    const int iterations = 256;
    for (int i=0; i<iterations; i++) {
        for (int n=0; n<1; ++n) {
            comm.send[comm_type::neighbour::left].set(test_data, 0);
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
int hpx_main(hpx::program_options::variables_map& vm)
{
    unsigned int seed = (unsigned int)std::time(nullptr);
    if (vm.count("seed"))
        seed = vm["seed"].as<unsigned int>();

    std::cout << "using seed: " << seed << std::endl;
    std::srand(seed);

    for (hpx::id_type const& id : hpx::find_remote_localities())
    {
        test_rdma_1(id);
    }

    // compare number of parcels with number of messages generated
    //print_counters("/parcels/count/*/sent");
    //print_counters("/messages/count/*/sent");

    return hpx::finalize();
}

///////////////////////////////////////////////////////////////////////////////
int main(int argc, char* argv[])
{
    // add command line option which controls the random number generator seed
    using namespace hpx::program_options;
    options_description desc_commandline(
        "Usage: " HPX_APPLICATION_STRING " [options]");

    desc_commandline.add_options()
        ("seed,s", value<unsigned int>(),
        "the random number generator seed to use for this run")
        ;

    // explicitly disable message handlers (parcel coalescing)
    std::vector<std::string> const cfg = {
        "hpx.parcel.message_handlers=0"
    };

    // Initialize and run HPX
    HPX_TEST_EQ_MSG(hpx::init(desc_commandline, argc, argv, cfg), 0,
        "HPX main exited with non-zero status");

    return hpx::util::report_errors();
}
