// HPX includes (before boost)
#include <hpx/hpx_init.hpp>
#include <hpx/hpx.hpp>
#include <hpx/include/iostreams.hpp>
// #include <hpx/util/asio_util.hpp>
// #include <hpx/lcos/local/dataflow.hpp>
// #include <hpx/lcos/local/mutex.hpp>
// #include <hpx/lcos/local/shared_mutex.hpp>
// //
// #include <hpx/runtime_impl.hpp>
#include <hpx/runtime/threads/detail/thread_pool.hpp>
#include <hpx/runtime/threads/detail/create_thread.hpp>
#include <hpx/runtime/threads/detail/create_work.hpp>
#include <hpx/runtime/threads/policies/callback_notifier.hpp>
#include <hpx/runtime/threads/policies/static_queue_scheduler.hpp>
//
// Boost includes
#include <boost/program_options.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/thread/locks.hpp>

// STL includes
#include <string>
#include <iostream>
#include <chrono>
#include <vector>
#include <map>
#include <algorithm>

//
//#undef RDMAHELPER_DISABLE_LOGGING
//#include "RdmaLogging.h"
//
using namespace hpx;
using namespace hpx::threads::policies;
//
typedef static_queue_scheduler<> scheduling_policy_type;
//
//template<>
//void hpx::threads::detail::scheduling_loop<scheduling_policy_type>(
//  std::size_t num_thread, scheduling_policy_type& scheduler,
//  boost::atomic<hpx::state>& global_state, scheduling_counters& counters,
//  util::function_nonser<void()> const& cb_outer,
//  util::function_nonser<void()> const& cb_inner)
//{
//  util::itt::stack_context ctx;        // helper for itt support
//  util::itt::domain domain(get_thread_name().data());
//  //         util::itt::id threadid(domain, this);
//  util::itt::frame_context fctx(domain);
//}


//----------------------------------------------------------------------------
class custom_scheduler {
private:
  typedef boost::mutex mutex_type;

public:
  //    typedef static_queue_scheduler<
  //                boost::mutex, lockfree_fifo, lockfree_fifo, lockfree_lifo>
  //                scheduling_policy_type;
  typedef static_queue_scheduler<> scheduling_policy_type;

  // mutex protecting the members
  mutable mutex_type mtx_;

  // some params we need to initialize the scheduler
  scheduling_policy_type::init_parameter init_param_;

  // The scheduler
  scheduling_policy_type scheduler_;

  // Callback notifier (?)
  callback_notifier notifier_;

  // The thread pool we run the scheduler on
  hpx::threads::detail::thread_pool<scheduling_policy_type> pool_;

  // avoid warnings about usage of this in member initializer list
  custom_scheduler* This() { return this; }

  //   static void init_tss(std::size_t num, char const* name)
  //   {
  //     hpx::runtime *runtime = hpx::get_runtime_ptr();
  //     runtime->register_thread(name, num, false);
  //   }

  //----------------------------------------------------------------------------
  custom_scheduler() :
    init_param_(),
    scheduler_(init_param_, false), // false = not deferred initialization
    notifier_(get_runtime().get_notification_policy("custom-pool")),
    pool_(scheduler_, notifier_, "custom-pool")
  {
    //        LOG_DEBUG_MSG("Creating custom scheduler");
    std::cout << "Creating custom scheduler" << std::endl;;
  }



  // a simple function that runs our hpx thread/function
  static inline threads::thread_state_enum thread_function_nullary(
    util::unique_function_nonser<void()> func)
  {
    // execute the actual thread function
    func();

    // Verify that there are no more registered locks for this
    // OS-thread. This will throw if there are still any locks
    // held.
    util::force_error_on_lock();

    return threads::terminated;
  }

  void init() {
    boost::unique_lock<mutex_type> lk(mtx_);

    init_affinity_data affinity_data;
    pool_.init(1, affinity_data);
    pool_.run(lk, 1);
  }

  void stop()
  {
    boost::unique_lock<mutex_type> lk(mtx_);
    pool_.stop(lk, true);
  }

  hpx::threads::thread_id_type register_thread_nullary(
    util::unique_function_nonser<void()> && func, char const* desc,
    threads::thread_state_enum initial_state, bool run_now,
    threads::thread_priority priority, std::size_t os_thread,
    threads::thread_stacksize stacksize, error_code& ec)
  {
    //        std::cout << "Here 1 in register_thread_nullary" << std::endl;
    //        LOG_DEBUG_MSG("Here 1 in register_thread_nullary");
    //        std::cout << "Here 2 in register_thread_nullary" << std::endl;
    //        LOG_DEBUG_MSG("Here 2 in register_thread_nullary");


    hpx::threads::thread_init_data data(
      util::bind(util::one_shot(&thread_function_nullary), std::move(func)),
      desc ? desc : "<unknown>", 0, priority, os_thread,
      threads::get_stack_size(stacksize));

    //        std::cout << "Here 4 in register_thread_nullary using os thread num " << data.num_os_thread << std::endl;
    //        LOG_DEBUG_MSG("Here 4 in register_thread_nullary");


    hpx::threads::thread_id_type id = threads::invalid_thread_id;
    //        hpx::threads::detail::create_work(&scheduler_, data, initial_state, ec);

    hpx::threads::detail::create_thread(&scheduler_, data, id, initial_state, run_now, ec);

    //        std::cout << "created a thread and got id " << id << std::endl;
    return id;
  }

};




