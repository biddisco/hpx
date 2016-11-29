//  Copyright (c) 2013-2015 Hartmut Kaiser
//  Copyright (c) 2013 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// Unidirectional network bandwidth test

#include <hpx/hpx.hpp>
#include <hpx/include/iostreams.hpp>
#include <hpx/include/serialization.hpp>
#include <hpx/include/parallel_for_each.hpp>
#include <hpx/include/util.hpp>
#include <hpx/util/detail/yield_k.hpp>

// Includes
#ifdef HPX_PARCELPORT_VERBS_HAVE_LOGGING
#  include <plugins/parcelport/verbs/rdma/rdma_logging.hpp>
#else
#  define LOG_DEBUG_MSG(x)
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

///////////////////////////////////////////////////////////////////////////////
void one_thread_sender(hpx::naming::id_type dest, std::size_t iterations, buffer_type send_buffer)
{
    LOG_DEBUG_MSG("Thread sender " << decnumber(iterations));
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

    if (size <= LARGE_MESSAGE_SIZE) {
        loop *= LOOP_SMALL_MULTIPLIER;
        skip *= LOOP_SMALL_MULTIPLIER;
    }

    // if we only have 1 thread, then we must suspend when waiting to allow
    // the network to progress, if we have >1 threads, we can use a very short delay
    // in yield_k that will not trigger a suspension
    std::size_t threads = hpx::get_os_thread_count();
    int suspend_delay = 24;
    if (threads>1) {
        suspend_delay = 4;
    }
    //LOG_DEBUG_MSG("Running with " << threads << " threads")

    typedef std::vector<char> char_buffer;
    std::vector<char_buffer> send_buffers(threads, char_buffer(size));

    for(std::size_t i=0; i<threads;++i) {
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
        hpx::util::detail::yield_k(24, nullptr);
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
    LOG_DEBUG_MSG("Here with count = " << count);
    // wait until the last message triggers the callback, so we know all msgs are sent
    while (count < iterations*threads) {
        // this makes the cpu wait briefly with without suspending our thread
        hpx::util::detail::yield_k(24, nullptr);
    }
    LOG_DEBUG_MSG("Here with count = " << count);

    double elapsed = t.elapsed();
    return (size / 1e6 * iterations*threads) / elapsed;
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

    hpx::id_type there = here;
    std::vector<hpx::id_type> localities = hpx::find_remote_localities();
    if (!localities.empty())
        there = localities[0];

    std::size_t max_size = vm["max-size"].as<std::size_t>();
    std::size_t min_size = vm["min-size"].as<std::size_t>();
    std::size_t loop = vm["loop"].as<std::size_t>();

    // perform actual measurements
    for (std::size_t size = min_size; size <= max_size; size *= 2)
    {
        if (rank==0) {
            double bw = ireceive(there, loop, size, vm["window-size"].as<std::size_t>());
            hpx::cout << std::left << std::setw(10)
                << size << bw << hpx::endl << hpx::flush;
        }
    }
}
