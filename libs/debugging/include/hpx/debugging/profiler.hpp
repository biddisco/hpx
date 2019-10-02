#ifndef HPX_DEBUG_PROFILER_HPP
#define HPX_DEBUG_PROFILER_HPP

#include <hpx/config.hpp>
#include <hpx/runtime/get_worker_thread_num.hpp>
//
#include <chrono>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>
#include <iostream>

namespace hpx { namespace debug {

    namespace detail {

        template <class T>
        inline std::string string(T el)
        {
            return std::to_string(el);
        }
        inline std::string string(const char* el)
        {
            return std::string(el);
        }
        inline std::string string(std::string el)
        {
            return el;
        }

        using TimeType = unsigned long long;

        class task_profile_data
        {
        public:
            task_profile_data(const std::string& task_name,
                const std::string& task_group_name, int thread_id_start,
                TimeType time_start, int thread_id_end, TimeType time_end)
              : task_name_(task_name)
              , task_group_name_(task_group_name)
              , thread_id_start_(thread_id_start)
              , time_start_(time_start)
              , thread_id_end_(thread_id_end)
              , time_end_(time_end)
            {
            }

            template <class Out>
            void write(Out& out_stream)
            {
                out_stream << task_name_ << ", ";
                out_stream << task_group_name_ << ", ";
                out_stream << thread_id_start_ << ", ";
                out_stream << time_start_ << ", ";
                out_stream << thread_id_end_ << ", ";
                out_stream << time_end_ << std::endl;
            }

        private:
            std::string task_name_;
            std::string task_group_name_;
            int thread_id_start_;
            TimeType time_start_;
            int thread_id_end_;
            TimeType time_end_;
        };

        class thread_profiler
        {
        public:
            void add(const std::string& task_name,
                const std::string& task_group_name, int thread_id_start,
                TimeType time_start, int thread_id_end, TimeType time_end)
            {
                task_profiles.emplace_back(task_name, task_group_name,
                    thread_id_start, time_start, thread_id_end, time_end);
            }

            template <class Out>
            void write(Out& out_stream)
            {
                for (auto& task_profile : task_profiles)
                    task_profile.write(out_stream);
            }

        private:
            std::deque<task_profile_data> task_profiles;
        };

#if defined(HPX_NO_PROFILING)
        class HPX_EXPORT profiler
        {
        public:
            profiler() {    }
            ~profiler() {}
            void setOutputFilename(std::string output_name) {}
            void add(const std::string& task_name,
                const std::string& task_group_name, int thread_id_start,
                detail::TimeType time_start, int thread_id_end,
                detail::TimeType time_end)
            {}
            detail::TimeType getTime()
            {
                return {};
            }
        };
#else
        class HPX_EXPORT profiler
        {
        public:
            profiler()
              : filename_("profile_" + std::to_string(getpid()) + ".txt")
              , profilers_(257)
            {
                std::cout << "Creating profiler : " << filename_ << std::endl;
            }

            ~profiler()
            {
                std::ofstream fout(filename_);
                for (auto& profiler : profilers_)
                    profiler.write(fout);
            }

            void setOutputFilename(std::string output_name)
            {
                std::cout << "Setting filename to " << output_name << std::endl;
                filename_ = output_name;
            }

            void add(const std::string& task_name,
                const std::string& task_group_name, int thread_id_start,
                detail::TimeType time_start, int thread_id_end,
                detail::TimeType time_end)
            {
                profilers_[thread_id_end + 1].add(task_name, task_group_name,
                    thread_id_start, time_start, thread_id_end, time_end);
            }

            detail::TimeType getTime()
            {
                return std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::high_resolution_clock::now()
                        .time_since_epoch())
                    .count();
            }

        private:
            std::string filename_;
            std::vector<detail::thread_profiler> profilers_;
        };
#endif
    }    // namespace detail

    // -----------------------------------------------------
    HPX_EXPORT std::unique_ptr<hpx::debug::detail::profiler>& get_profiler();
    HPX_EXPORT void delete_profiler();

#if defined(HPX_NO_PROFILING)

  class task_profiler {
    public:
    task_profiler(std::string task_name, std::string task_group_name) {}
  };
#else
    class task_profiler
    {
    public:
        task_profiler(std::string task_name, std::string task_group_name)
          : task_name_(task_name)
          , task_group_name_(task_group_name)
          , thread_id_start_(getThreadId())
          , time_start_(get_profiler()->getTime())
          /*, apex_profiler(task_name.c_str())*/
        {
        }

        ~task_profiler()
        {
            int thread_id_end = getThreadId();
            detail::TimeType time_end = get_profiler()->getTime();

            get_profiler()->add(task_name_, task_group_name_,
                thread_id_start_, time_start_, thread_id_end, time_end);
        }

        int getThreadId()
        {
            return hpx::get_worker_thread_num();
        }

    private:
        std::string task_name_;
        std::string task_group_name_;
        int thread_id_start_;
        detail::TimeType time_start_;
        // hpx::util::annotate_function apex_profiler;
    };
#endif

#if defined(HPX_NO_PROFILING)

    class Mpiprofiler
    {
    public:
        Mpiprofiler(std::string task_name, std::string task_group_name) {}
    };
#else
    class Mpiprofiler
    {
    public:
        Mpiprofiler(std::string task_name, std::string task_group_name)
          : task_name_(task_name)
          , task_group_name_(task_group_name)
          , thread_id_start_(-1)
          , time_start_(get_profiler()->getTime())
        {
        }

        ~Mpiprofiler()
        {
            int thread_id_end = -1;
            detail::TimeType time_end = get_profiler()->getTime();

            get_profiler()->add(task_name_, task_group_name_,
                thread_id_start_, time_start_, thread_id_end, time_end);
        }

    private:
        std::string task_name_;
        std::string task_group_name_;
        int thread_id_start_;
        detail::TimeType time_start_;
    };
#endif

    inline std::string createName(std::string s)
    {
        return s;
    }

    // std::string createName(std::string s, T1 e1, T2 e2, T3 e3, ...)
    // Where Ti is either string, char* or a numerical type.
    template <class T, class... Args>
    inline std::string createName(std::string s, T el)
    {
        return s /*+ " " + detail::string(el)*/;
    }

    template <class T, class... Args>
    inline std::string createName(std::string s, T el, Args... args)
    {
        return createName(s /*+ " " + detail::string(el), args...*/);
    }

    template <class T, class... Args>
    inline std::string createNameHP(std::string s, T el, Args... args)
    {
        return createName(s, el, args...);
    }
}}    // namespace hpx::debug

#endif    // HPX_DEBUG_PROFILER_HPP
