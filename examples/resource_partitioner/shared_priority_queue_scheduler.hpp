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
#include <hpx/runtime/threads/policies/thread_queue_mc.hpp>
#include <hpx/runtime/threads/policies/queue_holder_thread.hpp>
#include <hpx/runtime/threads/policies/queue_holder_numa.hpp>
#include <hpx/runtime/threads/policies/scheduler_base.hpp>
#include <hpx/runtime/threads/thread_data.hpp>
#include <hpx/runtime/threads/topology.hpp>
#include <hpx/runtime/threads_fwd.hpp>
#include <hpx/runtime/threads/detail/thread_num_tss.hpp>
#include <hpx/throw_exception.hpp>
#include <hpx/logging.hpp>
#include <hpx/util/yield_while.hpp>
#include <hpx/util_fwd.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <numeric>
#include <type_traits>
#include <vector>

#include <hpx/config/warnings_prefix.hpp>

#if !defined(HPX_HAVE_MAX_CPU_COUNT) && defined(HPX_HAVE_MORE_THAN_64_THREADS)
static_assert(false,
    "The shared_priority_scheduler does not support dynamic bitsets for CPU "
    "masks, i.e. HPX_WITH_MAX_CPU_COUNT=\"\" and "
    "HPX_WITH_MORE_THAN_64_THREADS=ON. Reconfigure HPX with either "
    "HPX_WITH_MAX_CPU_COUNT=N, where N is an integer, or disable the "
    "shared_priority_scheduler by setting HPX_WITH_THREAD_SCHEDULERS to not "
    "include \"all\" or \"shared-priority\"");
#else

#undef LOG_CUSTOM_MSG
#undef LOG_CUSTOM_MSG2
#define LOG_CUSTOM_MSG(a)
#define LOG_CUSTOM_MSG2(a)
#define LOG_CUSTOM_VAR(a)

#define SHARED_PRIORITY_QUEUE_SCHEDULER_API 2

// ------------------------------------------------------------

namespace hpx {
namespace threads {
namespace policies {
namespace example {

    // Holds core/queue ratios used by schedulers.
    struct core_ratios
    {
        core_ratios(std::size_t high_priority, std::size_t normal_priority,
            std::size_t low_priority)
          : high_priority(high_priority), normal_priority(normal_priority),
            low_priority(low_priority) {}

        std::size_t high_priority;
        std::size_t normal_priority;
        std::size_t low_priority;
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

        typedef thread_queue_mc<> thread_queue_type;

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
            HPX_ASSERT(num_worker_threads != 0);
        }

        virtual ~shared_priority_queue_scheduler() {}

        bool numa_sensitive() const override {
            return !numa_stealing_;
        }

        bool has_thread_stealing(std::size_t num_thread) const override {
            return core_stealing_;
        }

        static std::string get_scheduler_name()
        {
            return "shared_priority_queue_scheduler";
        }

        // ------------------------------------------------------------
        void abort_all_suspended_threads() override
        {
            // process all cores if -1 was sent in
            for (std::size_t d=0; d<num_domains_; ++d) {
                numa_holder_[d].abort_all_suspended_threads();
            }
        }

        // ------------------------------------------------------------
        bool cleanup_terminated(bool delete_all) override
        {
            // just cleanup the thread we were called by rather than all threads
//            LOG_CUSTOM_MSG("cleanup_terminated - global version");
            std::size_t thread_num =this->global_to_local_thread_index(get_worker_thread_num());
            std::size_t domain_num = d_lookup_[thread_num];
            std::size_t q_index    = q_lookup_[thread_num];
            return numa_holder_[domain_num].thread_queue(q_index)->
                    cleanup_terminated(delete_all);
        }

