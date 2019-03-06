//  Copyright (c) 2017-2018 John Biddiscombe
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(EXAMPLES_RESOURCE_PARTITIONER_SHARED_PRIORITY_QUEUE_SCHEDULER)
#define EXAMPLES_RESOURCE_PARTITIONER_SHARED_PRIORITY_QUEUE_SCHEDULER

#include <hpx/config.hpp>
#include <hpx/assertion.hpp>
#include <hpx/runtime/threads/detail/thread_num_tss.hpp>
#include <hpx/runtime/threads/policies/lockfree_queue_backends.hpp>
#include <hpx/runtime/threads/policies/scheduler_base.hpp>
#include <hpx/runtime/threads/policies/thread_queue.hpp>
#include <hpx/runtime/threads/thread_data.hpp>
#include <hpx/runtime/threads/topology.hpp>
#include <hpx/runtime/threads_fwd.hpp>
#include <hpx/throw_exception.hpp>
#include <hpx/logging.hpp>
#include <hpx/util_fwd.hpp>

#if !defined(HPX_MSVC)
#include <plugins/parcelport/parcelport_logging.hpp>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <numeric>
#include <sstream>
#include <type_traits>
#include <vector>

#if !defined(HPX_HAVE_MAX_CPU_COUNT) && defined(HPX_HAVE_MORE_THAN_64_THREADS)
static_assert(false,
    "The shared_priority_scheduler does not support dynamic bitsets for CPU "
    "masks, i.e. HPX_WITH_MAX_CPU_COUNT=\"\" and "
    "HPX_WITH_MORE_THAN_64_THREADS=ON. Reconfigure HPX with either "
    "HPX_WITH_MAX_CPU_COUNT=N, where N is an integer, or disable the "
    "shared_priority_scheduler by setting HPX_WITH_THREAD_SCHEDULERS to not "
    "include \"all\" or \"shared-priority\"");
#else

// #define SHARED_PRIORITY_SCHEDULER_DEBUG 1
// #define SHARED_PRIORITY_SCHEDULER_MINIMAL_DEBUG 1

// for debug reasons, we might want to pretend a single socket machine has 2 numa domains
//#define DEBUG_FORCE_2_NUMA_DOMAINS

#if !defined(HPX_MSVC) && defined(SHARED_PRIORITY_SCHEDULER_DEBUG)
static std::chrono::high_resolution_clock::time_point log_t_start =
    std::chrono::high_resolution_clock::now();

#define COMMA ,
#define LOG_CUSTOM_VAR(x) x

#define LOG_CUSTOM_WORKER(x)                                                   \
    dummy << "<CUSTOM> " << THREAD_ID << " time " << decimal(16) << nowt       \
          << ' ';                                                              \
    if (parent_pool_)                                                          \
        dummy << "pool " << std::setfill(' ') << std::setw(7)                  \
              << parent_pool_->get_pool_name() << " " << x << std::endl;       \
    else                                                                       \
        dummy << "pool (unset) " << x << std::endl;                            \
    std::cout << dummy.str().c_str();

#define LOG_CUSTOM_MSG(x)                                                      \
    std::stringstream dummy;                                                   \
    auto now = std::chrono::high_resolution_clock::now();                      \
    auto nowt = std::chrono::duration_cast<std::chrono::microseconds>(         \
        now - log_t_start)                                                     \
                    .count();                                                  \
    LOG_CUSTOM_WORKER(x);

#define LOG_CUSTOM_MSG2(x)                                                     \
    dummy.str(std::string());                                                  \
    LOG_CUSTOM_WORKER(x);

#define THREAD_DESC(thrd)                                                      \
    "\"" << thrd->get_description().get_description() << "\" "                 \
         << hexpointer(thrd)

#if defined(HPX_HAVE_THREAD_DESCRIPTION)
#define THREAD_DESC2(data, thrd)                                               \
    "\"" << data.description.get_description() << "\" "                        \
         << hexpointer(thrd ? thrd->get() : 0)
#else
#define THREAD_DESC2(data, thrd)                                               \
    hexpointer(thrd ? thrd : 0)
#endif

#else
#define LOG_CUSTOM_VAR(x)
#define LOG_CUSTOM_MSG(x)
#define LOG_CUSTOM_MSG2(x)
#endif

#if defined(HPX_MSVC)
#undef SHARED_PRIORITY_SCHEDULER_DEBUG
#undef LOG_CUSTOM_MSG
#undef LOG_CUSTOM_MSG2
#define LOG_CUSTOM_MSG(x)
#define LOG_CUSTOM_MSG2(x)
#endif

//
namespace hpx { namespace debug {
#ifdef SHARED_PRIORITY_SCHEDULER_MINIMAL_DEBUG
    template<typename T>
    void output(const std::string &name, const std::vector<T> &v) {
        std::cout << name.c_str() << "\t : {" << decnumber(v.size()) << "} : ";
        std::copy(std::begin(v), std::end(v), std::ostream_iterator<T>(std::cout, ", "));
        std::cout << "\n";
    }

    template<typename T, std::size_t N>
    void output(const std::string &name, const std::array<T, N> &v) {
        std::cout << name.c_str() << "\t : {" << decnumber(v.size()) << "} : ";
        std::copy(std::begin(v), std::end(v), std::ostream_iterator<T>(std::cout, ", "));
        std::cout << "\n";
    }

    template<typename Iter>
    void output(const std::string &name, Iter begin, Iter end) {
        std::cout << name.c_str() << "\t : {"
                  << decnumber(std::distance(begin,end)) << "} : ";
        std::copy(begin, end,
            std::ostream_iterator<typename std::iterator_traits<Iter>::
            value_type>(std::cout, ", "));
        std::cout << std::endl;
    }
#else
    template<typename T>
    void output(const std::string &name, const std::vector<T> &v) {
    }

    template<typename T, std::size_t N>
    void output(const std::string &name, const std::array<T, N> &v) {
    }

    template<typename Iter>
    void output(const std::string &name, Iter begin, Iter end) {
    }
#endif
}}

#include <hpx/config/warnings_prefix.hpp>

///////////////////////////////////////////////////////////////////////////////
namespace hpx {
namespace threads {
namespace policies {
namespace example {

    inline void spin_for_time(std::size_t microseconds, const char *task) {
#ifdef SHARED_PRIORITY_SCHEDULER_DEBUG
        hpx::util::annotate_function apex_profiler(task);
        std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
#endif
    }

    // we don't want false sharing, so align to cache line
    struct alignas(64) per_core_counters {
        per_core_counters() : next_queue(0) {}
        unsigned int next_queue;
    };

