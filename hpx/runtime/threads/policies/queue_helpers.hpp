//  Copyright (c) 2017-2018 John Biddiscombe
//  Copyright (c) 2007-2016 Hartmut Kaiser
//  Copyright (c)      2011 Bryce Lelbach
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_THREADMANAGER_SCHEDULING_QUEUE_HELPER)
#define HPX_THREADMANAGER_SCHEDULING_QUEUE_HELPER

#include <hpx/config.hpp>
#include <hpx/logging.hpp>
#include <hpx/runtime/threads/policies/thread_queue_init_parameters.hpp>
#include <hpx/runtime/threads/thread_data.hpp>
#include <hpx/type_support/unused.hpp>
#include <hpx/runtime/threads/policies/thread_queue_mc.hpp>
#include <hpx/runtime/threads/policies/lockfree_queue_backends.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <vector>
#include <unordered_set>
#include <list>
#include <map>

#include <atomic>
#include <mutex>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#define LOG_CUSTOM_MSG(x)

// ------------------------------------------------------------////////
namespace hpx { namespace threads { namespace policies
{
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

    // apply the modulo operator only when needed
    // (i.e. when the input is greater than the ceiling)
    // NB: the numbers must be positive
    HPX_FORCEINLINE int fast_mod(const unsigned int input, const unsigned int ceil) {
        return input >= ceil ? input % ceil : input;
    }

    enum : std::size_t { max_thread_count = 1000 };

    // ----------------------------------------------------------------
    // Helper class to hold a set of queues.
    // ----------------------------------------------------------------
    template <typename QueueType>
    struct queue_holder
    {
        queue_holder()
            : thread_map_count_(0),
              terminated_items_(max_thread_count),
              terminated_items_count_(0)
        {
        }

        // ----------------------------------------------------------------
        void debug(const char *txt, int new_tasks, int work_items, threads::thread_data* thrd)
        {
            LOG_CUSTOM_MSG(txt
                           << " new " << dec4(new_tasks)
                           << " work " << dec4(work_items)
                           << " map " << dec4(thread_map_count_)
                           << " terminated " << dec4(terminated_items_count_)
                           << THREAD_DESC(thrd)
                           );
        }

        void init(std::size_t cores, std::size_t queues,
            const thread_queue_init_parameters &thread_queue_init)
        {
            parameters_ = thread_queue_init;
            num_cores   = cores;
            num_queues  = queues;
            scale       = num_cores==1 || queues==0 ? 0
                         : static_cast<double>(num_queues-1)/(num_cores-1);
            //
            queues_.resize(num_queues);
            for (std::size_t i = 0; i < num_queues; ++i) {
                queues_[i] = new QueueType(this, i, thread_queue_init);
            }
        }

        // ----------------------------------------------------------------
        ~queue_holder()
        {
            for(auto t: thread_heap_small_)
                deallocate(t.get());

            for(auto t: thread_heap_medium_)
                deallocate(t.get());

            for(auto t: thread_heap_large_)
                deallocate(t.get());

            for(auto t: thread_heap_huge_)
                deallocate(t.get());

            for(auto &q : queues_) delete q;
            queues_.clear();
        }

        // ----------------------------------------------------------------
        inline std::size_t size() const {
            return num_queues;
        }

        // ----------------------------------------------------------------
        inline std::size_t get_queue_index(std::size_t id) const
        {
            return std::lround(id * scale);
        }

        // ----------------------------------------------------------------
        inline bool get_next_thread(std::size_t qidx,
            threads::thread_data*& thrd, bool steal)
        {
            // loop over queues and take one task,
            // starting with the requested queue
            for (std::size_t i=0; i<num_queues; ++i) {
                std::size_t q = fast_mod((qidx + i), num_queues);
                // we we got a thread, return it
                if (queues_[q]->get_next_thread(thrd)) return true;
                // if we're not stealing, then do not check other queues
                if (!steal) return false;
            }
            return false;
        }

