//  Copyright (c) 2007-2019 Hartmut Kaiser
//  Copyright (c) 2011      Bryce Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_THREADMANAGER_THREAD_QUEUE_MC)
#define HPX_THREADMANAGER_THREAD_QUEUE_MC

#include <hpx/config.hpp>
#include <hpx/compat/mutex.hpp>
#include <hpx/error_code.hpp>
#include <hpx/runtime/config_entry.hpp>
#include <hpx/runtime/threads/policies/lockfree_queue_backends.hpp>
#include <hpx/runtime/threads/thread_data.hpp>
#include <hpx/runtime/threads/policies/thread_queue.hpp>
#include <hpx/throw_exception.hpp>
#include <hpx/util/assert.hpp>
#include <hpx/util/block_profiler.hpp>
#include <hpx/util/cache_aligned_data.hpp>
#include <hpx/util/function.hpp>
#include <hpx/util/get_and_reset_value.hpp>
#include <hpx/util/high_resolution_clock.hpp>
#include <hpx/util/internal_allocator.hpp>
#include <hpx/util/unlock_guard.hpp>
//
#include <hpx/runtime/threads/policies/thread_queue.hpp>
#include <hpx/runtime/threads/policies/queue_holder_thread.hpp>
//
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

#include <atomic>
#include <mutex>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#define THREAD_DESC(x) ""

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace threads { namespace policies
{
    template <typename Mutex = compat::mutex,
              typename PendingQueuing = lockfree_fifo,
              typename StagedQueuing = lockfree_lifo,
              typename TerminatedQueuing = lockfree_fifo>
    class thread_queue_mc
    {
    public:
        // we use a simple mutex to protect the data members for now
        typedef Mutex mutex_type;

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

        int const queue_index;

        using thread_heap_type =
            std::list<thread_id_type, util::internal_allocator<thread_id_type>>;

        typedef util::tuple<thread_init_data, thread_state_enum> task_description;

        typedef thread_data thread_description;

        typedef typename PendingQueuing::template
            apply<thread_description*>::type work_items_type;

        typedef typename StagedQueuing::template
            apply<task_description*>::type task_items_type;

    protected:
        // ----------------------------------------------------------------
        // add new threads if there is some amount of work available
        std::size_t add_new(std::int64_t add_count, thread_queue_mc* addfrom,
            std::unique_lock<mutex_type> &lk, bool steal = false)
        {
            HPX_ASSERT(lk.owns_lock());

            if (HPX_UNLIKELY(0 == add_count))
                return 0;

            std::size_t added = 0;
            task_description* task = nullptr;
            while (add_count-- && addfrom->new_tasks_.pop(task, steal))
            {
                // create the new thread
                threads::thread_init_data& data = util::get<0>(*task);
                thread_state_enum state = util::get<1>(*task);
                threads::thread_id_type thrd;

                holder_->debug("add_new 1 ", queue_index, new_tasks_count_.data_, work_items_count_.data_, thrd.get());
                holder_->create_thread_object(thrd, data, state, lk);
                holder_->add_to_thread_map(thrd, lk);
                // Decrement only after thread_map_count_ has been incremented
                --addfrom->new_tasks_count_.data_;

                holder_->debug("add_new 2 ", queue_index, new_tasks_count_.data_, work_items_count_.data_, thrd.get());
                // only insert the thread into the work-items queue if it is in
                // pending state
                if (state == pending) {
                    // pushing the new thread into the pending queue of the
                    // specified thread_queue
                    ++added;
                    schedule_thread(thrd.get(), false);
                }

                // this thread has to be in the map now
                HPX_ASSERT(&thrd->get_queue<queue_holder_thread<thread_queue_mc<>>>() == holder_);
            }

            if (added) {
                LTM_(debug) << "add_new: added " << added << " tasks to queues"; //-V128
            }
            return added;
        }

        // ----------------------------------------------------------------
        bool add_new_always(std::size_t& added, thread_queue_mc* addfrom,
            std::unique_lock<mutex_type> &lk, bool steal = false)
        {
            HPX_ASSERT(lk.owns_lock());

            // create new threads from pending tasks (if appropriate)
            std::int64_t add_count = -1;            // default is no constraint

            // if we are desperate (no work in the queues), add some even if the
            // map holds more than max_count
            if (HPX_LIKELY(max_count_)) {
                std::size_t count = holder_->thread_map_.size();
                if (max_count_ >= count + min_add_new_count) { //-V104
                    HPX_ASSERT(max_count_ - count <
                        static_cast<std::size_t>(
                            (std::numeric_limits<std::int64_t>::max)()
                        ));
                    add_count = static_cast<std::int64_t>(max_count_ - count);
                    if (add_count < min_add_new_count)
                        add_count = min_add_new_count;
                    if (add_count > max_add_new_count)
                        add_count = max_add_new_count;
                }
                else if (work_items_.empty()) {
                    add_count = min_add_new_count;    // add this number of threads
                    max_count_ += min_add_new_count;  // increase max_count //-V101
                }
                else {
                    return false;
                }
            }

            std::size_t addednew = add_new(add_count, addfrom, lk, steal);
            added += addednew;
            return addednew != 0;
        }

    public:

        // ----------------------------------------------------------------
        // The maximum number of active threads this thread manager should
        // create. This number will be a constraint only as long as the work
        // items queue is not empty. Otherwise the number of active threads
        // will be incremented in steps equal to the \a min_add_new_count
        // specified above.
        enum { max_thread_count = 1000 };

        // ----------------------------------------------------------------
        thread_queue_mc(int index)
          : min_tasks_to_steal_pending(detail::get_min_tasks_to_steal_pending()),
            min_tasks_to_steal_staged(detail::get_min_tasks_to_steal_staged()),
            min_add_new_count(detail::get_min_add_new_count()),
            max_add_new_count(detail::get_max_add_new_count()),
            max_delete_count(detail::get_max_delete_count()),
            max_terminated_threads(detail::get_max_terminated_threads()),
            queue_index(index),
            holder_(nullptr),
            work_items_(128),
            max_count_(static_cast<std::size_t>(max_thread_count)),
            new_tasks_(128)
        {
            new_tasks_count_.data_ = 0;
            work_items_count_.data_ = 0;
        }

        // ----------------------------------------------------------------
        void set_holder(queue_holder_thread<thread_queue_mc<>> *holder)
        {
            holder_ = holder;
        }

        // ----------------------------------------------------------------
        ~thread_queue_mc()
        {
        }

        void set_max_count(std::size_t max_count = max_thread_count)
        {
            max_count_ = (0 == max_count) ? max_thread_count : max_count; //-V105
        }

        // ----------------------------------------------------------------
        // This returns the current length of the queues (work items and new items)
        std::int64_t get_queue_length() const
        {
            return work_items_count_.data_ + new_tasks_count_.data_;
        }

        // ----------------------------------------------------------------
        // This returns the current length of the pending queue
        std::int64_t get_queue_length_pending() const
        {
            return work_items_count_.data_;
        }

        // ----------------------------------------------------------------
        // This returns the current length of the staged queue
        std::int64_t get_queue_length_staged(
            std::memory_order order = std::memory_order_seq_cst) const
        {
            return new_tasks_count_.data_.load(order);
        }

        // ----------------------------------------------------------------
        // Return the number of existing threads with the given state.
        std::int64_t get_thread_count() const
        {
            HPX_THROW_EXCEPTION(bad_parameter,
                "queue_holder_thread::get_thread_count",
                "use get_queue_length_staged/get_queue_length_pending");
            return 0;
        }

        // ----------------------------------------------------------------
        // create a new thread and schedule it if the initial state is equal to
        // pending
        void create_thread(thread_init_data& data, thread_id_type* id,
            thread_state_enum initial_state, bool run_now, error_code& ec)
        {
            // thread has not been created yet
            if (id) *id = invalid_thread_id;

            if (run_now)
            {
                threads::thread_id_type thrd;

                // The mutex can not be locked while a new thread is getting
                // created, as it might have that the current HPX thread gets
                // suspended.
                {
                    std::unique_lock<mutex_type> lk(holder_->mtx_);

                    holder_->create_thread_object(thrd, data, initial_state, lk);
                    holder_->add_to_thread_map(thrd, lk);
                    holder_->debug("create run", queue_index, new_tasks_count_.data_, work_items_count_.data_, thrd.get());

                    // push the new thread in the pending queue thread
                    if (initial_state == pending)
                        schedule_thread(thrd.get(), false);

                    // return the thread_id of the newly created thread
                    if (id) *id = thrd;

                    if (&ec != &throws)
                        ec = make_success_code();
                    return;
                }
            }

            // do not execute the work, but register a task description for
            // later thread creation
            ++new_tasks_count_.data_;

            task_description* td = new task_description();
            new (td) task_description(std::move(data), initial_state); //-V106
            new_tasks_.push(td);
            if (&ec != &throws)
                ec = make_success_code();

            holder_->debug("create    ", queue_index, new_tasks_count_.data_, work_items_count_.data_, nullptr);
        }

        // ----------------------------------------------------------------
        /// Return the next thread to be executed, return false if none is
        /// available
        bool get_next_thread(threads::thread_data*& thrd,
            bool allow_stealing, bool other_end) HPX_HOT
        {
            std::int64_t work_items_count =
                work_items_count_.data_.load(std::memory_order_relaxed);

            if (allow_stealing && min_tasks_to_steal_pending > work_items_count)
            {
                holder_->debug("nostealing", queue_index, new_tasks_count_.data_, work_items_count_.data_, thrd);
                return false;
            }

            if (0 != work_items_count && work_items_.pop(thrd, other_end))
            {
                --work_items_count_.data_;
                holder_->debug("get       ", queue_index, new_tasks_count_.data_, work_items_count_.data_, thrd);
                return true;
            }
            return false;
        }

        // ----------------------------------------------------------------
        /// Schedule the passed thread (put it on the ready work queue)
        void schedule_thread(threads::thread_data* thrd, bool other_end)
        {
            int t = ++work_items_count_.data_;
            holder_->debug("schedule  ", queue_index, new_tasks_count_.data_, t, thrd);
            work_items_.push(thrd, other_end);
#ifdef SHARED_PRIORITY_SCHEDULER_DEBUG
//            debug_queue(work_items_);
#endif
        }

        // ----------------------------------------------------------------
        /// This is a function which gets called periodically by the thread
        /// manager to allow for maintenance tasks to be executed in the
        /// scheduler. Returns true if the OS thread calling this function
        /// has to be terminated (i.e. no more work has to be done).
        inline bool wait_or_add_new(bool running,
            std::size_t& added, bool steal = false) HPX_HOT
        {
            if (0 == new_tasks_count_.data_.load(std::memory_order_relaxed))
            {
                return true;
            }

            // No obvious work has to be done, so a lock won't hurt too much.
            //
            // We prefer to exit this function (some kind of very short
            // busy waiting) to blocking on this lock. Locking fails either
            // when a thread is currently doing thread maintenance, which
            // means there might be new work, or the thread owning the lock
            // just falls through to the cleanup work below (no work is available)
            // in which case the current thread (which failed to acquire
            // the lock) will just retry to enter this loop.
            std::unique_lock<mutex_type> lk(holder_->mtx_, std::try_to_lock);
            if (!lk.owns_lock())
                return false;            // avoid long wait on lock

            // stop running after all HPX threads have been terminated
            return add_new_always(added, this, lk);
        }

        ///////////////////////////////////////////////////////////////////////
        void on_start_thread(std::size_t num_thread) {}
        void on_stop_thread(std::size_t num_thread) {}
        void on_error(std::size_t num_thread, std::exception_ptr const& e) {}

        // pops all tasks off the queue, prints info and pushes them back on
        // just because we can't iterate over the queue/stack in general
#ifdef SHARED_PRIORITY_SCHEDULER_DEBUG
        void debug_queue(work_items_type &q) {
            std::unique_lock<std::mutex> Lock(special_mtx_);
            //
            work_items_type work_items_copy_;
            int x = 0;
            thread_description *thrd;
            while (q.pop(thrd)) {
                LOG_CUSTOM_MSG("\t" << x++ << " " << THREAD_DESC(thrd));
                work_items_copy_.push(thrd);
            }
            LOG_CUSTOM_MSG("\tPushing to old queue");
            while (work_items_copy_.pop(thrd)) {
                q.push(thrd);
                LOG_CUSTOM_MSG("\t" << --x << " " << THREAD_DESC(thrd));
            }
        }
#endif

    public:
        queue_holder_thread<thread_queue_mc<>> *holder_;

        // list of active work items
        work_items_type work_items_;

        std::size_t max_count_;     // maximum number of existing HPX-threads
        task_items_type new_tasks_; // list of new tasks to run

        // count of new tasks to run, separate to new cache line to avoid false
        // sharing
        util::cache_line_data<std::atomic<std::int64_t>> new_tasks_count_;

        // count of active work items
        util::cache_line_data<std::atomic<std::int64_t>> work_items_count_;

#ifdef SHARED_PRIORITY_SCHEDULER_DEBUG
        std::mutex special_mtx_;
#endif

    };

}}}

#endif