    ///////////////////////////////////////////////////////////////////////////
    /// The shared_priority_queue_scheduler maintains a set of high, normal, and
    /// low priority queues. For each priority level there is a core/queue ratio
    /// which determines how many cores share a single queue. If the high
    /// priority core/queue ratio is 4 the first 4 cores will share a single
    /// high priority queue, the next 4 will share another one and so on. In
    /// addition, the shared_priority_queue_scheduler is NUMA-aware and takes
    /// NUMA scheduling hints into account when creating and scheduling work.
    template <typename Mutex = std::mutex,
        typename PendingQueuing = lockfree_fifo,
        typename StagedQueuing = lockfree_fifo,
        typename TerminatedQueuing = lockfree_lifo>
    class shared_priority_queue_scheduler : public scheduler_base
    {
    protected:
        // The maximum number of active threads this thread manager should
        // create. This number will be a constraint only as long as the work
        // items queue is not empty. Otherwise the number of active threads
        // will be incremented in steps equal to the \a min_add_new_count
        // specified above.
        // FIXME: this is specified both here, and in thread_queue.
        enum {
            max_thread_count = 1000
        };

    public:
        enum work_assignment_policy {
            assign_work_round_robin,
            assign_work_thread_parent
        };

    public:
        typedef std::false_type has_periodic_maintenance;

        typedef thread_queue<Mutex, PendingQueuing, StagedQueuing,
            TerminatedQueuing>
            thread_queue_type;

        // ------------------------------------------------------------
        shared_priority_queue_scheduler(
            std::size_t num_worker_threads,
            core_ratios cores_per_queue,
            bool numa_stealing,
            bool core_stealing,
            work_assignment_policy work_policy,
            char const* description,
            int max_tasks = max_thread_count)
          : scheduler_base(num_worker_threads, description)
          , cores_per_queue_(cores_per_queue)
          , numa_stealing_(numa_stealing)
          , core_stealing_(core_stealing)
          , work_policy_(work_policy)
          , max_queue_thread_count_(max_tasks)
          , num_workers_(num_worker_threads)
          , num_domains_(1)
          , initialized_(false)
        {
            LOG_CUSTOM_MSG(
                "Constructing shared_priority_queue_scheduler with num threads "
                << decnumber(num_worker_threads));
            //
            HPX_ASSERT(num_worker_threads != 0);
        }

        virtual ~shared_priority_queue_scheduler()
        {
            LOG_CUSTOM_MSG(this->get_description()
                << " - Deleting shared_priority_queue_scheduler ");
        }

        bool numa_sensitive() const override {
            return !numa_stealing_;
        }

        bool has_thread_stealing(std::size_t) const override {
            return core_stealing_;
        }

        static std::string get_scheduler_name()
        {
            return "shared_priority_queue_scheduler";
        }

#ifdef HPX_HAVE_THREAD_CREATION_AND_CLEANUP_RATES
        std::uint64_t get_creation_time(bool reset)
        {
            std::uint64_t time = 0;

            for (std::size_t d = 0; d < num_domains_; ++d) {
                for (auto &queue : lp_queues_[d].queues_) {
                    time += queue->get_creation_time(reset);
                }

                for (auto &queue : np_queues_[d].queues_) {
                    time += queue->get_creation_time(reset);
                }

                for (auto &queue : hp_queues_[d].queues_) {
                    time += queue->get_creation_time(reset);
                }
            }

            return time;
        }

        std::uint64_t get_cleanup_time(bool reset)
        {
            std::uint64_t time = 0;

            for (std::size_t d = 0; d < num_domains_; ++d) {
                for (auto &queue : lp_queues_[d].queues_) {
                    time += queue->get_cleanup_time(reset);
                }

                for (auto &queue : np_queues_[d].queues_) {
                    time += queue->get_cleanup_time(reset);
                }

                for (auto &queue : hp_queues_[d].queues_) {
                    time += queue->get_cleanup_time(reset);
                }
            }

            return time;
        }
#endif

#ifdef HPX_HAVE_THREAD_STEALING_COUNTS
        std::int64_t get_num_pending_misses(
            std::size_t num_thread, bool reset) override
        {
            std::int64_t num_pending_misses = 0;

            if (num_thread == std::size_t(-1))
            {
                for (std::size_t d = 0; d < num_domains_; ++d) {
                    for (auto &queue : lp_queues_[d].queues_) {
                        num_pending_misses += queue->get_num_pending_misses(
                            reset);
                    }

                    for (auto &queue : np_queues_[d].queues_) {
                        num_pending_misses += queue->get_num_pending_misses(
                            reset);
                    }

                    for (auto &queue : hp_queues_[d].queues_) {
                        num_pending_misses += queue->get_num_pending_misses(
                            reset);
                    }
                }

                return num_pending_misses;
            }

            std::size_t domain_num = d_lookup_[num_thread];

            num_pending_misses +=
                lp_queues_[domain_num].queues_[lp_lookup_[num_thread]]->
                    get_num_pending_misses(reset);

            num_pending_misses +=
                np_queues_[domain_num].queues_[np_lookup_[num_thread]]->
                    get_num_pending_misses(reset);

            num_pending_misses +=
                hp_queues_[domain_num].queues_[hp_lookup_[num_thread]]->
                    get_num_pending_misses(reset);

            return num_pending_misses;
        }

        std::int64_t get_num_pending_accesses(
            std::size_t num_thread, bool reset) override
        {
            std::int64_t num_pending_accesses = 0;

            if (num_thread == std::size_t(-1))
            {
                for (std::size_t d = 0; d < num_domains_; ++d) {
                    for (auto &queue : lp_queues_[d].queues_) {
                        num_pending_accesses += queue->get_num_pending_accesses(
                            reset);
                    }

                    for (auto &queue : np_queues_[d].queues_) {
                        num_pending_accesses += queue->get_num_pending_accesses(
                            reset);
                    }

                    for (auto &queue : hp_queues_[d].queues_) {
                        num_pending_accesses += queue->get_num_pending_accesses(
                            reset);
                    }
                }

                return num_pending_accesses;
            }

            std::size_t domain_num = d_lookup_[num_thread];

            num_pending_accesses +=
                lp_queues_[domain_num].queues_[lp_lookup_[num_thread]]->
                    get_num_pending_accesses(reset);

            num_pending_accesses +=
                np_queues_[domain_num].queues_[np_lookup_[num_thread]]->
                    get_num_pending_accesses(reset);

            num_pending_accesses +=
                hp_queues_[domain_num].queues_[hp_lookup_[num_thread]]->
                    get_num_pending_accesses(reset);

            return num_pending_accesses;
        }