        // ----------------------------------------------------------------
        inline bool wait_or_add_new(std::size_t id, bool running,
           std::size_t& added, bool steal)
        {
            // loop over all queues and take one task,
            // starting with the requested queue
            // then stealing from any other one in the container
            bool result = true;
            for (std::size_t i=0; i<num_queues; ++i) {
                std::size_t q = fast_mod((id + i), num_queues);
                result = queues_[q]->wait_or_add_new(running,
                    added, steal) && result;
                if (0 != added) {
                    return result;
                }
                if (!steal) break;
            }
            return result;
        }

        // ----------------------------------------------------------------
        inline std::size_t get_new_tasks_queue_length() const
        {
            std::size_t len = 0;
            for (auto &q : queues_) len += q->get_staged_queue_length();
            return len;
        }

        // ----------------------------------------------------------------
        inline std::size_t get_queue_length() const
        {
            std::size_t len = 0;
            for (auto &q : queues_) len += q->get_queue_length();
            return len;
        }

        // ----------------------------------------------------------------
        inline std::size_t get_thread_count(thread_state_enum state = unknown) const
        {
//            std::size_t len = 0;
//            for (auto &q : queues_) len += q->get_thread_count(state);

            if (terminated == state)
                return terminated_items_count_;

            if (staged == state)
                return get_new_tasks_queue_length();

            if (unknown == state)
                return thread_map_count_ + get_new_tasks_queue_length() - terminated_items_count_;

            // acquire lock only if absolutely necessary
            std::lock_guard<mutex_type> lk(mtx_);

            std::int64_t num_threads = 0;
            thread_map_type::const_iterator end = thread_map_.end();
            for (thread_map_type::const_iterator it = thread_map_.begin();
                 it != end; ++it)
            {
                if ((*it)->get_state().state() == state)
                    ++num_threads;
            }
            return num_threads;
            //return len;
        }

        // ----------------------------------------------------------------

        // ----------------------------------------------------------------

        // ------------------------------------------------------------
//        inline bool cleanup_terminated(bool delete_all)
//        {
//            bool empty = true;
//            for (auto &q : queues_)
//                 empty = empty && q->cleanup_terminated(delete_all);
//            return empty;
//        }

        // ----------------------------------------------------------------
        // ----------------------------------------------------------------
        // ----------------------------------------------------------------
        thread_queue_init_parameters parameters_;
        std::size_t num_cores;
        std::size_t num_queues;
        double scale;
        std::vector<QueueType*> queues_;

        // ----------------------------------------------------------------
        typedef std::mutex mutex_type ;

        using thread_heap_type =
            std::list<thread_id_type, util::internal_allocator<thread_id_type>>;

        thread_heap_type thread_heap_small_;
        thread_heap_type thread_heap_medium_;
        thread_heap_type thread_heap_large_;
        thread_heap_type thread_heap_huge_;

        // mutex protecting the members
        mutable mutex_type mtx_;

        static util::internal_allocator<threads::thread_data> thread_alloc_;

        typedef util::tuple<thread_init_data, thread_state_enum> task_description;

        // -------------------------------------
        // thread map stores every task in this queue set
        // this is the type of a map holding all threads (except depleted/terminated)
        using thread_map_type = std::unordered_set<thread_id_type,
            std::hash<thread_id_type>, std::equal_to<thread_id_type>,
            util::internal_allocator<thread_id_type>>;
        thread_map_type             thread_map_;
        std::atomic<std::int64_t>   thread_map_count_;

        // -------------------------------------
        // staged tasks
        // tasks that have been created, but are not yet runnable/active
//        using task_items_type = lockfree_fifo::apply<task_description>::type;
//        task_items_type             new_tasks_;
//        std::atomic<std::int64_t>   new_tasks_count_;

//        // -------------------------------------
//        // pending : tasks that are ready to run / active
//        using work_items_type = lockfree_fifo::apply<thread_data*>::type;
//        work_items_type             work_items_;
//        std::atomic<std::int64_t>   work_items_count_;

        // -------------------------------------
        // terminated tasks
        // completed tasks that can be reused (stack space etc)
        using terminated_items_type = lockfree_fifo::apply<thread_data*>::type;
        terminated_items_type       terminated_items_;
        std::atomic<std::int64_t>   terminated_items_count_;

