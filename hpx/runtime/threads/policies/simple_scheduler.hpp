//  Copyright (c) 2019 Mikael Simberg
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_THREADMANAGER_SCHEDULING_SIMPLE_SCHEDULER)
#define HPX_THREADMANAGER_SCHEDULING_SIMPLE_SCHEDULER

#include <hpx/config.hpp>

#include <hpx/compat/mutex.hpp>
#include <hpx/runtime/threads/policies/lockfree_queue_backends.hpp>
#include <hpx/runtime/threads/policies/scheduler_base.hpp>
#include <hpx/util/lockfree/concurrentqueue.hpp>
#include <hpx/runtime/threads/thread_data.hpp>
#include <hpx/runtime/threads/topology.hpp>
#include <hpx/runtime/threads_fwd.hpp>
#include <hpx/throw_exception.hpp>
#include <hpx/util/assert.hpp>
#include <hpx/util/logging.hpp>
#include <hpx/util_fwd.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include <hpx/config/warnings_prefix.hpp>

#include <hpx/config.hpp>
#include <hpx/compat/mutex.hpp>
#include <hpx/error_code.hpp>
#include <hpx/runtime/config_entry.hpp>
#include <hpx/runtime/threads/policies/lockfree_queue_backends.hpp>
#include <hpx/runtime/threads/thread_data.hpp>
#include <hpx/throw_exception.hpp>
#include <hpx/util/assert.hpp>
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

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace threads { namespace policies {
    template <typename T = void>
    alignas(64) struct simple_thread_queue
    {
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

        using task_description =
            util::tuple<thread_init_data, thread_state_enum>;

        void create_thread_object(threads::thread_id_type& thrd,
            threads::thread_init_data& data, thread_state_enum state)
        {
            if (state == pending_do_not_schedule || state == pending_boost)
            {
                state = pending;
            }

            // std::cout << "thread_heap_.size() = " << thread_heap_.size()
            //           << std::endl;

            // Check for an unused thread object.
            if (!thread_heap_.empty())
            {
                // Take ownership of the thread object and rebind it.
                thrd = std::move(thread_heap_[thread_heap_.size() - 1]);
                thread_heap_.pop_back();
                thrd->rebind(data, state);
            }
            else
            {
                // Allocate a new thread object.
                // threads::thread_data* p = thread_alloc_.allocate(1);
                // new (p) threads::thread_data(data, this, state);
                // thrd = thread_id_type(p);
                thrd =
                    thread_id_type(new threads::thread_data(data, this, state));
            }
        }

        static util::internal_allocator<threads::thread_data> thread_alloc_;

        simple_thread_queue(std::size_t thread_number, std::size_t num_threads)
          : min_tasks_to_steal_pending(0)
          , min_tasks_to_steal_staged(0)
          , min_add_new_count(0)
          , max_add_new_count(0)
          , max_delete_count(0)
          , max_terminated_threads(0)
          , local_queue_(128)
          , local_queue_size_(0)
          , shared_queue_(128)
          , shared_queue_size_(0)
          , terminated_items_()
          , terminated_items_count_(0)
          , thread_heap_()
          , thread_number_(thread_number)
          , num_threads_(num_threads)
          , queue_consumer_tokens_()
          , queue_producer_tokens_()
        {
            terminated_items_.reserve(128);
            thread_heap_.reserve(128);

            for (std::size_t i = 0; i < num_threads_; ++i)
            {
                queue_consumer_tokens_.emplace_back(shared_queue_);
                queue_producer_tokens_.emplace_back(shared_queue_);
            }
        }

        static void deallocate(threads::thread_data* p)
        {
            using threads::thread_data;
            p->~thread_data();
            thread_alloc_.deallocate(p, 1);
        }

        ~simple_thread_queue()
        {
            for (auto t : thread_heap_)
            {
                delete t.get();
                // deallocate(t.get());
            }
        }

        ///////////////////////////////////////////////////////////////////////
        // This returns the current length of the queues (work items and new items)
        std::int64_t get_queue_length() const
        {
            return local_queue_size_ + shared_queue_size_;
        }

        // This returns the current length of the pending queue
        std::int64_t get_pending_queue_length() const
        {
            return local_queue_size_;
        }

        // This returns the current length of the staged queue
        std::int64_t get_staged_queue_length(
            std::memory_order order = std::memory_order_seq_cst) const
        {
            return shared_queue_size_.load(order);
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
            if (id)
            {
                *id = invalid_thread_id;
            }

            ++shared_queue_size_;
            if (hpx::threads::get_self_ptr() != nullptr)
            {
                shared_queue_.enqueue(
                    // queue_producer_tokens_[hpx::get_worker_thread_num()],
                    task_description(std::move(data), initial_state));
            }
            else
            {
                shared_queue_.enqueue(
                    task_description(std::move(data), initial_state));
            }

            // std::ostringstream s;
            // s << "simple_thread_queue::create_thread on "
            //   << thread_number_ << "\n";
            // std::cout << s.str();

            if (&ec != &throws)
                ec = make_success_code();
        }

        bool get_next_thread(threads::thread_data*& thrd) HPX_HOT
        {
            if (local_queue_.try_dequeue(thrd))
            {
                // std::ostringstream s;
                // s << "simple_thread_queue::get_next_thread on "
                //   << thread_number_ << "\n";
                // std::cout << s.str();

                --local_queue_size_;
                return true;
            }

            return false;
        }

        /// Schedule the passed thread
        void schedule_thread(threads::thread_data* thrd)
        {
            ++local_queue_size_;
            local_queue_.enqueue(thrd);

            // std::ostringstream s;
            // s << "simple_thread_queue::schedule_thread on " << thread_number_
            //   << "\n";
            // std::cout << s.str();
        }

        /// Destroy the passed thread as it has been terminated
        void terminate_thread(threads::thread_data* thrd)
        {
            thread_heap_.emplace_back(thrd);
        }

        ///////////////////////////////////////////////////////////////////////
        /// Return the number of existing threads with the given state.
        std::int64_t get_thread_count(thread_state_enum state = unknown) const
        {
            if (terminated == state)
                return terminated_items_count_;

            if (staged == state)
                return shared_queue_size_;

            if (unknown == state)
                return local_queue_size_ + shared_queue_size_ -
                    terminated_items_count_;

            std::int64_t num_threads = 0;
            return num_threads;
        }

        ///////////////////////////////////////////////////////////////////////
        void abort_all_suspended_threads() {}

        bool enumerate_threads(
            util::function_nonser<bool(thread_id_type)> const& f,
            thread_state_enum state = unknown) const
        {
            return true;
        }

        /// This is a function which gets called periodically by the thread
        /// manager to allow for maintenance tasks to be executed in the
        /// scheduler. Returns true if the OS thread calling this function
        /// has to be terminated (i.e. no more work has to be done).
        inline bool wait_or_add_new(std::size_t& added,
            simple_thread_queue* other_queue = nullptr)
        {
            if (other_queue == nullptr)
            {
                other_queue = this;
            }

            // int add_count =
            //     std::max(std::size_t(10), shared_queue_.size_approx() / 100);
            // task_description task;
            // while (add_count-- && other_queue->shared_queue_.try_dequeue(task))
            // {
            //     threads::thread_init_data& data = util::get<0>(task);
            //     thread_state_enum state = util::get<1>(task);
            //     threads::thread_id_type thrd;

            //     create_thread_object(thrd, data, state);

            //     --other_queue->shared_queue_size_;

            //     if (state == pending)
            //     {
            //         ++added;
            //         schedule_thread(thrd.get());
            //     }

            //     HPX_ASSERT(&thrd->get_queue<simple_thread_queue>() == this);
            // }

            constexpr std::size_t num_tasks_dequeue = 10;
            task_description tasks[num_tasks_dequeue];
            std::size_t num_tasks_dequeued =
                other_queue->shared_queue_.try_dequeue_bulk(
                    // other_queue->queue_consumer_tokens_[thread_number_],
                    tasks,
                    num_tasks_dequeue);
            other_queue->shared_queue_size_ -= num_tasks_dequeued;
            for (std::size_t task_index = 0; task_index < num_tasks_dequeued;
                 ++task_index)
            {
                task_description task = std::move(tasks[task_index]);
                threads::thread_init_data& data = util::get<0>(task);
                thread_state_enum state = util::get<1>(task);
                threads::thread_id_type thrd;

                create_thread_object(thrd, data, state);

                if (state == pending)
                {
                    ++added;
                    // TODO: Also do in bulk.
                    schedule_thread(thrd.get());
                }

                HPX_ASSERT(&thrd->get_queue<simple_thread_queue>() == this);
            }

            return false;
        }

        ///////////////////////////////////////////////////////////////////////
        bool dump_suspended_threads(
            std::size_t num_thread, std::int64_t& idle_loop_count, bool running)
        {
            return false;
        }

        ///////////////////////////////////////////////////////////////////////
        void on_start_thread(std::size_t num_thread) {}
        void on_stop_thread(std::size_t num_thread) {}
        void on_error(std::size_t num_thread, std::exception_ptr const& e) {}

    private:
        moodycamel::ConcurrentQueue<thread_data*> local_queue_;
        std::atomic<std::int64_t> local_queue_size_;

        moodycamel::ConcurrentQueue<task_description> shared_queue_;
        std::atomic<std::int64_t> shared_queue_size_;

        std::vector<thread_data*> terminated_items_;
        std::atomic<std::int64_t> terminated_items_count_;

        std::vector<thread_id_type> thread_heap_;
        std::size_t thread_number_;
        std::size_t num_threads_;
        std::vector<moodycamel::ConsumerToken> queue_consumer_tokens_;
        std::vector<moodycamel::ProducerToken> queue_producer_tokens_;
    };

    ///////////////////////////////////////////////////////////////////////////
    template <typename T>
    util::internal_allocator<threads::thread_data>
        simple_thread_queue<T>::thread_alloc_;
}}}

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace threads { namespace policies {
    template <typename = void>
    class HPX_EXPORT simple_scheduler : public scheduler_base
    {
    public:
        using thread_queue_type = simple_thread_queue<>;

        struct init_parameter
        {
            init_parameter()
              : num_queues_(1)
              , description_("simple_scheduler")
            {
            }

            init_parameter(std::size_t num_queues,
                char const* description = "simple_scheduler")
              : num_queues_(num_queues)
              , description_(description)
            {
                // std::ostringstream s;
                // s << "simple_scheduler_ctor\n"
                //   << "    num_queues = " << num_queues << "\n"
                //   << "    description = \"" << description_ << "\"\n";
                // std::cout << s.str();
            }

            std::size_t num_queues_;
            char const* description_;
        };

        using init_parameter_type = init_parameter;

        simple_scheduler(init_parameter_type const& init)
          : scheduler_base(init.num_queues_, init.description_)
          , queues_(init.num_queues_)
          , curr_queue_(0)
        {
        }

        virtual ~simple_scheduler()
        {
            for (std::size_t i = 0; i != queues_.size(); ++i)
                delete queues_[i];
        }

        bool numa_sensitive() const override
        {
            return false;
        }
        virtual bool has_thread_stealing() const override
        {
            return true;
        }

        static std::string get_scheduler_name()
        {
            return "simple_scheduler";
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

        ///////////////////////////////////////////////////////////////////////
        void abort_all_suspended_threads() override {}

        void create_thread(thread_init_data& data, thread_id_type* id,
            thread_state_enum initial_state, bool /* run_now */,
            error_code& ec) override
        {
            std::size_t num_thread =
                data.schedulehint.mode == thread_schedule_hint_mode_thread ?
                data.schedulehint.hint :
                std::size_t(-1);

            std::size_t queues_size = queues_.size();

            if (std::size_t(-1) == num_thread)
            {
                ++curr_queue_;
                num_thread = curr_queue_ % queues_size;
            }
            else if (num_thread >= queues_size)
            {
                num_thread %= queues_size;
            }

            HPX_ASSERT(num_thread < queues_size);
            queues_[num_thread]->create_thread(data, id, initial_state, ec);
        }

        virtual bool get_next_thread(std::size_t num_thread, bool running,
            std::int64_t& idle_loop_count, threads::thread_data*& thrd) override
        {
            std::size_t queues_size = queues_.size();
            auto local_queue = queues_[num_thread];

            // First try local queue
            {
                HPX_ASSERT(num_thread < queues_size);

                bool result = local_queue->get_next_thread(thrd);

                if (result)
                {
                    // std::size_t added = 0;
                    // local_queue->wait_or_add_new(added, 1);
                    return true;
                }
            }

            // Then try to get items from shared local queue
            {
                std::size_t added = 0;
                if (local_queue->wait_or_add_new(added))
                {
                    if (local_queue->get_next_thread(thrd))
                    {
                        return true;
                    };
                }
            }

            // Then try shared remote queues
            // TODO: Do this properly for NUMA awareness.
            // NOTE: This is stupid! In other words, it's hardcoded for Piz
            // Daint multicore.
            if (num_thread < 18)
            {
                std::size_t max_queues = std::min(queues_size, std::size_t(18));
                for (std::size_t offset = 1; offset < max_queues; ++offset)
                {
                    const std::size_t i = (num_thread + offset) % max_queues;
                    auto remote_queue = queues_[i];
                    std::size_t added;
                    bool result =
                        local_queue->wait_or_add_new(added, remote_queue);
                    if (result)
                    {
                        result = local_queue->get_next_thread(thrd);
                        if (result)
                        {
                            return true;
                        };
                    }
                }
            }
            else
            {
                std::size_t max_queues =
                    std::min(queues_size - 18, std::size_t(18));
                std::size_t numa_offset = 18;
                for (std::size_t offset = 1; offset < max_queues; ++offset)
                {
                    const std::size_t i = numa_offset +
                        (((num_thread - 18) + offset) % max_queues);
                    auto remote_queue = queues_[i];
                    std::size_t added;
                    bool result =
                        local_queue->wait_or_add_new(added, remote_queue);
                    if (result)
                    {
                        result = local_queue->get_next_thread(thrd);
                        if (result)
                        {
                            return true;
                        };
                    }
                }
            }

            // Then try non-shared remote queues?

            return false;
        }

        void schedule_thread(threads::thread_data* thrd,
            threads::thread_schedule_hint schedulehint,
            bool allow_fallback = false,
            thread_priority priority = thread_priority_normal) override
        {
            std::size_t num_thread = std::size_t(-1);
            if (schedulehint.mode == thread_schedule_hint_mode_thread)
            {
                num_thread = schedulehint.hint;
            }

            std::size_t queues_size = queues_.size();

            if (std::size_t(-1) == num_thread)
            {
                ++curr_queue_;
                num_thread = curr_queue_ % queues_size;
            }
            else if (num_thread >= queues_size)
            {
                num_thread %= queues_size;
            }

            HPX_ASSERT(thrd->get_scheduler_base() == this);

            HPX_ASSERT(num_thread < queues_.size());

            // std::ostringstream s;
            // s << "simple_scheduler::schedule_thread on " << num_thread << "\n";
            // std::cout << s.str();

            queues_[num_thread]->schedule_thread(thrd);
        }

        void schedule_thread_last(threads::thread_data* thrd,
            threads::thread_schedule_hint schedulehint,
            bool allow_fallback = false,
            thread_priority priority = thread_priority_normal) override
        {
            schedule_thread(thrd, schedulehint, allow_fallback, priority);
        }

        void destroy_thread(
            threads::thread_data* thrd, std::int64_t& busy_count) override
        {
            HPX_ASSERT(thrd->get_scheduler_base() == this);
            // TODO: pass thread number
            // HPX_ASSERT(
            //     &thrd->get_queue<thread_queue_type>() == queues_[num_thread]);
            thrd->get_queue<thread_queue_type>().terminate_thread(thrd);
        }

        std::int64_t get_queue_length(
            std::size_t num_thread = std::size_t(-1)) const override
        {
            // Return queue length of one specific queue.
            std::int64_t count = 0;
            if (std::size_t(-1) != num_thread)
            {
                HPX_ASSERT(num_thread < queues_.size());

                return queues_[num_thread]->get_queue_length();
            }

            for (std::size_t i = 0; i != queues_.size(); ++i)
                count += queues_[i]->get_queue_length();

            return count;
        }

        std::int64_t get_thread_count(thread_state_enum state = unknown,
            thread_priority priority = thread_priority_default,
            std::size_t num_thread = std::size_t(-1),
            bool reset = false) const override
        {
            // Return thread count of one specific queue.
            std::int64_t count = 0;
            if (std::size_t(-1) != num_thread)
            {
                HPX_ASSERT(num_thread < queues_.size());

                switch (priority)
                {
                case thread_priority_default:
                case thread_priority_low:
                case thread_priority_normal:
                case thread_priority_boost:
                case thread_priority_high:
                case thread_priority_high_recursive:
                    return queues_[num_thread]->get_thread_count(state);

                default:
                case thread_priority_unknown:
                {
                    HPX_THROW_EXCEPTION(bad_parameter,
                        "simple_scheduler::get_thread_count",
                        "unknown thread priority value "
                        "(thread_priority_unknown)");
                    return 0;
                }
                }
                return 0;
            }

            // Return the cumulative count for all queues.
            switch (priority)
            {
            case thread_priority_default:
            case thread_priority_low:
            case thread_priority_normal:
            case thread_priority_boost:
            case thread_priority_high:
            case thread_priority_high_recursive:
            {
                for (std::size_t i = 0; i != queues_.size(); ++i)
                    count += queues_[i]->get_thread_count(state);
                break;
            }

            default:
            case thread_priority_unknown:
            {
                HPX_THROW_EXCEPTION(bad_parameter,
                    "simple_scheduler::get_thread_count",
                    "unknown thread priority value (thread_priority_unknown)");
                return 0;
            }
            }
            return count;
        }

        virtual bool wait_or_add_new(std::size_t num_thread, bool running,
            std::int64_t& idle_loop_count) override
        {
            return true;
        }

        ///////////////////////////////////////////////////////////////////////
        void on_start_thread(std::size_t num_thread) override
        {
            queues_[num_thread] =
                new thread_queue_type(num_thread, queues_.size());

            // std::ostringstream s;
            // s << "simple_scheduler::on_start_thread(" << num_thread << ")\n";
            // std::cout << s.str();
        }

        void on_stop_thread(std::size_t num_thread) override {}

        void on_error(
            std::size_t num_thread, std::exception_ptr const& e) override
        {
        }

        void reset_thread_distribution() override
        {
            curr_queue_ = 0;
        }

        bool enumerate_threads(
            util::function_nonser<bool(thread_id_type)> const& f,
            thread_state_enum state = unknown) const override
        {
            return true;
        }
        bool cleanup_terminated(bool delete_all) override
        {
            return true;
        }

        bool cleanup_terminated(
            std::size_t num_thread, bool delete_all) override
        {
            return true;
        }

    protected:
        std::vector<thread_queue_type*> queues_;
        std::size_t curr_queue_;
    };
}}}

#include <hpx/config/warnings_suffix.hpp>

#endif