        std::int64_t get_num_stolen_from_pending(
            std::size_t num_thread, bool reset) override
        {
            std::int64_t num_stolen_threads = 0;

            if (num_thread == std::size_t(-1))
            {
                for (std::size_t d = 0; d < num_domains_; ++d) {
                    for (auto &queue : lp_queues_[d].queues_) {
                        num_stolen_threads += queue->get_num_stolen_from_pending(
                            reset);
                    }

                    for (auto &queue : np_queues_[d].queues_) {
                        num_stolen_threads += queue->get_num_stolen_from_pending(
                            reset);
                    }

                    for (auto &queue : hp_queues_[d].queues_) {
                        num_stolen_threads += queue->get_num_stolen_from_pending(
                            reset);
                    }
                }

                return num_stolen_threads;
            }

            std::size_t domain_num = d_lookup_[num_thread];

            num_stolen_threads +=
                lp_queues_[domain_num].queues_[lp_lookup_[num_thread]]->
                    get_num_stolen_from_pending(reset);

            num_stolen_threads +=
                np_queues_[domain_num].queues_[np_lookup_[num_thread]]->
                    get_num_stolen_from_pending(reset);

            num_stolen_threads +=
                hp_queues_[domain_num].queues_[hp_lookup_[num_thread]]->
                    get_num_stolen_from_pending(reset);

            return num_stolen_threads;
        }

        std::int64_t get_num_stolen_to_pending(
            std::size_t num_thread, bool reset) override
        {
            std::int64_t num_stolen_threads = 0;

            if (num_thread == std::size_t(-1))
            {
                for (std::size_t d = 0; d < num_domains_; ++d) {
                    for (auto &queue : lp_queues_[d].queues_) {
                        num_stolen_threads += queue->get_num_stolen_to_pending(
                            reset);
                    }

                    for (auto &queue : np_queues_[d].queues_) {
                        num_stolen_threads += queue->get_num_stolen_to_pending(
                            reset);
                    }

                    for (auto &queue : hp_queues_[d].queues_) {
                        num_stolen_threads += queue->get_num_stolen_to_pending(
                            reset);
                    }
                }

                return num_stolen_threads;
            }

            std::size_t domain_num = d_lookup_[num_thread];

            num_stolen_threads +=
                lp_queues_[domain_num].queues_[lp_lookup_[num_thread]]->
                    get_num_stolen_to_pending(reset);

            num_stolen_threads +=
                np_queues_[domain_num].queues_[np_lookup_[num_thread]]->
                    get_num_stolen_to_pending(reset);

            num_stolen_threads +=
                hp_queues_[domain_num].queues_[hp_lookup_[num_thread]]->
                    get_num_stolen_to_pending(reset);

            return num_stolen_threads;
        }

        std::int64_t get_num_stolen_from_staged(
            std::size_t num_thread, bool reset) override
        {
            std::int64_t num_stolen_threads = 0;

            if (num_thread == std::size_t(-1))
            {
                for (std::size_t d = 0; d < num_domains_; ++d) {
                    for (auto &queue : lp_queues_[d].queues_) {
                        num_stolen_threads += queue->get_num_stolen_from_staged(
                            reset);
                    }

                    for (auto &queue : np_queues_[d].queues_) {
                        num_stolen_threads += queue->get_num_stolen_from_staged(
                            reset);
                    }

                    for (auto &queue : hp_queues_[d].queues_) {
                        num_stolen_threads += queue->get_num_stolen_from_staged(
                            reset);
                    }
                }

                return num_stolen_threads;
            }

            std::size_t domain_num = d_lookup_[num_thread];

            num_stolen_threads +=
                lp_queues_[domain_num].queues_[lp_lookup_[num_thread]]->
                    get_num_stolen_from_staged(reset);

            num_stolen_threads +=
                np_queues_[domain_num].queues_[np_lookup_[num_thread]]->
                    get_num_stolen_from_staged(reset);

            num_stolen_threads +=
                hp_queues_[domain_num].queues_[hp_lookup_[num_thread]]->
                    get_num_stolen_from_staged(reset);

            return num_stolen_threads;
        }

        std::int64_t get_num_stolen_to_staged(
            std::size_t num_thread, bool reset) override
        {
            std::int64_t num_stolen_threads = 0;

            if (num_thread == std::size_t(-1))
            {
                for (std::size_t d = 0; d < num_domains_; ++d) {
                    for (auto &queue : lp_queues_[d].queues_) {
                        num_stolen_threads += queue->get_num_stolen_to_staged(
                            reset);
                    }

                    for (auto &queue : np_queues_[d].queues_) {
                        num_stolen_threads += queue->get_num_stolen_to_staged(
                            reset);
                    }

                    for (auto &queue : hp_queues_[d].queues_) {
                        num_stolen_threads += queue->get_num_stolen_to_staged(
                            reset);
                    }
                }

                return num_stolen_threads;
            }

            std::size_t domain_num = d_lookup_[num_thread];

            num_stolen_threads +=
                lp_queues_[domain_num].queues_[lp_lookup_[num_thread]]->
                    get_num_stolen_to_staged(reset);

            num_stolen_threads +=
                np_queues_[domain_num].queues_[np_lookup_[num_thread]]->
                    get_num_stolen_to_staged(reset);

            num_stolen_threads +=
                hp_queues_[domain_num].queues_[hp_lookup_[num_thread]]->
                    get_num_stolen_to_staged(reset);

            return num_stolen_threads;
        }
#endif

#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
        ///////////////////////////////////////////////////////////////////////
        // Queries the current average thread wait time of the queues.
        std::int64_t get_average_thread_wait_time(
            std::size_t num_thread = std::size_t(-1)) const override
        {
            // Return average thread wait time of one specific queue.
            std::uint64_t wait_time = 0;
            std::uint64_t count = 0;

            if (num_thread == std::size_t(-1))
            {
                for (std::size_t d = 0; d < num_domains_; ++d) {
                    for (auto &queue : lp_queues_[d].queues_) {
                        wait_time += queue->get_average_thread_wait_time();
                        ++count;
                    }

                    for (auto &queue : np_queues_[d].queues_) {
                        wait_time += queue->get_average_thread_wait_time();
                        ++count;
                    }

                    for (auto &queue : hp_queues_[d].queues_) {
                        wait_time += queue->get_average_thread_wait_time();
                        ++count;
                    }
                }

                return wait_time / (count + 1);
            }

            std::size_t domain_num = d_lookup_[num_thread];

            wait_time +=
                lp_queues_[domain_num].queues_[lp_lookup_[num_thread]]->
                    get_average_thread_wait_time();
            ++count;

            wait_time +=
                np_queues_[domain_num].queues_[np_lookup_[num_thread]]->
                    get_average_thread_wait_time();
            ++count;

            wait_time +=
                hp_queues_[domain_num].queues_[hp_lookup_[num_thread]]->
                    get_average_thread_wait_time();
            ++count;

            return wait_time / (count + 1);
        }