        // ----------------------------------------------------------------
        // ----------------------------------------------------------------
        // ----------------------------------------------------------------
        void add_to_thread_map(
                threads::thread_id_type thrd,
                std::unique_lock<mutex_type> &lk)
        {
            HPX_ASSERT(lk.owns_lock());

            // add a new entry in the map for this thread
            std::pair<thread_map_type::iterator, bool> p =
                thread_map_.insert(thrd);

            if (/*HPX_UNLIKELY*/(!p.second)) {
                lk.unlock();
                HPX_THROW_EXCEPTION(hpx::out_of_memory,
                    "queue_helper::add_to_thread_map",
                    "Couldn't add new thread to the thread map");
            }

            ++thread_map_count_;

            // this thread has to be in the map now
            HPX_ASSERT(thread_map_.find(thrd)!=thread_map_.end());
            HPX_ASSERT(&thrd->get_queue<queue_holder>() == this);
        }

        void remove_from_thread_map(
                threads::thread_id_type thrd,
                bool dealloc)
        {
            // this thread has to be in this map
            HPX_ASSERT(thread_map_.find(thrd)  !=  thread_map_.end());
            HPX_ASSERT(thread_map_count_ >= 0);

            bool deleted = thread_map_.erase(thrd) != 0;
            HPX_ASSERT(deleted);
            if (dealloc) {
                deallocate(thrd.get());
            }
            --thread_map_count_;
        }

        template <typename Lock>
        void create_thread_object(threads::thread_id_type& thrd,
            threads::thread_init_data& data, thread_state_enum state, Lock& lk)
        {
            HPX_ASSERT(lk.owns_lock());
            HPX_ASSERT(data.stacksize != 0);

            std::ptrdiff_t stacksize = data.stacksize;

            thread_heap_type* heap = nullptr;

            if (stacksize == get_stack_size(thread_stacksize_small))
            {
                heap = &thread_heap_small_;
            }
            else if (stacksize == get_stack_size(thread_stacksize_medium))
            {
                heap = &thread_heap_medium_;
            }
            else if (stacksize == get_stack_size(thread_stacksize_large))
            {
                heap = &thread_heap_large_;
            }
            else if (stacksize == get_stack_size(thread_stacksize_huge))
            {
                heap = &thread_heap_huge_;
            }
            else {
                switch(stacksize) {
                case thread_stacksize_small:
                    heap = &thread_heap_small_;
                    break;

                case thread_stacksize_medium:
                    heap = &thread_heap_medium_;
                    break;

                case thread_stacksize_large:
                    heap = &thread_heap_large_;
                    break;

                case thread_stacksize_huge:
                    heap = &thread_heap_huge_;
                    break;

                default:
                    break;
                }
            }
            HPX_ASSERT(heap);

            if (state == pending_do_not_schedule || state == pending_boost)
            {
                state = pending;
            }

            // Check for an unused thread object.
            if (!heap->empty())
            {
                // Take ownership of the thread object and rebind it.
                thrd = heap->front();
                heap->pop_front();
                thrd->rebind(data, state);
            }
            else
            {
                hpx::util::unlock_guard<Lock> ull(lk);

                // Allocate a new thread object.
                threads::thread_data* p = thread_alloc_.allocate(1);
                new (p) threads::thread_data(data, this, state);
                thrd = thread_id_type(p);
            }
        }

        void recycle_thread(thread_id_type thrd)
        {
            std::ptrdiff_t stacksize = thrd->get_stack_size();

            if (stacksize == get_stack_size(thread_stacksize_small))
            {
                thread_heap_small_.push_front(thrd);
            }
            else if (stacksize == get_stack_size(thread_stacksize_medium))
            {
                thread_heap_medium_.push_front(thrd);
            }
            else if (stacksize == get_stack_size(thread_stacksize_large))
            {
                thread_heap_large_.push_front(thrd);
            }
            else if (stacksize == get_stack_size(thread_stacksize_huge))
            {
                thread_heap_huge_.push_front(thrd);
            }
            else
            {
                switch(stacksize) {
                case thread_stacksize_small:
                    thread_heap_small_.push_front(thrd);
                    break;

                case thread_stacksize_medium:
                    thread_heap_medium_.push_front(thrd);
                    break;

                case thread_stacksize_large:
                    thread_heap_large_.push_front(thrd);
                    break;

                case thread_stacksize_huge:
                    thread_heap_huge_.push_front(thrd);
                    break;

                default:
                    HPX_ASSERT(false);
                    break;
                }
            }
        }

