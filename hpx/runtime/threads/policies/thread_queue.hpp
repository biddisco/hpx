//  Copyright (c) 2007-2019 Hartmut Kaiser
//  Copyright (c) 2011      Bryce Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_THREADMANAGER_THREAD_QUEUE_AUG_25_2009_0132PM)
#define HPX_THREADMANAGER_THREAD_QUEUE_AUG_25_2009_0132PM

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

#ifdef HPX_HAVE_THREAD_CREATION_AND_CLEANUP_RATES
#   include <hpx/util/tick_counter.hpp>
#endif

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

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace threads { namespace policies
{
#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
    ///////////////////////////////////////////////////////////////////////////
    // We control whether to collect queue wait times using this global bool.
    // It will be set by any of the related performance counters. Once set it
    // stays set, thus no race conditions will occur.
    extern HPX_EXPORT bool maintain_queue_wait_times;
#endif
#ifdef HPX_HAVE_THREAD_MINIMAL_DEADLOCK_DETECTION
    ///////////////////////////////////////////////////////////////////////////
    // We globally control whether to do minimal deadlock detection using this
    // global bool variable. It will be set once by the runtime configuration
    // startup code
    extern bool minimal_deadlock_detection;
#endif

    namespace detail
    {
        inline int get_min_tasks_to_steal_pending()
        {
            static int min_tasks_to_steal_pending =
                boost::lexical_cast<int>(hpx::get_config_entry(
                    "hpx.thread_queue.min_tasks_to_steal_pending", "0"));
            return min_tasks_to_steal_pending;
        }

        inline int get_min_tasks_to_steal_staged()
        {
            static int min_tasks_to_steal_staged =
                boost::lexical_cast<int>(hpx::get_config_entry(
                    "hpx.thread_queue.min_tasks_to_steal_staged", "10"));
            return min_tasks_to_steal_staged;
        }

        inline int get_min_add_new_count()
        {
            static int min_add_new_count =
                boost::lexical_cast<int>(hpx::get_config_entry(
                    "hpx.thread_queue.min_add_new_count", "10"));
            return min_add_new_count;
        }

        inline int get_max_add_new_count()
        {
            static int max_add_new_count =
                boost::lexical_cast<int>(hpx::get_config_entry(
                    "hpx.thread_queue.max_add_new_count", "10"));
            return max_add_new_count;
        }

        inline int get_max_delete_count()
        {
            static int max_delete_count =
                boost::lexical_cast<int>(hpx::get_config_entry(
                    "hpx.thread_queue.max_delete_count", "1000"));
            return max_delete_count;
        }

        inline int get_max_terminated_threads()
        {
            static int max_terminated_threads =
                boost::lexical_cast<int>(hpx::get_config_entry(
                    "hpx.thread_queue.max_terminated_threads",
                    std::to_string(HPX_SCHEDULER_MAX_TERMINATED_THREADS)));
            return max_terminated_threads;
        }
    }

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
        typename StagedQueuing = lockfree_lifo,
        typename TerminatedQueuing = lockfree_fifo>
    class thread_queue
    {
    private:
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

        // this is the type of a map holding all threads (except depleted ones)
        using thread_map_type = std::unordered_set<thread_id_type,
            std::hash<thread_id_type>, std::equal_to<thread_id_type>,
            util::internal_allocator<thread_id_type>>;

        using thread_heap_type =
            std::list<thread_id_type, util::internal_allocator<thread_id_type>>;

#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
        typedef
            util::tuple<thread_init_data, thread_state_enum, std::uint64_t>
        task_description;
#else
        typedef util::tuple<thread_init_data, thread_state_enum> task_description;
#endif

#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
        typedef util::tuple<thread_data*, std::uint64_t> thread_description;
#else
        typedef thread_data thread_description;
#endif

        typedef typename PendingQueuing::template
            apply<thread_description*>::type work_items_type;

        typedef typename StagedQueuing::template
            apply<task_description*>::type task_items_type;

        typedef typename TerminatedQueuing::template
            apply<thread_data*>::type terminated_items_type;

    protected:
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

        static util::internal_allocator<threads::thread_data> thread_alloc_;
        static util::internal_allocator<task_description> task_description_alloc_;

//        ///////////////////////////////////////////////////////////////////////
//        // add new threads if there is some amount of work available
//        std::size_t add_new(std::int64_t add_count, thread_queue* addfrom,
//            std::unique_lock<mutex_type> &lk, bool steal = false)
//        {
//            HPX_ASSERT(lk.owns_lock());

//            if (HPX_UNLIKELY(0 == add_count))
//                return 0;

//            std::size_t added = 0;
//            task_description* task = nullptr;
//            while (add_count-- && addfrom->new_tasks_.pop(task, steal))
//            {
//#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
//                if (maintain_queue_wait_times) {
//                    addfrom->new_tasks_wait_ +=
//                        util::high_resolution_clock::now() - util::get<2>(*task);
//                    ++addfrom->new_tasks_wait_count_;
//                }
//#endif
//                --addfrom->new_tasks_count_;

//                // measure thread creation time
//                util::block_profiler_wrapper<add_new_tag> bp(add_new_logger_);

//                // create the new thread
//                threads::thread_init_data& data = util::get<0>(*task);
//                thread_state_enum state = util::get<1>(*task);
//                threads::thread_id_type thrd;

//                create_thread_object(thrd, data, state, lk);

//                delete task;

//                added += add_to_thread_map(thrd, lk, state);

//            }
//            if (added>0) {
//              LTM_(debug) << "add_new: added " << added << " tasks to queues"; //-V128
//            }
//            return added;
//        }

        ///////////////////////////////////////////////////////////////////////
        bool add_new_always(std::size_t& added, thread_queue* addfrom,
            std::unique_lock<mutex_type> &lk, bool steal = false)
        {
            HPX_ASSERT(lk.owns_lock());

#ifdef HPX_HAVE_THREAD_CREATION_AND_CLEANUP_RATES
            util::tick_counter tc(add_new_time_);
#endif

            // create new threads from pending tasks (if appropriate)
            std::int64_t add_count = -1;            // default is no constraint

            // if we are desperate (no work in the queues), add some even if the
            // map holds more than max_count
            if (HPX_LIKELY(max_count_)) {
                std::size_t count = thread_map_.size();
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

        ///////////////////////////////////////////////////////////////////////
        // add new threads if there is some amount of work available
        std::size_t add_to_thread_map(threads::thread_id_type thrd,
                               std::unique_lock<mutex_type> &lk,
                               thread_state_enum state)
        {
                // add the new entry to the map of all threads
                std::pair<thread_map_type::iterator, bool> p =
                    thread_map_.insert(thrd);

            if (HPX_UNLIKELY(!p.second)) {
                lk.unlock();
                HPX_THROW_EXCEPTION(hpx::out_of_memory,
                    "threadmanager::add_new",
                    "Couldn't add new thread to the thread map");
                return 0;
            }
            ++thread_map_count_;

            // this thread has to be in the map now
            HPX_ASSERT(thread_map_.find(thrd) != thread_map_.end());
            HPX_ASSERT(&thrd->get_queue<thread_queue>() == this);

            // only insert the thread into the work-items queue if it is in
            // pending state
            if (state == pending) {
                // pushing the new thread into the pending queue of the
                // specified thread_queue
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

    public:
        /// This function makes sure all threads which are marked for deletion
        /// (state is terminated) are properly destroyed.
        ///
        /// This returns 'true' if there are no more terminated threads waiting
        /// to be deleted.
        bool cleanup_terminated_locked(bool delete_all = false)
        {
#ifdef HPX_HAVE_THREAD_CREATION_AND_CLEANUP_RATES
            util::tick_counter tc(cleanup_terminated_time_);
#endif

            if (terminated_items_count_ == 0)
                return true;

            if (delete_all) {
                // delete all threads
                thread_data* todelete;
                while (terminated_items_.pop(todelete))
                {
                    thread_id_type tid(todelete);
                    --terminated_items_count_;

                    // this thread has to be in this map
                    HPX_ASSERT(thread_map_.find(tid) != thread_map_.end());

                    bool deleted = thread_map_.erase(tid) != 0;
                    HPX_ASSERT(deleted);
                    if (deleted) {
                        deallocate(todelete);
                        --thread_map_count_;
                        HPX_ASSERT(thread_map_count_ >= 0);
                    }
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

                    thread_map_type::iterator it = thread_map_.find(tid);

                    // this thread has to be in this map
                    HPX_ASSERT(it != thread_map_.end());

                    recycle_thread(*it);

                    thread_map_.erase(it);
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

        // The maximum number of active threads this thread manager should
        // create. This number will be a constraint only as long as the work
        // items queue is not empty. Otherwise the number of active threads
        // will be incremented in steps equal to the \a min_add_new_count
        // specified above.
        enum { max_thread_count = 1000 };

        thread_queue(std::size_t queue_num = std::size_t(-1),
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
#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
            work_items_wait_(0),
            work_items_wait_count_(0),
#endif
            terminated_items_(128),
            terminated_items_count_(0),
            max_count_((0 == max_count)
                      ? static_cast<std::size_t>(max_thread_count)
                      : max_count),
            thread_heap_small_(),
            thread_heap_medium_(),
            thread_heap_large_(),
            thread_heap_huge_(),
#ifdef HPX_HAVE_THREAD_CREATION_AND_CLEANUP_RATES
            add_new_time_(0),
            cleanup_terminated_time_(0),
#endif
#ifdef HPX_HAVE_THREAD_STEALING_COUNTS
            pending_misses_(0),
            pending_accesses_(0),
            stolen_from_pending_(0),
            stolen_from_staged_(0),
            stolen_to_pending_(0),
            stolen_to_staged_(0),
#endif
            add_new_logger_("thread_queue::add_new")
        {}

        static void deallocate(threads::thread_data* p)
        {
            using threads::thread_data;
            p->~thread_data();
            thread_alloc_.deallocate(p, 1);
        }

        ~thread_queue()
        {
            for(auto t: thread_heap_small_)
                deallocate(t.get());

            for(auto t: thread_heap_medium_)
                deallocate(t.get());

            for(auto t: thread_heap_large_)
                deallocate(t.get());

            for(auto t: thread_heap_huge_)
                deallocate(t.get());
        }

        void set_max_count(std::size_t max_count = max_thread_count)
        {
            max_count_ = (0 == max_count) ? max_thread_count : max_count; //-V105
        }

#ifdef HPX_HAVE_THREAD_CREATION_AND_CLEANUP_RATES
        std::uint64_t get_creation_time(bool reset)
        {
            return util::get_and_reset_value(add_new_time_, reset);
        }

        std::uint64_t get_cleanup_time(bool reset)
        {
            return util::get_and_reset_value(cleanup_terminated_time_, reset);
        }
#endif

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

#ifdef HPX_HAVE_THREAD_STEALING_COUNTS
        std::int64_t get_num_pending_misses(bool reset)
        {
            return util::get_and_reset_value(pending_misses_, reset);
        }

        void increment_num_pending_misses(std::size_t num = 1)
        {
            pending_misses_ += num;
        }

        std::int64_t get_num_pending_accesses(bool reset)
        {
            return util::get_and_reset_value(pending_accesses_, reset);
        }

        void increment_num_pending_accesses(std::size_t num = 1)
        {
            pending_accesses_ += num;
        }

        std::int64_t get_num_stolen_from_pending(bool reset)
        {
            return util::get_and_reset_value(stolen_from_pending_, reset);
        }

        void increment_num_stolen_from_pending(std::size_t num = 1)
        {
            stolen_from_pending_ += num;
        }

        std::int64_t get_num_stolen_from_staged(bool reset)
        {
            return util::get_and_reset_value(stolen_from_staged_, reset);
        }

        void increment_num_stolen_from_staged(std::size_t num = 1)
        {
            stolen_from_staged_ += num;
        }

        std::int64_t get_num_stolen_to_pending(bool reset)
        {
            return util::get_and_reset_value(stolen_to_pending_, reset);
        }

        void increment_num_stolen_to_pending(std::size_t num = 1)
        {
            stolen_to_pending_ += num;
        }

        std::int64_t get_num_stolen_to_staged(bool reset)
        {
            return util::get_and_reset_value(stolen_to_staged_, reset);
        }

        void increment_num_stolen_to_staged(std::size_t num = 1)
        {
            stolen_to_staged_ += num;
        }
#else
        void increment_num_pending_misses(std::size_t num = 1) {}
        void increment_num_pending_accesses(std::size_t num = 1) {}
        void increment_num_stolen_from_pending(std::size_t num = 1) {}
        void increment_num_stolen_from_staged(std::size_t num = 1) {}
        void increment_num_stolen_to_pending(std::size_t num = 1) {}
        void increment_num_stolen_to_staged(std::size_t num = 1) {}
#endif

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
                std::unique_lock<mutex_type> lk(mtx_);

                create_thread_object(thrd, data, initial_state, lk);

                // add a new entry in the map for this thread
                add_to_thread_map(thrd, lk, initial_state);

                // return the thread_id of the newly created thread
                if (id) *id = thrd;

                if (&ec != &throws)
                    ec = make_success_code();
                return;
            }
        }

        void move_work_items_from(thread_queue *src, std::int64_t count)
        {
            thread_description* trd;
            while (src->work_items_.pop(trd))
            {
                --src->work_items_count_;

#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
                if (maintain_queue_wait_times) {
                    std::uint64_t now = util::high_resolution_clock::now();
                    src->work_items_wait_ += now - util::get<1>(*trd);
                    ++src->work_items_wait_count_;
                    util::get<1>(*trd) = now;
                }
#endif

                bool finished = count == ++work_items_count_;
                work_items_.push(trd);
                if (finished)
                    break;
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

#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
            thread_description* tdesc;
            if (0 != work_items_count && work_items_.pop(tdesc, steal))
            {
                --work_items_count_;

                if (maintain_queue_wait_times) {
                    work_items_wait_ += util::high_resolution_clock::now() -
                        util::get<1>(*tdesc);
                    ++work_items_wait_count_;
                }

                thrd = util::get<0>(*tdesc);
                delete tdesc;

                return true;
            }
#else
            if (0 != work_items_count && work_items_.pop(thrd, steal))
            {
                --work_items_count_;
                return true;
            }
#endif
            return false;
        }

        /// Schedule the passed thread
        void schedule_thread(threads::thread_data* thrd, bool other_end = false)
        {
            ++work_items_count_;
#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
            work_items_.push(new thread_description(
                thrd, util::high_resolution_clock::now()), other_end);
#else
            work_items_.push(thrd, other_end);
#endif
        }

        /// Destroy the passed thread as it has been terminated
        void destroy_thread(threads::thread_data* thrd, std::int64_t& busy_count)
        {
            HPX_ASSERT(&thrd->get_queue<thread_queue>() == this);
            terminated_items_.push(thrd);

            std::int64_t count = ++terminated_items_count_;
            if (count > max_terminated_threads)
            {
                cleanup_terminated(true);   // clean up all terminated threads
            }
        }

        ///////////////////////////////////////////////////////////////////////
        /// Return the number of existing threads with the given state.
        std::int64_t get_thread_count(thread_state_enum state = unknown) const
        {
            if (terminated == state)
                return terminated_items_count_;

            if (unknown == state)
                return thread_map_count_ - terminated_items_count_;

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
        }

        ///////////////////////////////////////////////////////////////////////
        void abort_all_suspended_threads()
        {
            std::lock_guard<mutex_type> lk(mtx_);
            thread_map_type::iterator end =  thread_map_.end();
            for (thread_map_type::iterator it = thread_map_.begin();
                 it != end; ++it)
            {
                if ((*it)->get_state().state() == suspended)
                {
                    (*it)->set_state(pending, wait_abort);
                    schedule_thread((*it).get());
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
                    "thread_queue::iterate_threads",
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
        mutable mutex_type mtx_;            // mutex protecting the members

        thread_map_type thread_map_;        // mapping of thread id's to HPX-threads
        std::atomic<std::int64_t> thread_map_count_; // overall count of work items

        work_items_type work_items_;        // list of active work items
        std::atomic<std::int64_t> work_items_count_; // count of active work items

#ifdef HPX_HAVE_THREAD_QUEUE_WAITTIME
        std::atomic<std::int64_t> work_items_wait_; // overall wait time of work items
        std::atomic<std::int64_t> work_items_wait_count_; // overall number of
                                                          // work items in queue
#endif
        terminated_items_type terminated_items_;    // list of terminated threads
        std::atomic<std::int64_t> terminated_items_count_; // count of terminated items

        std::size_t max_count_;     // maximum number of existing HPX-threads

        thread_heap_type thread_heap_small_;
        thread_heap_type thread_heap_medium_;
        thread_heap_type thread_heap_large_;
        thread_heap_type thread_heap_huge_;

#ifdef HPX_HAVE_THREAD_CREATION_AND_CLEANUP_RATES
        std::uint64_t add_new_time_;
        std::uint64_t cleanup_terminated_time_;
#endif

#ifdef HPX_HAVE_THREAD_STEALING_COUNTS
        // # of times our associated worker-thread couldn't find work in work_items
        std::atomic<std::int64_t> pending_misses_;

        // # of times our associated worker-thread looked for work in work_items
        std::atomic<std::int64_t> pending_accesses_;

        // count of work_items stolen from this queue
        std::atomic<std::int64_t> stolen_from_pending_;
        // count of new_tasks stolen from this queue
        std::atomic<std::int64_t> stolen_from_staged_;
        // count of work_items stolen to this queue from other queues
        std::atomic<std::int64_t> stolen_to_pending_;
        // count of new_tasks stolen to this queue from other queues
        std::atomic<std::int64_t> stolen_to_staged_;
#endif

        util::block_profiler<add_new_tag> add_new_logger_;
    };

    ///////////////////////////////////////////////////////////////////////////
    template <typename Mutex, typename PendingQueuing, typename StagedQueuing,
        typename TerminatedQueuing>
    util::internal_allocator<threads::thread_data> thread_queue<Mutex,
        PendingQueuing, StagedQueuing, TerminatedQueuing>::thread_alloc_;

    template <typename Mutex, typename PendingQueuing, typename StagedQueuing,
        typename TerminatedQueuing>
    util::internal_allocator<typename thread_queue<Mutex, PendingQueuing,
            StagedQueuing, TerminatedQueuing>::task_description>
        thread_queue<Mutex, PendingQueuing, StagedQueuing,
            TerminatedQueuing>::task_description_alloc_;
}}}

#endif