        ///////////////////////////////////////////////////////////////////////
        // Queries the current average task wait time of the queues.
        std::int64_t get_average_task_wait_time(
            std::size_t num_thread = std::size_t(-1)) const override
        {
            // Return average task wait time of one specific queue.
            std::uint64_t wait_time = 0;
            std::uint64_t count = 0;

            if (num_thread == std::size_t(-1))
            {
                for (std::size_t d = 0; d < num_domains_; ++d) {
                    for (auto &queue : lp_queues_[d].queues_) {
                        wait_time += queue->get_average_task_wait_time();
                        ++count;
                    }

                    for (auto &queue : np_queues_[d].queues_) {
                        wait_time += queue->get_average_task_wait_time();
                        ++count;
                    }

                    for (auto &queue : hp_queues_[d].queues_) {
                        wait_time += queue->get_average_task_wait_time();
                        ++count;
                    }
                }

                return wait_time / (count + 1);
            }

            std::size_t domain_num = d_lookup_[num_thread];

            wait_time +=
                lp_queues_[domain_num].queues_[lp_lookup_[num_thread]]->
                    get_average_task_wait_time();
            ++count;

            wait_time +=
                np_queues_[domain_num].queues_[np_lookup_[num_thread]]->
                    get_average_task_wait_time();
            ++count;

            wait_time +=
                hp_queues_[domain_num].queues_[hp_lookup_[num_thread]]->
                    get_average_task_wait_time();
            ++count;

            return wait_time / (count + 1);
        }
#endif

        // ------------------------------------------------------------
        void abort_all_suspended_threads() override
        {
            LOG_CUSTOM_MSG("abort_all_suspended_threads");
            for (std::size_t d = 0; d < num_domains_; ++d) {
                np_queues_[d].abort_all_suspended_threads();
                hp_queues_[d].abort_all_suspended_threads();
                lp_queues_[d].abort_all_suspended_threads();
            }
        }

        // ------------------------------------------------------------
        bool cleanup_terminated(bool delete_all) override
        {
//            LOG_CUSTOM_MSG("cleanup_terminated with delete_all "
//                << delete_all);
            bool empty = true;

            for (std::size_t d=0; d<num_domains_; ++d) {
                empty = empty &&
                    np_queues_[d].cleanup_terminated(delete_all);
                if (cores_per_queue_.high_priority>0) empty = empty &&
                    hp_queues_[d].cleanup_terminated(delete_all);
                if (cores_per_queue_.low_priority>0) empty = empty &&
                    lp_queues_[d].cleanup_terminated(delete_all);
            }
            return empty;
        }

        // ------------------------------------------------------------
        bool cleanup_terminated(std::size_t thread_num, bool delete_all) override
        {
            HPX_ASSERT(thread_num>=0);
            bool empty = true;
            // find the numa domain from the local thread index
            std::size_t domain_num = d_lookup_[thread_num];
            // cleanup the queues assigned to this thread
            empty = empty && np_queues_[domain_num].queues_[np_lookup_[thread_num]]->
                    cleanup_terminated(delete_all);
            if (cores_per_queue_.high_priority>0)
                empty = empty && hp_queues_[domain_num].queues_[hp_lookup_[thread_num]]->
                    cleanup_terminated(delete_all);
            if (cores_per_queue_.low_priority>0)
                empty = empty && lp_queues_[domain_num].queues_[lp_lookup_[thread_num]]->
                    cleanup_terminated(delete_all);
            return empty;
        }

        // ------------------------------------------------------------
        // create a new thread and schedule it if the initial state
        // is equal to pending
        void create_thread(thread_init_data& data, thread_id_type* thrd,
            thread_state_enum initial_state, bool run_now, error_code& ec) override
        {
            // safety check that task was created by this thread/scheduler
            HPX_ASSERT(data.scheduler_base == this);

            std::size_t thread_num = 0;
            std::size_t domain_num = 0;
            std::size_t q_index = std::size_t(-1);

            LOG_CUSTOM_VAR(const char* const msgs[] =
                {"HINT_NONE" COMMA "HINT....." COMMA "ERROR...." COMMA  "NORMAL..."});
            LOG_CUSTOM_VAR(const char *msg = nullptr);

            std::unique_lock<pu_mutex_type> l;

            using threads::thread_schedule_hint_mode;
            switch (data.schedulehint.mode) {
            case thread_schedule_hint_mode::thread_schedule_hint_mode_none:
            {
                // Create thread on this worker thread if possible
                LOG_CUSTOM_VAR(msg = msgs[0]);
                std::size_t global_thread_num = hpx::get_worker_thread_num();
                thread_num = this->global_to_local_thread_index(global_thread_num);
                if (thread_num>=num_workers_) {
                    LOG_ERROR_MSG("thread numbering overflow xPool injection " << thread_num);
                    // This is a task being injected from a thread on another pool.
                    // Reset thread_num to first queue.
                    thread_num = fast_mod(core_counters_[thread_num].next_queue++, num_workers_);
                }
                if (work_policy_ == assign_work_round_robin) {
                    thread_num = fast_mod(core_counters_[thread_num].next_queue++, num_workers_);
                    LOG_CUSTOM_MSG("round robin assignment " << thread_num);
                }
                thread_num = select_active_pu(l, thread_num);
                domain_num = d_lookup_[thread_num];
                q_index    = q_lookup_[thread_num];
                break;
            }
            case thread_schedule_hint_mode::thread_schedule_hint_mode_thread:
            {
                // Create thread on requested worker thread
                LOG_CUSTOM_VAR(msg = msgs[3]);
                thread_num = select_active_pu(l, data.schedulehint.hint);
                domain_num = d_lookup_[thread_num];
                q_index    = q_lookup_[thread_num];
                break;
            }
            case thread_schedule_hint_mode::thread_schedule_hint_mode_numa:
            {
                // Create thread on requested NUMA domain

                // TODO: This case does not handle suspended PUs.
                LOG_CUSTOM_VAR(msg = msgs[1]);
                domain_num = fast_mod(data.schedulehint.hint, num_domains_);
                // if the thread creating the new task is on the domain
                // assigned to the new task - try to reuse the core as well
                std::size_t global_thread_num = hpx::get_worker_thread_num();
                thread_num = this->global_to_local_thread_index(global_thread_num);
                if (d_lookup_[thread_num] == domain_num) {
                    q_index = q_lookup_[thread_num];
                }
                else {
                    q_index = counters_[domain_num]++;
                }
                break;
            }
            default:
                HPX_THROW_EXCEPTION(bad_parameter,
                    "shared_priority_queue_scheduler::create_thread",
                    "Invalid schedule hint mode: " +
                    std::to_string(data.schedulehint.mode));
            }

            LOG_CUSTOM_MSG("create_thread " << msg << " "
                           << "queue " << decnumber(thread_num)
                           << "domain " << decnumber(domain_num)
                           << "qindex " << decnumber(q_index));

            // create the thread using priority to select queue
            if ((cores_per_queue_.high_priority>0) &&
                (data.priority == thread_priority_high ||
                 data.priority == thread_priority_high_recursive ||
                 data.priority == thread_priority_boost))
            {
                // boosted threads return to normal after being queued
                if (data.priority == thread_priority_boost)
                {
                    data.priority = thread_priority_normal;
                }

                hp_queues_[domain_num].queues_[hp_lookup_[
                    fast_mod(q_index, hp_queues_[domain_num].num_cores)]]->
                    create_thread(data, thrd, initial_state, run_now, ec);

                LOG_CUSTOM_MSG("create_thread thread_priority_high "
                               << "queue " << decnumber(q_index)
                               << "domain " << decnumber(domain_num)
                               << "desc " << THREAD_DESC2(data, thrd)
                               << "scheduler " << hexpointer(data.scheduler_base));
                return;
            }

            if ((cores_per_queue_.low_priority>0) &&
                (data.priority == thread_priority_low))
            {
                lp_queues_[domain_num].queues_[lp_lookup_[
                    fast_mod(q_index, lp_queues_[domain_num].num_cores)]]->
                    create_thread(data, thrd, initial_state, run_now, ec);

                LOG_CUSTOM_MSG("create_thread thread_priority_low "
                               << "queue " << decnumber(q_index)
                               << "domain " << decnumber(domain_num)
                               << "desc " << THREAD_DESC2(data, thrd)
                               << "scheduler " << hexpointer(data.scheduler_base));
                return;
            }

            // normal priority + anything unassigned above (no hp queues etc)
            np_queues_[domain_num].queues_[np_lookup_[
                fast_mod(q_index, np_queues_[domain_num].num_cores)]]->
                create_thread(data, thrd, initial_state, run_now, ec);

            LOG_CUSTOM_MSG2("create_thread thread_priority_normal "
                            << "queue " << decnumber(q_index)
                            << "domain " << decnumber(domain_num)
                            << "desc " << THREAD_DESC2(data, thrd)
                            << "scheduler " << hexpointer(data.scheduler_base));
        }