        // ------------------------------------------------------------
        bool cleanup_terminated(std::size_t thread_num, bool delete_all) override
        {
            HPX_ASSERT(thread_num ==
                this->global_to_local_thread_index(get_worker_thread_num()));

            // find the numa domain from the local thread index
            std::size_t domain_num = d_lookup_[thread_num];
            std::size_t q_index    = q_lookup_[thread_num];
            // cleanup the queues assigned to this thread
            return numa_holder_[domain_num].thread_queue(q_index)->
                    cleanup_terminated(delete_all);
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
                thread_num =
                    this->global_to_local_thread_index(get_worker_thread_num());
                if (thread_num>=num_workers_) {
                    // This is a task being injected from a thread on another pool.
                    // we can schedule on any thread available
                    thread_num = numa_holder_[0].thread_queue(0)->worker_next(num_workers_);
                }
                else if (work_policy_ == assign_work_round_robin) {
                    domain_num = d_lookup_[thread_num];
                    q_index    = q_lookup_[thread_num];
                    thread_num = numa_holder_[domain_num].thread_queue(q_index)->numa_next(num_workers_);
                }
                thread_num = select_active_pu(l, thread_num);
                domain_num = d_lookup_[thread_num];
                q_index    = q_lookup_[thread_num];
                numa_holder_[domain_num].thread_queue(q_index)->debug("create 0  ", q_index, 10+q_index, 10+q_index, nullptr);
                break;
            }
            case thread_schedule_hint_mode::thread_schedule_hint_mode_thread:
            {
                // @TODO. We should check that the thread num is valid
                // Create thread on requested worker thread
                thread_num = select_active_pu(l, data.schedulehint.hint);
                domain_num = d_lookup_[thread_num];
                q_index    = q_lookup_[thread_num];
                numa_holder_[domain_num].thread_queue(q_index)->debug("create t  ", q_index, 20+q_index, 20+q_index, nullptr);
                break;
            }
            case thread_schedule_hint_mode::thread_schedule_hint_mode_numa:
            {
                // Create thread on requested NUMA domain

                // TODO: This case does not handle suspended PUs.
                domain_num = fast_mod(data.schedulehint.hint, num_domains_);
                // if the thread creating the new task is on the domain
                // assigned to the new task - try to reuse the core as well
                thread_num =
                    this->global_to_local_thread_index(get_worker_thread_num());
                if (d_lookup_[thread_num] == domain_num) {
                    q_index = q_lookup_[thread_num];
                }
                else {
                    throw std::runtime_error("counter problem in thread scheduler");
                    q_index = numa_holder_[0].thread_queue(0)->worker_next(num_workers_);;
                }
                numa_holder_[domain_num].thread_queue(q_index)->debug("create n  ", q_index, 30+domain_num, 30+domain_num, nullptr);
                break;
            }
            default:
                HPX_THROW_EXCEPTION(bad_parameter,
                    "shared_priority_queue_scheduler::create_thread",
                    "Invalid schedule hint mode: " +
                    std::to_string(data.schedulehint.mode));
            }

            numa_holder_[domain_num].thread_queue(q_index)->
                    create_thread(data, thrd, initial_state, run_now, ec);
        }

        /// Return the next thread to be executed, return false if none available
        virtual bool get_next_thread(std::size_t thread_num, bool running,
            threads::thread_data*& thrd, bool /*enable_stealing*/) override
        {
            HPX_ASSERT(thread_num ==
                this->global_to_local_thread_index(get_worker_thread_num()));

            // find the numa domain from the local thread index
            std::size_t domain_num = d_lookup_[thread_num];
            std::size_t q_index    = q_lookup_[thread_num];

            bool result = false;
            for (std::size_t d=0; d<num_domains_; ++d) {
                std::size_t dom = fast_mod((domain_num+d), num_domains_);
                result = numa_holder_[dom].get_next_thread(q_index, thrd, core_stealing_);
                if (result) return result;
                // if no numa stealing - this thread should only check it's own numa
                if (!numa_stealing_) break;
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
                thread_num =
                    this->global_to_local_thread_index(get_worker_thread_num());
                if (thread_num>=num_workers_) {
                    // This is a task being injected from a thread on another pool.
                    // we can schedule on any thread available
                    thread_num = numa_holder_[0].thread_queue(0)->
                            worker_next(num_workers_);
                }
                else if (work_policy_ == assign_work_round_robin) {
                    domain_num = d_lookup_[thread_num];
                    q_index    = q_lookup_[thread_num];
                    thread_num = numa_holder_[domain_num].thread_queue(q_index)->
                            numa_next(num_workers_);
                }
                thread_num = select_active_pu(l, thread_num, allow_fallback);
                domain_num = d_lookup_[thread_num];
                q_index    = q_lookup_[thread_num];
                numa_holder_[domain_num].thread_queue(q_index)->debug("schedule 0", q_index, domain_num, domain_num, thrd);
                break;
            }
            case thread_schedule_hint_mode::thread_schedule_hint_mode_thread:
            {
                // @TODO. We should check that the thread num is valid
                // Create thread on requested worker thread
                LOG_CUSTOM_VAR(msg = msgs[3]);
                thread_num = select_active_pu(l, schedulehint.hint, allow_fallback);
                domain_num = d_lookup_[thread_num];
                q_index    = q_lookup_[thread_num];
                numa_holder_[domain_num].thread_queue(q_index)->debug("schedule t", q_index, domain_num, domain_num, thrd);
                break;
            }
            case thread_schedule_hint_mode::thread_schedule_hint_mode_numa:
            {
                // Create thread on requested NUMA domain
                LOG_CUSTOM_VAR(msg = msgs[1]);
                // TODO: This case does not handle suspended PUs.
                domain_num = fast_mod(schedulehint.mode, num_domains_);
                // if the thread creating the new task is on the domain
                // assigned to the new task - try to reuse the core as well
                thread_num =
                    this->global_to_local_thread_index(get_worker_thread_num());
                if (d_lookup_[thread_num] == domain_num) {
                    q_index = q_lookup_[thread_num];
                }
                else {
                    throw std::runtime_error("counter problem in thread scheduler");
                    q_index = numa_holder_[0].thread_queue(0)->worker_next(num_workers_);;
                }
                numa_holder_[domain_num].thread_queue(q_index)->debug("schedule n", q_index, domain_num, domain_num, thrd);
                break;
            }

            default:
                HPX_THROW_EXCEPTION(bad_parameter,
                    "shared_priority_queue_scheduler::schedule_thread",
                    "Invalid schedule hint mode: " +
                    std::to_string(schedulehint.mode));
            }