//----------------------------------------------------------------------------
int demo(double runfor, int pause)
{
  // timing
  boost::int64_t zero_time = 0;
  hpx::util::high_resolution_timer t;

  if (runfor<0 || t.elapsed()<runfor)
  {
    // try to detect when hpx is shutting down
    hpx::state run_state;
    hpx::runtime* rt = hpx::get_runtime_ptr();
    if (NULL == rt) {
      // we're probably either starting or stopping
      run_state = hpx::state_running;
    }
    else run_state = (rt->get_thread_manager().status());
    std::cout << "demo : hpx runtime state is " << (int)run_state << std::endl;

    // stop collecting data when the runtime is exiting
    // @TODO find out why quit does not work.

    hpx::threads::executors::generic_thread_pool_executor exec(
        hpx::this_thread::get_executor());

    if (exec.get_state() == hpx::state_running)
    {
        exec.add_after(
            boost::chrono::milliseconds(pause),
            hpx::util::bind(&demo, (std::max)(runfor-t.elapsed(), 0.0), pause),
            "demo");
    }

    // Schedule a suspend/wakeup after each check
    // hpx::this_thread::suspend(pause);
  }
  // exit when thread is terminated or shutting down
  return 0;
}

//----------------------------------------------------------------------------
// loops until terminated : Broken, does not terminate correctly
//----------------------------------------------------------------------------
int monitor(double runfor, boost::uint64_t pause)
{
  // timing
  boost::int64_t zero_time = 0;
  hpx::util::high_resolution_timer t;

  if (runfor<0 || t.elapsed()<runfor)
  {
    // try to detect when hpx is shutting down
    hpx::state run_state;
    hpx::runtime* rt = hpx::get_runtime_ptr();
    if (NULL == rt) {
      // we're probably either starting or stopping
      run_state = hpx::state_running;
    }
    else run_state = rt->get_state();
    std::cout << "hpx runtime state is " << (int)run_state << std::endl;

    if (run_state == hpx::state_running)
    {
        hpx::threads::executors::generic_thread_pool_executor exec(
            hpx::this_thread::get_executor());

        exec.add_after(
            boost::chrono::milliseconds(pause),
            hpx::util::bind(&monitor, (std::max)(runfor-t.elapsed(), 0.0), pause),
            "monitor");
    }
  }
  // exit when thread is terminated or shutting down
  return 0;
}

//----------------------------------------------------------------------------
int hpx_main(boost::program_options::variables_map& vm)
{
  //    LOG_DEBUG_MSG("About to instantiate custom scheduler");
  std::cout << "About to create custom scheduler" << std::endl;
  custom_scheduler my_scheduler;
  std::cout << "About to create custom scheduler" << std::endl;

  my_scheduler.init();


  // instead of executing a normal async call using
  //   hpx::async(monitor, -1, 1000);
  // we will manually create a high-priority thread to poll the performance counters
  // we do this so that we get as much info as possible, but it still can be made to wait
  // until a free work queue is available.
  // @TODO This will be moved into a custom scheduler once it is ready.
  hpx::threads::executors::default_executor exec(
    hpx::threads::thread_priority_critical);

  exec.add(hpx::util::bind(&monitor, 10, 1000), "monitor");


  //
  hpx::error_code ec(hpx::lightweight);
  my_scheduler.register_thread_nullary(
    hpx::util::bind(&demo, 10, 1000),
    "demo",
    hpx::threads::pending, true, hpx::threads::thread_priority_critical,
    -1, hpx::threads::thread_stacksize_default, ec);


  hpx::this_thread::sleep_for(boost::chrono::seconds(10));

  my_scheduler.stop();

  return hpx::finalize();
}

//----------------------------------------------------------------------------
// int main just sets up program options and then hands control to HPX
// our main entry is hpx_main()
//
// Caution. HPX adds a lot of command line options and some have the same shortcuts
// (one char representation) - avoid using them until we work out the correct way
// to handle clashes/duplicates.
//
//----------------------------------------------------------------------------
int main(int argc, char* argv[])
{
  // Initialize and run HPX,
  // we want to run hpx_main on all all localities so that each can initialize
  // the stuff needed for the solver manager etc.
  // This command line option is added to tell hpx to run hpx_main on all localities and not just rank 0
  std::vector<std::string> cfg;
  cfg.push_back("hpx.run_hpx_main!=1");
  return hpx::init(argc, argv, cfg);
}