        /// Return the next thread to be executed, return false if none available
        virtual bool get_next_thread(std::size_t thread_num, bool running,
            threads::thread_data*& thrd, bool /*enable_stealing*/) override
        {
            // LOG_CUSTOM_MSG("get_next_thread " << decnumber(thread_num));
            bool result = false;

            if (thread_num == std::size_t(-1)) {
                HPX_THROW_EXCEPTION(bad_parameter,
                    "shared_priority_queue_scheduler::get_next_thread",
                    "Invalid thread number: " + std::to_string(thread_num));
            }

            // find the numa domain from the local thread index
            std::size_t domain_num = d_lookup_[thread_num];

            // High priority task
            if (cores_per_queue_.high_priority>0) {
                for (std::size_t d=0; d<num_domains_; ++d) {
                    std::size_t dom = fast_mod((domain_num+d), num_domains_);
                    // set the preferred queue for this domain, if applicable
                    std::size_t q_index = q_lookup_[thread_num];
                    // get next task, steal if from another domain
                    result = hp_queues_[dom].get_next_thread(q_index, thrd, core_stealing_);
                    if (result) {
                        LOG_CUSTOM_MSG("got HP thread "
                           << "queue " << decnumber(thread_num)
                           << "domain " << decnumber(domain_num)
                           << "desc " << THREAD_DESC(thrd));
                        return result;
                    }
                    if (!numa_stealing_) break;
                }
            }

            // try a normal priority task
            for (std::size_t d=0; d<num_domains_; ++d) {
                std::size_t dom = fast_mod((domain_num+d), num_domains_);
                // set the preferred queue for this domain, if applicable
                std::size_t q_index = q_lookup_[thread_num];
                // get next task, steal if from another domain
                result = np_queues_[dom].get_next_thread(q_index, thrd, core_stealing_);
                if (result) {
                    LOG_CUSTOM_MSG("got NP thread "
                        << "queue " << decnumber(thread_num)
                        << "domain " << decnumber(domain_num)
                        << "desc " << THREAD_DESC(thrd));
                    return result;
                }
                if (!numa_stealing_) break;
            }

            // low priority task, x-numa stealing never happens
            if (cores_per_queue_.low_priority>0) {
                result = lp_queues_[domain_num].get_next_thread(0, thrd, core_stealing_);
                if (result) {
                    LOG_CUSTOM_MSG("got LP thread 1 "
                        << "queue " << decnumber(thread_num)
                        << "domain " << decnumber(domain_num)
                        << "desc " << THREAD_DESC(thrd));
                    return result;
                }
            }
            return false;
        }