            numa_holder_[domain_num].thread_queue(q_index)->
                    schedule_thread(thrd, priority, false);
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
            LOG_CUSTOM_MSG("destroy_thread " << THREAD_DESC(thrd));
            thrd->get_queue<queue_holder_thread<thread_queue_mc<>>>().destroy_thread(thrd, busy_count);
        }

        //---------------------------------------------------------------------
        // This returns the current length of the queues
        // (work items and new items)
        //---------------------------------------------------------------------
        std::int64_t get_queue_length(
            std::size_t thread_num = std::size_t(-1)) const override
        {
            LOG_CUSTOM_MSG("get_queue_length"
                << " thread_num " << decnumber(thread_num));

            HPX_ASSERT(thread_num != std::size_t(-1));

            std::int64_t count = 0;
            if (thread_num != std::size_t(-1)) {
                std::size_t domain_num = d_lookup_[thread_num];
                std::size_t q_index    = q_lookup_[thread_num];
                count += numa_holder_[domain_num].thread_queue(q_index)->
                        get_queue_length();
            }
            else {
                throw std::runtime_error("unhandled get_queue_length with -1");
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

            if (thread_num != std::size_t(-1)) {
                std::size_t domain_num = d_lookup_[thread_num];
                std::size_t q_index    = q_lookup_[thread_num];
                return numa_holder_[domain_num].thread_queue(q_index)->
                        get_thread_count(state, priority);
            }
            else {
                std::int64_t count = 0;
                for (std::size_t d=0; d<num_domains_; ++d) {
                    count += numa_holder_[d].get_thread_count(state, priority);
                }
                return count;
            }
        }

        //---------------------------------------------------------------------
        // Enumerate matching threads from all queues
        bool enumerate_threads(
            util::function_nonser<bool(thread_id_type)> const& f,
            thread_state_enum state = unknown) const override
        {
            bool result = true;

            LOG_CUSTOM_MSG("enumerate_threads");

            for (std::size_t d=0; d<num_domains_; ++d) {
                result = numa_holder_[d].enumerate_threads(f, state) &&
                        result;
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
            HPX_ASSERT(thread_num ==
                this->global_to_local_thread_index(get_worker_thread_num()));

            added = 0;

            // process this thread only if specified
            if (thread_num!=std::size_t(-1)) {
                // find the numa domain from the local thread index
                std::size_t domain_num = d_lookup_[thread_num];
                std::size_t q_index    = q_lookup_[thread_num];
                return numa_holder_[domain_num].thread_queue(q_index)->
                    wait_or_add_new(running, idle_loop_count, added, core_stealing_);
            }

            // find the numa domain from the local thread index
            std::size_t domain_num = d_lookup_[thread_num];
            std::size_t q_index    = q_lookup_[thread_num];
            bool result = false;
            // process all cores if -1 was sent in
            for (std::size_t d=0; d<num_domains_; ++d) {
                std::size_t dom = fast_mod((domain_num+d), num_domains_);
                // get next task, steal if from another domain
                result = numa_holder_[dom].thread_queue(q_index)->wait_or_add_new(running,
                    idle_loop_count, added, core_stealing_);
                if (0 != added) return result;
                if (!numa_stealing_) break;
            }
            return result;
        }

        ///////////////////////////////////////////////////////////////////////
        void on_start_thread(std::size_t thread_num) override
        {
            // the main initialization can be done by any one thread
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

                for (std::size_t local_id=0; local_id!=num_workers_; ++local_id)
                {
                    std::size_t global_id = local_to_global_thread_index(local_id);
                    std::size_t pu_num = rp.get_pu_num(global_id);
                    std::size_t domain = topo.get_numa_node_number(pu_num);
                    d_lookup_[local_id] = domain;
                    num_domains_ = (std::max)(num_domains_, domain+1);
                }

                HPX_ASSERT(num_domains_ <= HPX_HAVE_MAX_NUMA_DOMAIN_COUNT);

                // if we have zero cores on a numa domain, then reindex the domains to be
                // sequential otherwise it messes up counting as an indexing operation
                {
                    std::vector<std::size_t> d_inx(d_lookup_.begin(), d_lookup_.end());
                    // reduce list of all used domains to simple unique sort list
                    std::sort(d_inx.begin(), d_inx.end(), std::less<std::size_t>());
                    auto last = std::unique(d_inx.begin(), d_inx.end());
                    d_inx.erase(last, d_inx.end());
                    num_domains_ = d_inx.size();
                    // turn list into a map
                    std::map<std::size_t, std::size_t> domain_map;
                    std::size_t index = 0;
                    for (auto d : d_inx) domain_map.emplace(d, index++);
                    // replace old domain number with new one
                    for (std::size_t d=0; d<d_lookup_.size(); ++d) {
                        d_lookup_[d] = domain_map[d_lookup_[d]];
                    }
                }

                // count cores per domain and assign queues accordingly
                for (std::size_t local_id=0; local_id<num_workers_; ++local_id)
                {
                    q_lookup_[local_id] = q_counts_[d_lookup_[local_id]]++;
                }

                // init the numa_holder for each domain
                for (std::size_t dom=0; dom<num_domains_; ++dom)
                {
                    // init with {cores, queues} on this domain
                    numa_holder_[dom].init(q_counts_[dom]);
                }


                debug::output("d_lookup_  ", &d_lookup_[0],  &d_lookup_[num_workers_]);
                debug::output("q_lookup_  ", &q_lookup_[0],  &q_lookup_[num_workers_]);
                debug::output("q_counts_  ", &q_counts_[0],  &q_counts_[num_domains_]);
            }

            // all threads should now complete their initialization by creating
            // the queues that are local to their own thread
            lock.unlock();

            // ------------------------------------
            // if cores_per_queue > 1 then some queues are shared between threads,
            // one thread will be the 'owner' of the queue.
            // allow one thread at a time to enter this section in incrementing
            // thread number ordering so that we can init queues and assign them
            // with the guarantee that references to threads with lower ids are valid.
            // ------------------------------------
            static volatile std::size_t thread_counter = 0;
            while (thread_num>thread_counter) {
                // std::thread because we can't suspend HPX threads until initialized
                std::this_thread::yield();
            }

            // this is the index of out thread in the local numa domain
            int local_id   = q_lookup_[thread_num];
            int domain_num = d_lookup_[thread_num];

            // queue pointers we will assign to each thread
            thread_queue_type *hp_queue = nullptr;
            thread_queue_type *np_queue = nullptr;
            thread_queue_type *lp_queue = nullptr;

            std::int16_t owner_mask = 0;
            // does not need to be atomic because we serialize thread access
            static std::tuple<int, int, int> q_counters = {0,0,0};

            // High priority
            if (cores_per_queue_.high_priority>0) {
                if (local_id % cores_per_queue_.high_priority == 0) {
                    // if we will own the queue, create it
                    hp_queue = new thread_queue_type(local_id);
                    std::get<0>(q_counters)++;
                    owner_mask |= 1;
                }
                else {
                    // share the queue with our next lowest thread num neighbour
                    hp_queue = numa_holder_[domain_num].thread_queue(local_id-1)->hp_queue_;
                }
            }

            // Normal priority
            if (local_id % cores_per_queue_.normal_priority == 0) {
                // if we will own the queue, create it
                np_queue = new thread_queue_type(local_id);
                std::get<1>(q_counters)++;
                owner_mask |= 2;
            }
            else {
                // share the queue with our next lowest thread num neighbour
                np_queue = numa_holder_[domain_num].thread_queue(local_id-1)->np_queue_;
            }

            // Low priority
            if (cores_per_queue_.low_priority>0) {
                if (local_id % cores_per_queue_.low_priority == 0) {
                    // if we will own the queue, create it
                    lp_queue = new thread_queue_type(local_id);
                    std::get<2>(q_counters)++;
                    owner_mask |= 4;
                }
                else {
                    // share the queue with our next lowest thread num neighbour
                    lp_queue = numa_holder_[domain_num].thread_queue(local_id-1)->lp_queue_;
                }
            }

            queue_holder_thread<thread_queue_mc<>> *thread_holder =
                    new queue_holder_thread<thread_queue_mc<>>(hp_queue, np_queue, lp_queue, owner_mask, thread_counter);

            numa_holder_[domain_num].queues_[local_id] = thread_holder;

            // we can now increment the thread counter and allow the next thread to init
            thread_counter++;

            // we do not want to allow threads to start stealing from others
            // until all threads have initialized their structures.
            // We therefore block at this point until all threads are here.
            while (thread_counter < num_workers_) {
                // std::thread because we can't suspend HPX threads until initialized
                std::this_thread::yield();
            }

            // last thread can dump debug info
            if (thread_num==(thread_counter-1)) {
                //debug::output("q_counters ", &q_counters[0],  &q_counters[num_domains_]);
                std::cout << "q_counters " << "\t : {"
                          << decnumber(sizeof(q_counters)/sizeof(int)) << "} : ";
                std::cout << std::get<0>(q_counters) << ", "
                          << std::get<1>(q_counters) << ", "
                          << std::get<2>(q_counters) << std::endl;
            }

        }

        void on_stop_thread(std::size_t thread_num) override
        {
            if (thread_num>num_workers_) {
                HPX_THROW_EXCEPTION(bad_parameter,
                    "shared_priority_queue_scheduler::on_stop_thread",
                    "Invalid thread number: " + std::to_string(thread_num));
            }
            // @TODO Do we need to do any queue related cleanup here?
        }

        void on_error(
            std::size_t thread_num, std::exception_ptr const& e) override
        {
            if (thread_num>num_workers_) {
                HPX_THROW_EXCEPTION(bad_parameter,
                    "shared_priority_queue_scheduler::on_error",
                    "Invalid thread number: " + std::to_string(thread_num));
            }
            // @TODO Do we need to do any queue related cleanup here?
        }

#ifdef HPX_HAVE_THREAD_STEALING_COUNTS
        std::int64_t get_num_pending_misses(std::size_t num, bool reset)
            override { return 0; }

        std::int64_t get_num_pending_accesses(std::size_t num, bool reset)
            override { return 0; }

        std::int64_t get_num_stolen_from_pending(std::size_t num, bool reset)
            override { return 0; }

        std::int64_t get_num_stolen_to_pending(std::size_t num, bool reset)
            override { return 0; }

        std::int64_t get_num_stolen_from_staged(std::size_t num, bool reset)
            override { return 0; }

        std::int64_t get_num_stolen_to_staged(std::size_t num, bool reset)
            override { return 0; }
#endif

    protected:
        typedef queue_holder_numa<thread_queue_type> numa_queues;

        // one item per numa domain of a container for queues on that domain
        std::array<numa_queues, HPX_HAVE_MAX_NUMA_DOMAIN_COUNT> numa_holder_;

        // lookup domain from local worker index
        std::array<std::size_t, HPX_HAVE_MAX_CPU_COUNT> d_lookup_;
        std::array<std::size_t, HPX_HAVE_MAX_CPU_COUNT> q_lookup_;

        // index of queue on domain from local worker index
        std::array<std::size_t, HPX_HAVE_MAX_CPU_COUNT> hp_lookup_;
        std::array<std::size_t, HPX_HAVE_MAX_CPU_COUNT> np_lookup_;
        std::array<std::size_t, HPX_HAVE_MAX_CPU_COUNT> lp_lookup_;

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
