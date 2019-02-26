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
#include <hpx/runtime/threads/thread_data.hpp>
#include <hpx/throw_exception.hpp>
#include <hpx/util/assert.hpp>
#include <hpx/util/block_profiler.hpp>
#include <hpx/util/function.hpp>
#include <hpx/util/get_and_reset_value.hpp>
#include <hpx/util/high_resolution_clock.hpp>
#include <hpx/util/internal_allocator.hpp>
#include <hpx/util/unlock_guard.hpp>

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

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace threads { namespace policies
{
    template <typename QueueType>
    struct queue_holder;

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
    template <typename Mutex = compat::mutex,
              typename PendingQueuing = lockfree_lifo,
              typename TerminatedQueuing = lockfree_fifo>
    class new_thread_queue
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

        using thread_heap_type =
            std::list<thread_id_type, util::internal_allocator<thread_id_type>>;

        typedef util::tuple<thread_init_data, thread_state_enum> task_description;

        typedef thread_data thread_description;

        typedef typename PendingQueuing::template
            apply<thread_description*>::type work_items_type;

        typedef lockfree_fifo::
            apply<task_description>::type task_items_type;

    protected:
        ///////////////////////////////////////////////////////////////////////
        // add new threads if there is some amount of work available
        std::size_t add_new(std::int64_t add_count, new_thread_queue* addfrom,
            std::unique_lock<mutex_type> &lk, bool steal = false)
        {
            HPX_ASSERT(lk.owns_lock());

            if (HPX_UNLIKELY(0 == add_count))
                return 0;

            std::size_t added = 0;
            task_description task;
            while (add_count-- && addfrom->new_tasks_.pop(task, steal))
            {
                // create the new thread
                threads::thread_init_data& data = util::get<0>(task);
                thread_state_enum state = util::get<1>(task);
                threads::thread_id_type thrd;

                holder_->create_thread_object(thrd, data, state, lk);
                holder_->add_to_thread_map(thrd, lk);

                // Decrement only after thread_map_count_ has been incremented
                --addfrom->new_tasks_count_;

                holder_->debug("add_new   ", new_tasks_count_, work_items_count_, thrd.get());
                // only insert the thread into the work-items queue if it is in
                // pending state
                if (state == pending) {
                    // pushing the new thread into the pending queue of the
                    // specified thread_queue
                    ++added;
                    schedule_thread(thrd.get());
                }

                // this thread has to be in the map now
                HPX_ASSERT(&thrd->get_queue<queue_holder<new_thread_queue<>>>() == holder_);
            }

            if (added) {
                LTM_(debug) << "add_new: added " << added << " tasks to queues"; //-V128
            }
            return added;
        }

        ///////////////////////////////////////////////////////////////////////
        bool add_new_always(std::size_t& added, new_thread_queue* addfrom,
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

        // The maximum number of active threads this thread manager should
        // create. This number will be a constraint only as long as the work
        // items queue is not empty. Otherwise the number of active threads
        // will be incremented in steps equal to the \a min_add_new_count
        // specified above.
        enum { max_thread_count = 1000 };


        new_thread_queue(queue_holder<new_thread_queue<>> *holder, std::size_t size_ = max_thread_count)
          : min_tasks_to_steal_pending(detail::get_min_tasks_to_steal_pending()),
            min_tasks_to_steal_staged(detail::get_min_tasks_to_steal_staged()),
            min_add_new_count(detail::get_min_add_new_count()),
            max_add_new_count(detail::get_max_add_new_count()),
            max_delete_count(detail::get_max_delete_count()),
            max_terminated_threads(detail::get_max_terminated_threads()),
            holder_(holder),
            work_items_(128),
            work_items_count_(0),
            max_count_((0 == size_)
                      ? static_cast<std::size_t>(max_thread_count)
                      : size_),
            new_tasks_(128),
            new_tasks_count_(0)
        {}

        ~new_thread_queue()
        {
        }

        void set_max_count(std::size_t max_count = max_thread_count)
        {
            max_count_ = (0 == max_count) ? max_thread_count : max_count; //-V105
        }

        ///////////////////////////////////////////////////////////////////////
        // This returns the current length of the queues (work items and new items)
        std::int64_t get_queue_length() const
        {
            return work_items_count_ + new_tasks_count_;
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
            return new_tasks_count_.load(order);
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
                    holder_->debug("create run", new_tasks_count_, work_items_count_, thrd.get());

                    // push the new thread in the pending queue thread
                    if (initial_state == pending)
                        schedule_thread(thrd.get());

                    // return the thread_id of the newly created thread
                    if (id) *id = thrd;

                    if (&ec != &throws)
                        ec = make_success_code();
                    return;
                }
            }

            // do not execute the work, but register a task description for
            // later thread creation
            ++new_tasks_count_;

            new_tasks_.push(task_description(std::move(data), initial_state)); //-V106

            if (&ec != &throws)
                ec = make_success_code();

            holder_->debug("create    ", new_tasks_count_, work_items_count_, nullptr);
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

        void move_task_items_from(new_thread_queue *src,
            std::int64_t count)
        {
            task_description task;
            while (src->new_tasks_.pop(task))
            {
                bool finish = count == ++new_tasks_count_;

                // Decrement only after the local new_tasks_count_ has
                // been incremented
                --src->new_tasks_count_;

                if (new_tasks_.push(task))
                {
                    if (finish)
                        break;
                }
                else
                {
                    --new_tasks_count_;
                }
            }
        }

        /// Return the next thread to be executed, return false if none is
        /// available
        bool get_next_thread(threads::thread_data*& thrd,
            bool allow_stealing = false, bool steal = false) HPX_HOT
        {
            std::int64_t work_items_count =
                work_items_count_.load(std::memory_order_relaxed);

            if (allow_stealing && min_tasks_to_steal_pending > work_items_count)
            {
                return false;
            }

            if (0 != work_items_count && work_items_.pop(thrd, steal))
            {
                --work_items_count_;
                holder_->debug("get       ", new_tasks_count_, work_items_count_, thrd);
                return true;
            }
            return false;
        }

        /// Schedule the passed thread
        void schedule_thread(threads::thread_data* thrd, bool other_end = false)
        {
            int t = ++work_items_count_;
            holder_->debug("schedule  ", new_tasks_count_, t, thrd);
            work_items_.push(thrd, other_end);
            debug_queue(work_items_);
        }

        ///////////////////////////////////////////////////////////////////////
        void abort_all_suspended_threads()
        {
//            std::lock_guard<mutex_type> lk(holder_->mtx_);
//            thread_map_type::iterator end =  thread_map_.end();
//            for (thread_map_type::iterator it = thread_map_.begin();
//                 it != end; ++it)
//            {
//                if ((*it)->get_state().state() == suspended)
//                {
//                    (*it)->set_state(pending, wait_abort);
//                    schedule_thread((*it).get());
//                }
//            }
        }

        /// This is a function which gets called periodically by the thread
        /// manager to allow for maintenance tasks to be executed in the
        /// scheduler. Returns true if the OS thread calling this function
        /// has to be terminated (i.e. no more work has to be done).
        inline bool wait_or_add_new(bool running,
            std::int64_t& idle_loop_count, std::size_t& added,
            new_thread_queue* addfrom = nullptr, bool steal = false) HPX_HOT
        {
            // try to generate new threads from task lists, but only if our
            // own list of threads is empty
            if (0 == work_items_count_.load(std::memory_order_relaxed))
            {
                // see if we can avoid grabbing the lock below
                if (addfrom)
                {
                    // don't try to steal if there are only a few tasks left on
                    // this queue
                    if (running && min_tasks_to_steal_staged >
                        addfrom->new_tasks_count_.load(std::memory_order_relaxed))
                    {
                        return false;
                    }
                }
                else
                {
                    if (running &&
                        0 == new_tasks_count_.load(std::memory_order_relaxed))
                    {
                        return false;
                    }
                    addfrom = this;
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
                bool added_new = add_new_always(added, addfrom, lk, steal);
                if (!added_new) {
                    // Before exiting each of the OS threads deletes the
                    // remaining terminated HPX threads
                    // REVIEW: Should we be doing this if we are stealing?
                    bool canexit = holder_->cleanup_terminated_locked(true);
                    if (!running && canexit) {
                        // we don't have any registered work items anymore
                        //do_some_work();       // notify possibly waiting threads
                        return true;            // terminate scheduling loop
                    }
                    return false;
                }
                else
                {
                    return holder_->cleanup_terminated_locked();
                }
            }

            bool canexit = holder_->cleanup_terminated(true);
            if (!running && canexit)
            {
                // we don't have any registered work items anymore
                return true; // terminate scheduling loop
            }

            return false;
        }

        ///////////////////////////////////////////////////////////////////////
        void on_start_thread(std::size_t num_thread) {}
        void on_stop_thread(std::size_t num_thread) {}
        void on_error(std::size_t num_thread, std::exception_ptr const& e) {}

        void debug_queue(work_items_type &q) {
            std::unique_lock<std::mutex> Lock(special_mtx_);
            //
            int x= 0;
            thread_description *thrd;
            while (q.pop(thrd)) {
                std::cout << "\t" << x++ << " " << THREAD_DESC(thrd) << "\n";
                work_items_copy_.push(thrd);
            }
            while (work_items_copy_.pop(thrd)) {
                q.push(thrd);
            }
        }

    public:
        queue_holder<new_thread_queue<>> *holder_;

        work_items_type work_items_;        // list of active work items
        std::atomic<std::int64_t> work_items_count_; // count of active work items

        std::size_t max_count_;     // maximum number of existing HPX-threads
        task_items_type new_tasks_; // list of new tasks to run

        std::atomic<std::int64_t> new_tasks_count_; // count of new tasks to run

        std::mutex special_mtx_;
        work_items_type work_items_copy_;        // list of active work items

    };

}}}

#endif