        static void deallocate(threads::thread_data* p)
        {
            using threads::thread_data;
            p->~thread_data();
            thread_alloc_.deallocate(p, 1);
        }

        bool cleanup_terminated_locked(bool delete_all = false)
        {
            if (terminated_items_count_ == 0)
                return true;

            if (delete_all) {
                // delete all threads
                thread_data* todelete;
                while (terminated_items_.pop(todelete))
                {
                    thread_id_type tid(todelete);
                    --terminated_items_count_;
                    remove_from_thread_map(tid, true);
                    debug("deallocate",
                          queues_[0]->get_staged_queue_length(),  // new_tasks_count_
                          queues_[0]->get_pending_queue_length(), // work_items_count_
                          nullptr);
                }
            }
            else {
                // delete only this many threads
                std::int64_t delete_count =
                    (std::max)(
                        static_cast<std::int64_t>(terminated_items_count_ / 10),
                        static_cast<std::int64_t>(parameters_.max_delete_count_));

                thread_data* todelete;
                while (delete_count && terminated_items_.pop(todelete))
                {
                    thread_id_type tid(todelete);
                    --terminated_items_count_;
                    remove_from_thread_map(tid, false);
                    recycle_thread(tid);
                    debug("recycle   ",
                          queues_[0]->get_staged_queue_length(),  // new_tasks_count_
                          queues_[0]->get_pending_queue_length(), // work_items_count_
                          todelete);

                    --delete_count;

                }
            }
            return terminated_items_count_ == 0;
        }

    public:
        bool cleanup_terminated(bool delete_all = false)
        {
            if (terminated_items_count_ == 0)
                return true;

            if (delete_all) {
                // do not lock mutex while deleting all threads, do it piece-wise
                while (true)
                {
                    std::lock_guard<mutex_type> lk(mtx_);
                    if (cleanup_terminated_locked(false))
                    {
                        return true;
                    }
                }
                return false;
            }

            std::lock_guard<mutex_type> lk(mtx_);
            return cleanup_terminated_locked(false);
        }

//        // ------------------------------------------------------------
//        // This returns the current length of the pending queue
//        std::int64_t get_pending_queue_length() const
//        {
//            return work_items_count_;
//        }

//        // This returns the current length of the staged queue
//        std::int64_t get_staged_queue_length(
//            std::memory_order order = std::memory_order_seq_cst) const
//        {
//            return new_tasks_count_.load(order);
//        }

        void increment_num_pending_misses(std::size_t num = 1) {}
        void increment_num_pending_accesses(std::size_t num = 1) {}
        void increment_num_stolen_from_pending(std::size_t num = 1) {}
        void increment_num_stolen_from_staged(std::size_t num = 1) {}
        void increment_num_stolen_to_pending(std::size_t num = 1) {}
        void increment_num_stolen_to_staged(std::size_t num = 1) {}