        /// Schedule the passed thread
        void schedule_thread(threads::thread_data* thrd,
            threads::thread_schedule_hint schedulehint,
            bool allow_fallback,
            thread_priority priority = thread_priority_normal) override
        {
            HPX_ASSERT(thrd->get_scheduler_base() == this);

            std::size_t thread_num = 0;
            std::size_t domain_num = 0;
            std::size_t q_index = std::size_t(-1);

            LOG_CUSTOM_VAR(const char* const msgs[] =
                {"HINT_NONE" COMMA "HINT....." COMMA "ERROR...." COMMA  "NORMAL..."});
            LOG_CUSTOM_VAR(const char *msg = nullptr);

            std::unique_lock<pu_mutex_type> l;

            using threads::thread_schedule_hint_mode;

            switch (schedulehint.mode) {
            case thread_schedule_hint_mode::thread_schedule_hint_mode_none:
            {
                // Create thread on this worker thread if possible
                LOG_CUSTOM_VAR(msg = msgs[0]);
                std::size_t global_thread_num = hpx::get_worker_thread_num();
                thread_num = this->global_to_local_thread_index(global_thread_num);
                if (thread_num>=num_workers_) {
                    // This is a task being injected from a thread on another pool.
                    // Reset thread_num to first queue.
                    thread_num = 0;
                }
                thread_num = select_active_pu(l, thread_num, allow_fallback);
                domain_num = d_lookup_[thread_num];
                q_index    = q_lookup_[thread_num];
                break;
            }
            case thread_schedule_hint_mode::thread_schedule_hint_mode_thread:
            {
                // Create thread on requested worker thread
                LOG_CUSTOM_VAR(msg = msgs[3]);
                thread_num = select_active_pu(l, schedulehint.hint,
                    allow_fallback);
                domain_num = d_lookup_[thread_num];
                q_index    = q_lookup_[thread_num];
                break;
            }
            case thread_schedule_hint_mode::thread_schedule_hint_mode_numa:
            {
                // Create thread on requested NUMA domain

                // TODO: This case does not handle suspended PUs.
                LOG_CUSTOM_VAR(msg = msgs[1]);
                domain_num = fast_mod(schedulehint.hint, num_domains_);
                // if the thread creating the new task is on the domain
                // assigned to the new task - try to reuse the core as well
                std::size_t global_thread_num = hpx::get_worker_thread_num();
                thread_num = this->global_to_local_thread_index(global_thread_num);
                if (d_lookup_[thread_num] == domain_num) {
                    q_index = q_lookup_[thread_num];
                }
                else {
                    q_index = counters_[domain_num]++;
                }
                break;
            }
            default:
                HPX_THROW_EXCEPTION(bad_parameter,
                    "shared_priority_queue_scheduler::schedule_thread",
                    "Invalid schedule hint mode: " +
                    std::to_string(schedulehint.mode));
            }

            LOG_CUSTOM_MSG("thread scheduled "
                          << "queue " << decnumber(thread_num)
                          << "domain " << decnumber(domain_num)
                          << "qindex " << decnumber(q_index));

            if ((cores_per_queue_.high_priority>0) &&
                (priority == thread_priority_high ||
                 priority == thread_priority_high_recursive ||
                 priority == thread_priority_boost))
            {
                hp_queues_[domain_num].queues_[hp_lookup_[
                    fast_mod(q_index, hp_queues_[domain_num].num_cores)]]->
                    schedule_thread(thrd, false);
            }
            else if ((cores_per_queue_.low_priority>0) &&
                     (priority == thread_priority_low))
            {
                lp_queues_[domain_num].queues_[lp_lookup_[
                    fast_mod(q_index, lp_queues_[domain_num].num_cores)]]->
                    schedule_thread(thrd, false);
            }
            else
            {
                np_queues_[domain_num].queues_[np_lookup_[
                    fast_mod(q_index, np_queues_[domain_num].num_cores)]]->
                    schedule_thread(thrd, false);
            }
        }

        /// Put task on the back of the queue : not yet implemented
        /// just put it on the normal queue for now
        void schedule_thread_last(threads::thread_data* thrd,
            threads::thread_schedule_hint schedulehint,
            bool allow_fallback,
            thread_priority priority = thread_priority_normal) override
        {
            LOG_CUSTOM_MSG("schedule_thread_last ");
            schedule_thread(thrd, schedulehint, allow_fallback, priority);
        }

        //---------------------------------------------------------------------
        // Destroy the passed thread - as it has been terminated
        //---------------------------------------------------------------------
        void destroy_thread(
            threads::thread_data* thrd, std::int64_t& busy_count) override
        {
            HPX_ASSERT(thrd->get_scheduler_base() == this);
            thrd->get_queue<thread_queue_type>().destroy_thread(thrd, busy_count);
        }

        ///////////////////////////////////////////////////////////////////////
        // This returns the current length of the queues (work items and new
        // items)
        //---------------------------------------------------------------------
        std::int64_t get_queue_length(
            std::size_t thread_num = std::size_t(-1)) const override
        {
            LOG_CUSTOM_MSG("get_queue_length"
                << "thread_num " << decnumber(thread_num));

            HPX_ASSERT(thread_num != std::size_t(-1));

            std::int64_t count = 0;
            if (thread_num != std::size_t(-1)) {
                // find the numa domain from the local thread index
                std::size_t domain = d_lookup_[thread_num];
                count += np_queues_[domain].queues_[np_lookup_[thread_num]]->
                        get_queue_length();

                if (cores_per_queue_.high_priority>0)
                    count += hp_queues_[domain].queues_[hp_lookup_[thread_num]]->
                        get_queue_length();

                if (cores_per_queue_.low_priority>0)
                    count += lp_queues_[domain].queues_[lp_lookup_[thread_num]]->
                        get_queue_length();
            }
            else {
                for (std::size_t d=0; d<num_domains_; ++d) {
                    count += np_queues_[d].get_queue_length();
                    count += hp_queues_[d].get_queue_length();
                    count += lp_queues_[d].get_queue_length();
                }
            }

            return count;
        }

        //---------------------------------------------------------------------
        // Queries the current thread count of the queues.
        //---------------------------------------------------------------------
        std::int64_t get_thread_count(thread_state_enum state = unknown,
            thread_priority priority = thread_priority_default,
            std::size_t thread_num = std::size_t(-1),
            bool reset = false) const override
        {
            LOG_CUSTOM_MSG("get_thread_count thread_num "
                << hexnumber(thread_num));

            std::int64_t count = 0;

            // if a specific worker id was requested
            if (thread_num != std::size_t(-1)) {
                std::size_t domain = d_lookup_[thread_num];
                //
                switch (priority) {
                case thread_priority_default: {
                    count += np_queues_[domain].queues_[np_lookup_[thread_num]]->
                            get_thread_count(state);
                    if (cores_per_queue_.high_priority>0)
                        count += hp_queues_[domain].queues_[hp_lookup_[thread_num]]->
                            get_thread_count();
                    if (cores_per_queue_.low_priority>0)
                        count += lp_queues_[domain].queues_[lp_lookup_[thread_num]]->
                            get_thread_count();
                    LOG_CUSTOM_MSG("default get_thread_count thread_num "
                        << hexnumber(thread_num) << decnumber(count));
                    return count;
                }
                case thread_priority_low: {
                    if (cores_per_queue_.low_priority>0)
                        count += lp_queues_[domain].queues_[lp_lookup_[thread_num]]->
                            get_thread_count();
                    LOG_CUSTOM_MSG("low get_thread_count thread_num "
                        << hexnumber(thread_num) << decnumber(count));
                    return count;
                }
                case thread_priority_normal: {
                    count += np_queues_[domain].queues_[np_lookup_[thread_num]]->
                        get_thread_count(state);
                    LOG_CUSTOM_MSG("normal get_thread_count thread_num "
                        << hexnumber(thread_num) << decnumber(count));
                    return count;
                }
                case thread_priority_boost:
                case thread_priority_high:
                case thread_priority_high_recursive: {
                    if (cores_per_queue_.high_priority>0)
                        count += hp_queues_[domain].queues_[hp_lookup_[thread_num]]->
                            get_thread_count();
                    return count;
                }
                default:
                case thread_priority_unknown:
                    HPX_THROW_EXCEPTION(bad_parameter,
                        "shared_priority_queue_scheduler::get_thread_count",
                        "unknown thread priority (thread_priority_unknown)");
                    return 0;
                }
            }

            switch (priority) {
            case thread_priority_default: {
                for (std::size_t d=0; d<num_domains_; ++d) {
                    count += hp_queues_[d].get_thread_count(state);
                    count += np_queues_[d].get_thread_count(state);
                    count += lp_queues_[d].get_thread_count(state);
                }
                    LOG_CUSTOM_MSG("default get_thread_count thread_num "
                        << decnumber(thread_num) << decnumber(count));
                return count;
            }
            case thread_priority_low: {
                for (std::size_t d=0; d<num_domains_; ++d) {
                    count += lp_queues_[d].get_thread_count(state);
                }
                LOG_CUSTOM_MSG("low get_thread_count thread_num "
                    << decnumber(thread_num) << decnumber(count));
                return count;
            }
            case thread_priority_normal: {
                for (std::size_t d=0; d<num_domains_; ++d) {
                    count += np_queues_[d].get_thread_count(state);
                }
                LOG_CUSTOM_MSG("normal get_thread_count thread_num "
                    << decnumber(thread_num) << decnumber(count));
                return count;
            }
            case thread_priority_boost:
            case thread_priority_high:
            case thread_priority_high_recursive: {
                for (std::size_t d=0; d<num_domains_; ++d) {
                    count += hp_queues_[d].get_thread_count(state);
                }
                LOG_CUSTOM_MSG("high get_thread_count thread_num "
                    << decnumber(thread_num) << decnumber(count));
                return count;
            }
            default:
            case thread_priority_unknown:
                HPX_THROW_EXCEPTION(bad_parameter,
                    "shared_priority_queue_scheduler::get_thread_count",
                    "unknown thread priority (thread_priority_unknown)");
                return 0;
            }

            return count;
        }

