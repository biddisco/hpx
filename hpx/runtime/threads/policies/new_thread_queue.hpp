//  Copyright (c) 2007-2019 Hartmut Kaiser
//  Copyright (c) 2011      Bryce Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_THREADMANAGER_new_thread_queue_AUG_25_2009_0132PM)
#define HPX_THREADMANAGER_new_thread_queue_AUG_25_2009_0132PM

#include <hpx/config.hpp>
#include <hpx/compat/mutex.hpp>
#include <hpx/error_code.hpp>
#include <hpx/runtime/config_entry.hpp>
#include <hpx/runtime/threads/policies/lockfree_queue_backends.hpp>
#include <hpx/runtime/threads/policies/queue_helpers.hpp>
#include <hpx/runtime/threads/thread_data.hpp>
#include <hpx/throw_exception.hpp>
#include <hpx/util/assert.hpp>
#include <hpx/util/block_profiler.hpp>
#include <hpx/util/function.hpp>
#include <hpx/util/get_and_reset_value.hpp>
#include <hpx/util/high_resolution_clock.hpp>
#include <hpx/util/internal_allocator.hpp>
#include <hpx/util/unlock_guard.hpp>

#include <hpx/concurrent/junction/junction/ConcurrentMap_Leapfrog.h>
#include <boost/lockfree/stack.hpp>
#include <boost/lexical_cast.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#define HPX_HAS_THREAD_MAP