        // ------------------------------------------------------------
        // create a new thread and schedule it if the initial state is equal to
        // pending. This is called when a function such as async() or .then()
        // triggers the creation of a new task that may - or may not - be
        // ready to run yet
        // ------------------------------------------------------------
//        void create_thread(thread_init_data& data, thread_id_type* id,
//            thread_state_enum initial_state, bool run_now, error_code& ec)
//        {
//            // thread has not been created yet
//            if (id) *id = invalid_thread_id;

//            // if ready to run, it goes directly to a work queue
//            // if not, it goes onto a staged task queue
//            if (run_now)
//            {
//                threads::thread_id_type thrd;

//                // The mutex can not be locked while a new thread is getting
//                // created, as it might have that the current HPX thread gets
//                // suspended.
//                {
//                    std::unique_lock<mutex_type> lk(mtx_);
//                    create_thread_object(thrd, data, initial_state, lk);

//                    add_to_thread_map(thrd, lk);

//                    // push the new thread in the pending queue thread
//                    if (initial_state == pending)
//                        schedule_thread(thrd.get());

//                    // return the thread_id of the newly created thread
//                    if (id) *id = thrd;

//                    if (&ec != &throws)
//                        ec = make_success_code();
//                    return;
//                }
//            }

//            // do not execute the work, but register a task description for
//            // later thread creation
//            ++new_tasks_count_;

//            new_tasks_.push(task_description(std::move(data), initial_state)); //-V106
//            if (&ec != &throws)
//                ec = make_success_code();
//        }

        /// Destroy the passed thread as it has been terminated
        void destroy_thread(threads::thread_data* thrd, std::int64_t& busy_count)
        {
            HPX_ASSERT(&thrd->get_queue<queue_holder>() == this);
            terminated_items_.push(thrd);
            std::int64_t count = ++terminated_items_count_;
            if (count > parameters_.max_terminated_threads_)
            {
                cleanup_terminated(true);   // clean up all terminated threads
            }
            debug("destroy   ",
                  queues_[0]->new_tasks_count_.data_,
                  queues_[0]->work_items_count_.data_,
                  thrd);
        }

        // ------------------------------------------------------------
        void abort_all_suspended_threads()
        {
            throw std::runtime_error("This function needs to be reimplemented");
            std::lock_guard<mutex_type> lk(mtx_);
            thread_map_type::iterator end =  thread_map_.end();
            for (thread_map_type::iterator it = thread_map_.begin();
                 it != end; ++it)
            {
                if ((*it)->get_state().state() == suspended)
                {
//                    (*it)->set_state(pending, wait_abort);
//                    schedule_thread((*it).get());
                }
            }
        }

        bool enumerate_threads(
            util::function_nonser<bool(thread_id_type)> const& f,
            thread_state_enum state = unknown) const
        {
            std::uint64_t count = thread_map_count_;
            if (state == terminated)
            {
                count = terminated_items_count_;
            }
            else if (state == staged)
            {
                HPX_THROW_EXCEPTION(bad_parameter,
                    "queue_holder::iterate_threads",
                    "can't iterate over thread ids of staged threads");
                return false;
            }

            std::vector<thread_id_type> ids;
            ids.reserve(static_cast<std::size_t>(count));

            if (state == unknown)
            {
                std::lock_guard<mutex_type> lk(mtx_);
                thread_map_type::const_iterator end =  thread_map_.end();
                for (thread_map_type::const_iterator it = thread_map_.begin();
                     it != end; ++it)
                {
                    ids.push_back(*it);
                }
            }
            else
            {
                std::lock_guard<mutex_type> lk(mtx_);
                thread_map_type::const_iterator end =  thread_map_.end();
                for (thread_map_type::const_iterator it = thread_map_.begin();
                     it != end; ++it)
                {
                    if ((*it)->get_state().state() == state)
                        ids.push_back(*it);
                }
            }

            // now invoke callback function for all matching threads
            for (thread_id_type const& id : ids)
            {
                if (!f(id))
                    return false;       // stop iteration
            }

            return true;
        }

        // ------------------------------------------------------------
        void on_start_thread(std::size_t num_thread) {}
        void on_stop_thread(std::size_t num_thread) {}
        void on_error(std::size_t num_thread, std::exception_ptr const& e) {}
    };


#ifdef HPX_HAVE_THREAD_MINIMAL_DEADLOCK_DETECTION
    ///////////////////////////////////////////////////////////////////////////
    // We globally control whether to do minimal deadlock detection using this
    // global bool variable. It will be set once by the runtime configuration
    // startup code
    extern bool minimal_deadlock_detection;
#endif

        ///////////////////////////////////////////////////////////////////////////
        // debug helper function, logs all suspended threads
        // this returns true if all threads in the map are currently suspended
namespace detail {
    template <typename Map>
        bool dump_suspended_threads(std::size_t num_thread, Map& tm,
            std::int64_t& idle_loop_count, bool running) HPX_COLD;