        ///////////////////////////////////////////////////////////////////////
        // Enumerate matching threads from all queues
        bool enumerate_threads(
            util::function_nonser<bool(thread_id_type)> const& f,
            thread_state_enum state = unknown) const override
        {
            bool result = true;

            LOG_CUSTOM_MSG("enumerate_threads");

            for (std::size_t d=0; d<num_domains_; ++d) {
                result = result &&
                    np_queues_[d].enumerate_threads(f, state);
                result = result &&
                    hp_queues_[d].enumerate_threads(f, state);
                result = result &&
                    lp_queues_[d].enumerate_threads(f, state);
            }
            return result;
        }

        /// This is a function which gets called periodically by the thread
        /// manager to allow for maintenance tasks to be executed in the
        /// scheduler. Returns true if the OS thread calling this function
        /// has to be terminated (i.e. no more work has to be done).
        virtual bool wait_or_add_new(std::size_t thread_num, bool running,
            std::int64_t& idle_loop_count, bool /*enable_stealing*/,
            std::size_t& added) override
        {
            added = 0;
            bool result = false;

            HPX_ASSERT(thread_num != std::size_t(-1));

            LOG_CUSTOM_MSG("wait_or_add_new thread num " << decnumber(thread_num));

            // find the numa domain from the local thread index
            std::size_t domain_num = d_lookup_[thread_num];

            // is there a high priority task, take first from our numa domain
            // and then try to steal from others
            if (cores_per_queue_.high_priority>0) {
                for (std::size_t d=0; d<num_domains_; ++d) {
                    std::size_t dom = fast_mod((domain_num+d), num_domains_);
                    // set the preferred queue for this domain, if applicable
                    std::size_t q_index = q_lookup_[thread_num];
                    // get next task, steal if from another domain
                    result = hp_queues_[dom].wait_or_add_new(q_index, running,
                        added, core_stealing_);
                    if (0 != added) return result;
                    if (!numa_stealing_) break;
                }
            }

            // try a normal priority task
            for (std::size_t d=0; d<num_domains_; ++d) {
                std::size_t dom = fast_mod((domain_num+d), num_domains_);
                // set the preferred queue for this domain, if applicable
                std::size_t q_index = q_lookup_[thread_num];
                // get next task, steal if from another domain
                result = np_queues_[dom].wait_or_add_new(q_index, running,
                    added, core_stealing_);
                if (0 != added) return result;
                if (!numa_stealing_) break;
            }

            // low priority task
            // no cross domain stealing for LP queues
            if (cores_per_queue_.low_priority>0) {
                result = lp_queues_[domain_num].wait_or_add_new(0, running,
                    added, core_stealing_);
                if (0 != added) return result;
            }
            return result;
        }

        ///////////////////////////////////////////////////////////////////////
        void on_start_thread(std::size_t thread_num) override
        {
            LOG_CUSTOM_MSG("start thread with local thread num "
                << decnumber(thread_num));

            std::unique_lock<hpx::lcos::local::spinlock> lock(init_mutex);
            if (!initialized_)
            {
                initialized_ = true;

                auto &rp = resource::get_partitioner();
                auto const& topo = rp.get_topology();

                // For each worker thread, count which each numa domain they
                // belong to and build lists of useful indexes/refs
                num_domains_ = 1;
                std::array<std::size_t, HPX_HAVE_MAX_NUMA_DOMAIN_COUNT> q_counts_;
                std::fill(d_lookup_.begin(), d_lookup_.end(), 0);
                std::fill(q_lookup_.begin(), q_lookup_.end(), 0);
                std::fill(q_counts_.begin(), q_counts_.end(), 0);
                std::fill(counters_.begin(), counters_.end(), 0);
                std::fill(core_counters_.begin(), core_counters_.end(), per_core_counters());

                for (std::size_t local_id=0; local_id!=num_workers_; ++local_id)
                {
                    std::size_t global_id = local_to_global_thread_index(local_id);
                    std::size_t pu_num = rp.get_pu_num(global_id);
                    std::size_t domain = topo.get_numa_node_number(pu_num);
                    d_lookup_[local_id] = domain;
                    num_domains_ = (std::max)(num_domains_, domain+1);
                }

                HPX_ASSERT(num_domains_ <= HPX_HAVE_MAX_NUMA_DOMAIN_COUNT);

                for (std::size_t local_id=0; local_id!=num_workers_; ++local_id)
                {
                    q_lookup_[local_id] = q_counts_[d_lookup_[local_id]]++;
                }

                // create queue sets for each numa domain
                for (std::size_t i=0; i<num_domains_; ++i) {
                    std::size_t queues = cores_per_queue_.high_priority>0 ?
                        (std::max)(q_counts_[i] / cores_per_queue_.high_priority,
                        std::size_t(1)) : 0;
                    hp_queues_[i].init(
                        q_counts_[i], queues, max_queue_thread_count_);
                    LOG_CUSTOM_MSG2("Created HP queue for numa " << i
                                    << " cores " << q_counts_[i]
                                    << " queues " << queues);

                    queues = (std::max)(
                        q_counts_[i] / cores_per_queue_.normal_priority,
                        std::size_t(1));
                    np_queues_[i].init(
                        q_counts_[i], queues, max_queue_thread_count_);
                    LOG_CUSTOM_MSG2("Created NP queue for numa " << i
                                    << " cores " << q_counts_[i]
                                    << " queues " << queues);

                    queues = cores_per_queue_.low_priority>0 ?
                        (std::max)(q_counts_[i] / cores_per_queue_.low_priority,
                            std::size_t(1)) : 0;
                    lp_queues_[i].init(
                        q_counts_[i], queues, max_queue_thread_count_);
                    LOG_CUSTOM_MSG2("Created LP queue for numa " << i
                                    << " cores " << q_counts_[i]
                                    << " queues " << queues);
                }

                // create worker_id to queue lookups for each queue type
                for (std::size_t local_id=0; local_id!=num_workers_; ++local_id)
                {
                    hp_lookup_[local_id] = hp_queues_[d_lookup_[local_id]].
                        get_queue_index(q_lookup_[local_id]);
                    np_lookup_[local_id] = np_queues_[d_lookup_[local_id]].
                        get_queue_index(q_lookup_[local_id]);
                    lp_lookup_[local_id] = lp_queues_[d_lookup_[local_id]].
                        get_queue_index(q_lookup_[local_id]);
                }

                debug::output("d_lookup_  ", &d_lookup_[0],  &d_lookup_[num_workers_]);
                debug::output("q_lookup_  ", &q_lookup_[0],  &q_lookup_[num_workers_]);
                debug::output("hp_lookup_ ", &hp_lookup_[0], &hp_lookup_[num_workers_]);
                debug::output("np_lookup_ ", &np_lookup_[0], &np_lookup_[num_workers_]);
                debug::output("lp_lookup_ ", &lp_lookup_[0], &lp_lookup_[num_workers_]);
                debug::output("counters_  ", &counters_[0],  &counters_[num_domains_]);
                debug::output("q_counts_  ", &q_counts_[0],  &q_counts_[num_domains_]);
            }

            lock.unlock();

            std::size_t domain_num = d_lookup_[thread_num];

            // NOTE: This may call on_start_thread multiple times for a single
            // thread_queue.
            if (cores_per_queue_.high_priority>0)
                hp_queues_[domain_num].queues_[hp_lookup_[thread_num]]->
                    on_start_thread(thread_num);

            np_queues_[domain_num].queues_[np_lookup_[thread_num]]->
                on_start_thread(thread_num);

            if (cores_per_queue_.low_priority>0)
                lp_queues_[domain_num].queues_[lp_lookup_[thread_num]]->
                    on_start_thread(thread_num);
        }