template <>
struct turf::util::BestFit<hpx::threads::thread_id_type> {
    typedef u64 Unsigned;
    typedef s64 Signed;
};

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace threads { namespace policies
{
#ifdef HPX_HAVE_THREAD_MINIMAL_DEADLOCK_DETECTION
    ///////////////////////////////////////////////////////////////////////////
    // We globally control whether to do minimal deadlock detection using this
    // global bool variable. It will be set once by the runtime configuration
    // startup code
    extern bool minimal_deadlock_detection;
#endif

    ///////////////////////////////////////////////////////////////////////////
    // // Queue back-end interface:
    //
    // template <typename T>
    // struct queue_backend
    // {
    //     typedef ... container_type;
    //     typedef ... value_type;
    //     typedef ... reference;
    //     typedef ... const_reference;
    //     typedef ... size_type;
    //
    //     queue_backend(
    //         size_type initial_size = ...
    //       , size_type num_thread = ...
    //         );
    //
    //     bool push(const_reference val);
    //
    //     bool pop(reference val, bool steal = true);
    //
    //     bool empty();
    // };
    //
    // struct queue_policy
    // {
    //     template <typename T>
    //     struct apply
    //     {
    //         typedef ... type;
    //     };
    // };
    template <
        typename StagedQueuing = lockfree_lifo,
        typename TerminatedQueuing = lockfree_fifo>
    class new_thread_queue
    {
    private:
        // don't steal if less than this amount of tasks are left
        int const min_tasks_to_steal_pending;
        int const min_tasks_to_steal_staged;

        // create at least this amount of threads from tasks
        int const min_add_new_count;

        // create not more than this amount of threads from tasks
        int const max_add_new_count;

        // number of terminated threads to discard
        int const max_delete_count;

        // number of terminated threads to collect before cleaning them up
        int const max_terminated_threads;

        // this is the type of a map holding all threads (except depleted ones)
        using thread_map_type = junction::ConcurrentMap_Leapfrog<
            std::uintptr_t, thread_data*>;
        using thread_map_iterator = thread_map_type::Iterator;

//        using thread_map_type = std::unordered_set<thread_id_type,
//            std::hash<thread_id_type>, std::equal_to<thread_id_type>,
//            util::internal_allocator<thread_id_type>>;

        using thread_heap_type =
            boost::lockfree::stack<thread_id_type, util::internal_allocator<thread_id_type>>;

        typedef util::tuple<thread_init_data, thread_state_enum> task_description;

        typedef thread_data thread_description;

        typedef typename StagedQueuing::template
            apply<thread_description*>::type work_items_type;

        typedef typename StagedQueuing::template
            apply<task_description*>::type task_items_type;

        typedef typename TerminatedQueuing::template
            apply<thread_data*>::type terminated_items_type;

    protected:
        //template <typename Lock>
        void create_thread_object(threads::thread_id_type& thrd,
            threads::thread_init_data& data, thread_state_enum state/*, Lock& lk*/)
        {
//            HPX_ASSERT(lk.owns_lock());
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
                heap->pop(thrd);
                thrd->rebind(data, state);
            }
            else
            {
//                hpx::util::unlock_guard<Lock> ull(lk);

                // Allocate a new thread object.
                threads::thread_data* p = thread_alloc_.allocate(1);
                new (p) threads::thread_data(data, this, state);
                thrd = thread_id_type(p);
            }
        }

        static util::internal_allocator<threads::thread_data> thread_alloc_;
        static util::internal_allocator<task_description> task_description_alloc_;


        ///////////////////////////////////////////////////////////////////////
        // add new threads if there is some amount of work available
        std::size_t add_to_thread_map(threads::thread_id_type thrd,
//                               std::unique_lock<mutex_type> &lk,
                               thread_state_enum state)
        {
#ifdef HPX_HAS_THREAD_MAP
            // add the new entry to the map of all threads
            thread_map_.assign(reinterpret_cast<std::uintptr_t>(thrd.get()), thrd.get());
            // this thread has to be in the map now
            HPX_ASSERT(thrd.get() == thread_map_.get(reinterpret_cast<std::uintptr_t>(thrd.get())));
            HPX_ASSERT(&thrd->get_queue<new_thread_queue>() == this);
#endif
            ++thread_map_count_;

            // only insert the thread into the work-items queue if it is in
            // pending state
            if (state == pending) {
                // pushing the new thread into the pending queue of the
                // specified new_thread_queue
                schedule_thread(thrd.get());
                return 1;
            }
            return 0;
        }

        void recycle_thread(thread_id_type thrd)
        {
            std::ptrdiff_t stacksize = thrd->get_stack_size();

            if (stacksize == get_stack_size(thread_stacksize_small))
            {
                thread_heap_small_.push(thrd);
            }
            else if (stacksize == get_stack_size(thread_stacksize_medium))
            {
                thread_heap_medium_.push(thrd);
            }
            else if (stacksize == get_stack_size(thread_stacksize_large))
            {
                thread_heap_large_.push(thrd);
            }
            else if (stacksize == get_stack_size(thread_stacksize_huge))
            {
                thread_heap_huge_.push(thrd);
            }
            else
            {
                switch(stacksize) {
                case thread_stacksize_small:
                    thread_heap_small_.push(thrd);
                    break;

                case thread_stacksize_medium:
                    thread_heap_medium_.push(thrd);
                    break;

                case thread_stacksize_large:
                    thread_heap_large_.push(thrd);
                    break;

                case thread_stacksize_huge:
                    thread_heap_huge_.push(thrd);
                    break;

                default:
                    HPX_ASSERT(false);
                    break;
                }
            }
        }

    public:
        /// This function makes sure all threads which are marked for deletion
        /// (state is terminated) are properly destroyed.
        ///
        /// This returns 'true' if there are no more terminated threads waiting
        /// to be deleted.
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

#ifdef HPX_HAS_THREAD_MAP
                    // this thread has to be in this map
                    HPX_ASSERT(todelete == thread_map_.erase(reinterpret_cast<std::uintptr_t>(tid.get())));
#endif
                    deallocate(todelete);
                    --thread_map_count_;
                    HPX_ASSERT(thread_map_count_ >= 0);
                }
            }
            else {
                // delete only this many threads
                std::int64_t delete_count =
                    (std::max)(
                        static_cast<std::int64_t>(terminated_items_count_ / 10),
                        static_cast<std::int64_t>(max_delete_count));

                thread_data* todelete;
                while (delete_count && terminated_items_.pop(todelete))
                {
                    thread_id_type tid(todelete);
                    --terminated_items_count_;
#ifdef HPX_HAS_THREAD_MAP
                    HPX_ASSERT(todelete == thread_map_.erase(reinterpret_cast<std::uintptr_t>(tid.get())));
                    recycle_thread(tid);
#else
                    recycle_thread(tid);
#endif
                    --thread_map_count_;
                    HPX_ASSERT(thread_map_count_ >= 0);

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
#ifdef HPX_HAS_THREAD_MAP
//            std::lock_guard<mutex_type> lk(mtx_);
#endif
            return cleanup_terminated_locked(false);
        }

        // The maximum number of active threads this thread manager should
        // create. This number will be a constraint only as long as the work
        // items queue is not empty. Otherwise the number of active threads
        // will be incremented in steps equal to the \a min_add_new_count
        // specified above.
        enum { max_thread_count = 1000 };

        new_thread_queue(std::size_t queue_num = std::size_t(-1),
                std::size_t max_count = max_thread_count)
          : min_tasks_to_steal_pending(detail::get_min_tasks_to_steal_pending()),
            min_tasks_to_steal_staged(detail::get_min_tasks_to_steal_staged()),
            min_add_new_count(detail::get_min_add_new_count()),
            max_add_new_count(detail::get_max_add_new_count()),
            max_delete_count(detail::get_max_delete_count()),
            max_terminated_threads(detail::get_max_terminated_threads()),
            thread_map_count_(0),
            work_items_(128, queue_num),
            work_items_count_(0),
            terminated_items_(128),
            terminated_items_count_(0),
            max_count_((0 == max_count)
                      ? static_cast<std::size_t>(max_thread_count)
                      : max_count),
            thread_heap_small_(),
            thread_heap_medium_(),
            thread_heap_large_(),
            thread_heap_huge_(),
            add_new_logger_("new_thread_queue::add_new")
        {}

        static void deallocate(threads::thread_data* p)
        {
            using threads::thread_data;
            p->~thread_data();
            thread_alloc_.deallocate(p, 1);
        }

        ~new_thread_queue()
        {
            while (!thread_heap_small_.empty()) {
                threads::thread_id_type t;
                thread_heap_small_.pop(t);
                deallocate(t.get());
            }
            while (!thread_heap_medium_.empty()) {
                threads::thread_id_type t;
                thread_heap_medium_.pop(t);
                deallocate(t.get());
            }
            while (!thread_heap_large_.empty()) {
                threads::thread_id_type t;
                thread_heap_large_.pop(t);
                deallocate(t.get());
            }
            while (!thread_heap_huge_.empty()) {
                threads::thread_id_type t;
                thread_heap_huge_.pop(t);
                deallocate(t.get());
            }
        }

        void set_max_count(std::size_t max_count = max_thread_count)
        {
            max_count_ = (0 == max_count) ? max_thread_count : max_count; //-V105
        }

        ///////////////////////////////////////////////////////////////////////
        // This returns the current length of the queues (work items and new items)
        std::int64_t get_queue_length() const
        {
            return work_items_count_;
        }

        // This returns the current length of the pending queue
        std::int64_t get_pending_queue_length() const
        {
            return work_items_count_;
        }

        // This returns the current length of the staged queue
        std::int64_t get_staged_queue_length(
            std::memory_order order = std::memory_order_seq_cst) const
        {
            return 0;
        }

        void increment_num_pending_misses(std::size_t num = 1) {}
        void increment_num_pending_accesses(std::size_t num = 1) {}
        void increment_num_stolen_from_pending(std::size_t num = 1) {}
        void increment_num_stolen_from_staged(std::size_t num = 1) {}
        void increment_num_stolen_to_pending(std::size_t num = 1) {}
        void increment_num_stolen_to_staged(std::size_t num = 1) {}

        ///////////////////////////////////////////////////////////////////////
        // create a new thread and schedule it if the initial state is equal to
        // pending
        void create_thread(thread_init_data& data, thread_id_type* id,
            thread_state_enum initial_state, error_code& ec)
        {
            // thread has not been created yet
            if (id) *id = invalid_thread_id;

            threads::thread_id_type thrd;

            // The mutex can not be locked while a new thread is getting
            // created, as it might have that the current HPX thread gets
            // suspended.
            {
//                std::unique_lock<mutex_type> lk(mtx_);

                create_thread_object(thrd, data, initial_state/*, lk*/);

                // add a new entry in the map for this thread
                add_to_thread_map(thrd, /*lk, */initial_state);

                // return the thread_id of the newly created thread
                if (id) *id = thrd;

                if (&ec != &throws)
                    ec = make_success_code();
                return;
            }
        }

        void move_work_items_from(new_thread_queue *src, std::int64_t count)
        {
            thread_description* trd;
            while (src->work_items_.pop(trd))
            {
                --src->work_items_count_;

                bool finished = count == ++work_items_count_;
                work_items_.push(trd);
                if (finished)
                    break;
            }
        }

        /// Return the next thread to be executed, return false if none is
        /// available
        bool get_next_thread(threads::thread_data*& thrd) HPX_HOT
        {
            std::int64_t work_items_count =
                work_items_count_.load(std::memory_order_relaxed);

            if (0 != work_items_count && work_items_.pop(thrd, false))
            {
                --work_items_count_;
                return true;
            }
            return false;
        }

        /// Schedule the passed thread
        void schedule_thread(threads::thread_data* thrd, bool other_end = false)
        {
            ++work_items_count_;
            work_items_.push(thrd, other_end);
        }

        /// Destroy the passed thread as it has been terminated
        void destroy_thread(threads::thread_data* thrd, std::int64_t& busy_count)
        {
            HPX_ASSERT(&thrd->get_queue<new_thread_queue>() == this);
            terminated_items_.push(thrd);

            std::int64_t count = ++terminated_items_count_;
            if (count > max_terminated_threads)
            {
                cleanup_terminated(true);   // clean up all terminated threads
            }
        }

        ///////////////////////////////////////////////////////////////////////
        /// Return the number of existing threads with the given state.
        std::int64_t get_thread_count(thread_state_enum state = unknown) /*const*/
        {
            if (terminated == state)
                return terminated_items_count_;

            if (unknown == state)
                return thread_map_count_ - terminated_items_count_;

            // acquire lock only if absolutely necessary
//            std::lock_guard<mutex_type> lk(mtx_);

            std::int64_t num_threads = 0;
            for (thread_map_iterator iter(thread_map_); iter.isValid(); iter.next()) {
                if (iter.getValue()->get_state().state() == state)
                    ++num_threads;
            }
            return num_threads;
        }

        ///////////////////////////////////////////////////////////////////////
        void abort_all_suspended_threads()
        {
            for (thread_map_iterator iter(thread_map_); iter.isValid(); iter.next()) {
                if (iter.getValue()->get_state().state() == suspended) {
                    iter.getValue()->set_state(pending, wait_abort);
                    schedule_thread(iter.getValue());
                }
            }
        }

        bool enumerate_threads(
            util::function_nonser<bool(thread_id_type)> const& f,
            thread_state_enum state = unknown) /*const*/
        {
            std::uint64_t count = thread_map_count_;
            if (state == terminated)
            {
                count = terminated_items_count_;
            }
            else if (state == staged)
            {
                HPX_THROW_EXCEPTION(bad_parameter,
                    "new_thread_queue::iterate_threads",
                    "can't iterate over thread ids of staged threads");
                return false;
            }

            std::vector<thread_id_type> ids;
            ids.reserve(static_cast<std::size_t>(count));

            if (state == unknown)
            {
                for (thread_map_iterator iter(thread_map_); iter.isValid(); iter.next()) {
                    ids.push_back(thread_id_type(iter.getValue()));
                }
            }
            else
            {
                for (thread_map_iterator iter(thread_map_); iter.isValid(); iter.next()) {
                    if (iter.getValue()->get_state().state() == state) {
                        ids.push_back(thread_id_type(iter.getValue()));
                    }
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

        ///////////////////////////////////////////////////////////////////////
        bool dump_suspended_threads(std::size_t num_thread
          , std::int64_t& idle_loop_count, bool running)
        {
#ifndef HPX_HAVE_THREAD_MINIMAL_DEADLOCK_DETECTION
            return false;
#else
            if (minimal_deadlock_detection) {
                std::lock_guard<mutex_type> lk(mtx_);
                return detail::dump_suspended_threads(num_thread, thread_map_
                  , idle_loop_count, running);
            }
            return false;
#endif
        }

        ///////////////////////////////////////////////////////////////////////
        void on_start_thread(std::size_t num_thread) {}
        void on_stop_thread(std::size_t num_thread) {}
        void on_error(std::size_t num_thread, std::exception_ptr const& e) {}

    private:
#ifdef HPX_HAS_THREAD_MAP
//        mutable mutex_type mtx_;            // mutex protecting the members
#endif

        thread_map_type thread_map_;        // mapping of thread id's to HPX-threads
        std::atomic<std::int64_t> thread_map_count_; // overall count of work items

        work_items_type work_items_;        // list of active work items
        std::atomic<std::int64_t> work_items_count_; // count of active work items

        terminated_items_type terminated_items_;    // list of terminated threads
        std::atomic<std::int64_t> terminated_items_count_; // count of terminated items

        std::size_t max_count_;     // maximum number of existing HPX-threads

        thread_heap_type thread_heap_small_;
        thread_heap_type thread_heap_medium_;
        thread_heap_type thread_heap_large_;
        thread_heap_type thread_heap_huge_;

        util::block_profiler<add_new_tag> add_new_logger_;
    };

    ///////////////////////////////////////////////////////////////////////////
    template <typename StagedQueuing,
        typename TerminatedQueuing>
    util::internal_allocator<threads::thread_data> new_thread_queue<
        StagedQueuing, TerminatedQueuing>::thread_alloc_;

    template <typename StagedQueuing,
        typename TerminatedQueuing>
    util::internal_allocator<typename new_thread_queue<StagedQueuing,
            TerminatedQueuing>::task_description>
        new_thread_queue<StagedQueuing,
            TerminatedQueuing>::task_description_alloc_;
}}}

#endif
