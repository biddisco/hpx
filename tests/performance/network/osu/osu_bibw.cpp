//  Copyright (c) 2013-2015 Hartmut Kaiser
//  Copyright (c) 2013 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// Bidirectional network bandwidth test

#include <hpx/hpx.hpp>
#include <hpx/include/iostreams.hpp>
#include <hpx/include/serialization.hpp>
#include <hpx/include/parallel_for_each.hpp>
#include <hpx/include/util.hpp>
#include <hpx/util/detail/yield_k.hpp>

// Includes
#ifndef HPX_PARCELPORT_VERBS_HAVE_LOGGING
#  include <plugins/parcelport/verbs/rdma/rdma_logging.hpp>
#else
#  define LOG_DEBUG_MSG(x)
#  define LOG_DEVEL_MSG(x)
#endif
//
#include <boost/range/irange.hpp>
//
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

///////////////////////////////////////////////////////////////////////////////
#define LOOP_SMALL_MULTIPLIER 5
#define SKIP                  20
#define LARGE_MESSAGE_SIZE    8192
#define MAX_MSG_SIZE         (1<<22)

///////////////////////////////////////////////////////////////////////////////
typedef hpx::serialization::serialize_buffer<char> buffer_type;
// used to track messages that have been 'sent'
std::atomic<std::size_t> count;
std::shared_ptr<hpx::promise<double>> remote_time;

///////////////////////////////////////////////////////////////////////////////
void isend(hpx::serialization::serialize_buffer<char> const& receive_buffer) {}
HPX_PLAIN_DIRECT_ACTION(isend);

///////////////////////////////////////////////////////////////////////////////
void async_callback(std::atomic<std::size_t> &count, boost::system::error_code
    const& ec, hpx::parcelset::parcel const& p)
{
    // this global atomic tracks how many messages have 'been sent'
    count++;
}

    using hpx::parallel::for_each;
    using hpx::parallel::par;
    using hpx::parallel::task;

void set_remote_time(double t)
{
    remote_time->set_value(t);
}
HPX_PLAIN_DIRECT_ACTION(set_remote_time);

///////////////////////////////////////////////////////////////////////////////
void one_thread_sender(hpx::naming::id_type dest, std::size_t iterations, buffer_type send_buffer)
{
    //LOG_DEVEL_MSG("Thread sender " << decnumber(iterations));
    using hpx::util::placeholders::_1;
    using hpx::util::placeholders::_2;
    //
    isend_action send;
    for (std::size_t i=0; i<iterations; ++i) {
        hpx::apply_cb(send, dest,
            hpx::util::bind(&async_callback,
                std::ref(count), _1, _2),
            send_buffer);
    }
    //LOG_DEBUG_MSG("Done Thread sender " << decnumber(iterations));
}

///////////////////////////////////////////////////////////////////////////////
double ireceive(hpx::naming::id_type dest, std::size_t loop,
                std::size_t size, std::size_t window_size)
{
    std::size_t skip = SKIP;

    using hpx::parallel::for_each;
    using hpx::parallel::par;
    using hpx::parallel::task;

    std::size_t threads = hpx::get_os_thread_count();
    LOG_DEVEL_MSG("Running with " << threads << " threads")

    typedef std::vector<char> char_buffer;
    std::vector<char_buffer> send_buffers(threads, char_buffer(size));

    for (std::size_t i=0; i<threads;++i) {
        std::memset(send_buffers[i].data(), 'a', size);
    }

    using namespace hpx::parallel;

    // create one task per OS thread and launch messages from those worker tasks
    count = 0;
    std::size_t zero = 0;
    std::size_t iterations =  loop/threads;
    auto range = boost::irange(zero, threads);
    for_each(par.with(static_chunk_size(1)), boost::begin(range), boost::end(range),
        [&](std::size_t j)
        {
            one_thread_sender(dest, skip,
                buffer_type(send_buffers[j].data(), size, buffer_type::reference));
        }
    );
    // wait until the last message triggers the callback, so we know all msgs are sent
    while (count < skip*threads) {
        // this makes the cpu wait with without suspending our thread
        hpx::util::detail::yield_k(4, nullptr);
    }

    count = 0;
    hpx::util::high_resolution_timer t;
    for_each(par.with(hpx::parallel::static_chunk_size(1)), boost::begin(range), boost::end(range),
        [&](std::size_t j)
        {
            one_thread_sender(dest, iterations,
                buffer_type(send_buffers[j].data(), size, buffer_type::reference));
        }
    );
    //LOG_DEVEL_MSG("Here with count = " << count);
    // wait until the last message triggers the callback, so we know all msgs are sent
    while (count < iterations*threads) {
        // this makes the cpu wait briefly with without suspending our thread
        hpx::util::detail::yield_k(4, nullptr);
    }
    //LOG_DEVEL_MSG("Here with count = " << count);

    // test is complete. Get the time
    double elapsed = t.elapsed();

    // send our time to remote node
    LOG_DEVEL_MSG("sending our time to remote node " << elapsed);
    set_remote_time_action setter;
    hpx::apply(setter, dest, elapsed);

    // in bidirectional test, we need the timing from the remote node
    LOG_DEVEL_MSG("Getting remote time from future");
    auto remote_elapsed = get_remote_time().get();
    LOG_DEVEL_MSG("Happy");
    double actual = std::max(elapsed, remote_elapsed);

    return (2 * size / 1e6 * iterations*threads) / actual;
}

///////////////////////////////////////////////////////////////////////////////
void print_header()
{
    hpx::cout << "# OSU HPX Bandwidth Test\n"
              << "# Size    Bandwidth (MB/s)"
              << std::endl;
}

void run_benchmark(boost::program_options::variables_map & vm)
{
    // use the first remote locality to bounce messages, if possible
    hpx::id_type here = hpx::find_here();
    uint64_t     rank = hpx::naming::get_locality_id_from_id(here);
    std::cout <<"run_benchmark on rank " << rank << std::endl;

    hpx::id_type there = here;
    std::vector<hpx::id_type> localities = hpx::find_remote_localities();
    if (!localities.empty())
        there = localities[0];

    std::size_t max_size = vm["max-size"].as<std::size_t>();
    std::size_t min_size = vm["min-size"].as<std::size_t>();
    std::size_t loop = vm["loop"].as<std::size_t>();


    // perform actual measurements
    std::cout <<"Main ireceive loop on rank " << rank << std::endl;
    for (std::size_t size = min_size; size <= max_size; size *= 2)
    {
        LOG_DEVEL_MSG("SETTING PROMISE on rank " << rank);
        remote_time = std::make_shared<hpx::promise<double>>();
        LOG_DEVEL_MSG("Done SETTING PROMISE");
            double bw = ireceive(there, loop, size, vm["window-size"].as<std::size_t>());
            hpx::cout << std::left << std::setw(10)
                << size << bw << hpx::endl << hpx::flush;
    }
}