        template <typename Map>
        bool dump_suspended_threads(std::size_t num_thread, Map& tm,
            std::int64_t& idle_loop_count, bool running)
    {
#ifndef HPX_HAVE_THREAD_MINIMAL_DEADLOCK_DETECTION
            HPX_UNUSED(tm);
            HPX_UNUSED(idle_loop_count);
            HPX_UNUSED(running);    //-V601
            return false;
#else
            if (!minimal_deadlock_detection)
                return false;

            // attempt to output possibly deadlocked threads occasionally only
            if (HPX_LIKELY((idle_loop_count++ % HPX_IDLE_LOOP_COUNT_MAX) != 0))
                return false;

            bool result = false;
            bool collect_suspended = true;

            bool logged_headline = false;
            typename Map::const_iterator end = tm.end();
            for (typename Map::const_iterator it = tm.begin(); it != end; ++it)
            {
                threads::thread_data const* thrd = it->get();
                threads::thread_state_enum state = thrd->get_state().state();
            threads::thread_state_enum marked_state = thrd->get_marked_state();

            if (state != marked_state) {
                    // log each thread only once
                if (!logged_headline) {
                    if (running) {
                            LTM_(error)    //-V128
                                << "Listing suspended threads while queue ("
                                << num_thread << ") is empty:";
                        }
                    else {
                        LHPX_CONSOLE_(hpx::util::logging::level::error) //-V128
                            << "  [TM] Listing suspended threads while queue ("
                                << num_thread << ") is empty:\n";
                        }
                        logged_headline = true;
                    }

                if (running) {
                    LTM_(error) << "queue(" << num_thread << "): " //-V128
                                << get_thread_state_name(state)
                                << "(" << std::hex << std::setw(8)
                                    << std::setfill('0') << (*it)
                                << "." << std::hex << std::setw(2)
                                    << std::setfill('0') << thrd->get_thread_phase()
                                << "/" << std::hex << std::setw(8)
                                    << std::setfill('0') << thrd->get_component_id()
                                << ")"
#ifdef HPX_HAVE_THREAD_PARENT_REFERENCE
                            << " P" << std::hex << std::setw(8)
                            << std::setfill('0') << thrd->get_parent_thread_id()
#endif
                                << ": " << thrd->get_description()
                                << ": " << thrd->get_lco_description();
                    }
                else {
                    LHPX_CONSOLE_(hpx::util::logging::level::error) << "  [TM] " //-V128
                                << "queue(" << num_thread << "): "
                                << get_thread_state_name(state)
                                << "(" << std::hex << std::setw(8)
                                    << std::setfill('0') << (*it)
                                << "." << std::hex << std::setw(2)
                            << std::setfill('0') << thrd->get_thread_phase()
                            << "/" << std::hex << std::setw(8)
                            << std::setfill('0') << thrd->get_component_id()
                            << ")"
#ifdef HPX_HAVE_THREAD_PARENT_REFERENCE
                            << " P" << std::hex << std::setw(8)
                            << std::setfill('0') << thrd->get_parent_thread_id()
#endif
                                << ": " << thrd->get_description()
                                << ": " << thrd->get_lco_description() << "\n";
                    }
                    thrd->set_marked_state(state);

                    // result should be true if we found only suspended threads
                if (collect_suspended) {
                    switch(state) {
                        case threads::suspended:
                            result = true;    // at least one is suspended
                            break;

                        case threads::pending:
                        case threads::active:
                        result = false;   // one is active, no deadlock (yet)
                            collect_suspended = false;
                            break;

                        default:
                            // If the thread is terminated we don't care too much
                            // anymore.
                            break;
                        }
                    }
                }
            }
            return result;
#endif
    }
}
    template <typename QueueType>
    util::internal_allocator<threads::thread_data>
        queue_holder<QueueType>::thread_alloc_;

}}}    // namespace hpx::threads::policies

#endif    // HPX_F0153C92_99B1_4F31_8FA9_4208DB2F26CE