        void on_stop_thread(std::size_t thread_num) override
        {
            LOG_CUSTOM_MSG("on_stop_thread with local thread num "
                << decnumber(thread_num));

            if (thread_num>num_workers_) {
                HPX_THROW_EXCEPTION(bad_parameter,
                    "shared_priority_queue_scheduler::on_stop_thread",
                    "Invalid thread number: " + std::to_string(thread_num));
            }

            std::size_t domain_num = d_lookup_[thread_num];

            // NOTE: This may call on_stop_thread multiple times for a single
            // thread_queue.
            if (cores_per_queue_.high_priority>0)
                hp_queues_[domain_num].queues_[hp_lookup_[thread_num]]->
                    on_stop_thread(thread_num);

            np_queues_[domain_num].queues_[np_lookup_[thread_num]]->
                on_stop_thread(thread_num);

            if (cores_per_queue_.low_priority>0)
                lp_queues_[domain_num].queues_[lp_lookup_[thread_num]]->
                    on_stop_thread(thread_num);
        }

        void on_error(
            std::size_t thread_num, std::exception_ptr const& e) override
        {
            LOG_CUSTOM_MSG("on_error with local thread num "
                << decnumber(thread_num));

            if (thread_num>num_workers_) {
                HPX_THROW_EXCEPTION(bad_parameter,
                    "shared_priority_queue_scheduler::on_error",
                    "Invalid thread number: " + std::to_string(thread_num));
            }

            std::size_t domain_num = d_lookup_[thread_num];

            // NOTE: This may call on_error multiple times for a single
            // thread_queue.
            if (cores_per_queue_.high_priority>0)
                hp_queues_[domain_num].queues_[hp_lookup_[thread_num]]->
                    on_error(thread_num, e);

            np_queues_[domain_num].queues_[np_lookup_[thread_num]]->
                on_error(thread_num, e);

            if (cores_per_queue_.low_priority>0)
                lp_queues_[domain_num].queues_[lp_lookup_[thread_num]]->
                    on_error(thread_num, e);

        }

        void reset_thread_distribution() override
        {
            std::fill(counters_.begin(), counters_.end(), 0);
        }

    protected:
        typedef queue_holder<thread_queue_type> numa_queues;

        std::array<numa_queues, HPX_HAVE_MAX_NUMA_DOMAIN_COUNT> np_queues_;
        std::array<numa_queues, HPX_HAVE_MAX_NUMA_DOMAIN_COUNT> hp_queues_;
        std::array<numa_queues, HPX_HAVE_MAX_NUMA_DOMAIN_COUNT> lp_queues_;
        std::array<std::size_t, HPX_HAVE_MAX_NUMA_DOMAIN_COUNT> counters_;

        // lookup domain from local worker index
        std::array<std::size_t, HPX_HAVE_MAX_CPU_COUNT> d_lookup_;

        // index of queue on domain from local worker index
        std::array<std::size_t, HPX_HAVE_MAX_CPU_COUNT> hp_lookup_;
        std::array<std::size_t, HPX_HAVE_MAX_CPU_COUNT> np_lookup_;
        std::array<std::size_t, HPX_HAVE_MAX_CPU_COUNT> lp_lookup_;

        // lookup sub domain queue index from local worker index
        std::array<std::size_t, HPX_HAVE_MAX_CPU_COUNT> q_lookup_;
        std::array<per_core_counters, HPX_HAVE_MAX_CPU_COUNT> core_counters_;

        // number of cores per queue for HP, NP, LP queues
        core_ratios cores_per_queue_;

        // when true, numa_stealing permits stealing across numa domains,
        // when false, no stealing takes place across numa domains,
        bool numa_stealing_;

        // when true, core_stealing permits stealing between cores(queues),
        // when false, no stealing takes place between any cores(queues)
        bool core_stealing_;

        work_assignment_policy work_policy_;


        // max storage size of any queue
        std::size_t max_queue_thread_count_;

        // number of worker threads assigned to this pool
        std::size_t num_workers_;

        // number of numa domains that the threads are occupying
        std::size_t num_domains_;

        // used to make sure the scheduler is only initialized once on a thread
        bool initialized_;
        hpx::lcos::local::spinlock init_mutex;
    };
}}}}
#endif

#include <hpx/config/warnings_suffix.hpp>

#endif
