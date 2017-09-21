//  Copyright (c) 2007-2016 Hartmut Kaiser
//  Copyright (c) 2012-2013 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/runtime/threads/topology.hpp>

#include <hpx/compat/thread.hpp>
#include <hpx/error_code.hpp>
#include <hpx/throw_exception.hpp>
#include <hpx/runtime/threads/policies/topology.hpp>
#include <hpx/runtime.hpp>
#include <hpx/util/static.hpp>

#include <cstddef>
#include <inttypes.h>

#if defined(__ANDROID__) && defined(ANDROID)
#include <cpu-features.h>
#endif

#if defined(__bgq__)
#include <hwi/include/bqc/A2_inlines.h>
#endif

#if defined(_POSIX_VERSION)
#include <sys/syscall.h>
#include <sys/resource.h>
#endif

#if !defined(HPX_HAVE_HWLOC)

namespace hpx { namespace threads
{
    mask_type noop_topology::empty_mask =
        mask_type(noop_topology::hardware_concurrency());
}}

#else
#include <hwloc.h>
#include <hpx/exception.hpp>

namespace hpx { namespace threads { namespace detail
{
    std::size_t hwloc_hardware_concurrency()
    {
        threads::topology& top = threads::create_topology();
        return top.get_number_of_pus();
    }
}}}
#endif

namespace hpx { namespace threads
{
    ///////////////////////////////////////////////////////////////////////////
    mask_type topology::get_service_affinity_mask(
        mask_cref_type used_processing_units, error_code& ec) const
    {
        // We bind the service threads to the first NUMA domain. This is useful
        // as the first NUMA domain is likely to have the PCI controllers etc.
        mask_cref_type machine_mask = this->get_numa_node_affinity_mask(0, ec);
        if (ec || !any(machine_mask))
            return mask_type();

        if (&ec != &throws)
            ec = make_success_code();

        mask_type res = ~used_processing_units & machine_mask;

        return (!any(res)) ? machine_mask : res;
    }

    void print_rlimit(struct rlimit *r, const char *name)
    {
        int64_t cur;                /* Soft limit */
        int64_t max;                /* Hard limit */
        cur = r->rlim_cur;
        max = r->rlim_max;
        printf("RLIMIT_%s\t:rlim_cur => %" PRId64
                        ", :rlim_max => %" PRId64 "\n", name, cur, max);
    }

    void set_sched_param()
    {
        const char *sched_policy[] = {
        "SCHED_OTHER",
        "SCHED_FIFO",
        "SCHED_RR",
        "SCHED_BATCH"
        };
        struct sched_param sp = {
            .sched_priority = 90
        };
        pid_t pid = getpid();
        printf("pid=(%d)\n",pid);
        sched_setscheduler(pid, SCHED_RR, &sp);
        printf("Scheduler Policy is %s.\n", sched_policy[sched_getscheduler(pid)]);

//        pthread_t tid ;
//        pthread_create(&tid , NULL, Thread1 , (void*)(long)3);
//        pthread_create(&tid , NULL, Thread2 , (void*)(long)3);
//        pthread_create(&tid , NULL, Thread3 , (void*)(long)3);
//        while(1)
//            sleep(100);
    }

    bool topology::set_mempolicy(mask_cref_type cpu_mask, error_code& ec) const
    {
        set_mem_policy_interleaved(cpu_mask, ec);
        return true;
    }

    bool topology::reduce_thread_priority(error_code& ec) const
    {

//        set_sched_param();
/*
        struct rlimit rlim;
        int resources[] = {RLIMIT_CORE, RLIMIT_CPU, RLIMIT_DATA, RLIMIT_FSIZE,
                           RLIMIT_MEMLOCK, RLIMIT_NOFILE, RLIMIT_NPROC, RLIMIT_RSS,
                           RLIMIT_STACK, RLIMIT_NICE};
        const char *names[] = {"CORE", "CPU", "DATA", "FSIZE",
                               "MEMLOCK", "NOFILE", "NPROC", "RSS",
                               "STACK", "NICE"};
        int n = sizeof(resources)/sizeof(resources[0]);
        int i;
        for (i = 0; i < n; i++) {
            getrlimit(resources[i], &rlim);
            print_rlimit(&rlim, names[i]);
        }
*/

#ifdef HPX_HAVE_NICE_THREADLEVEL
#if defined(__linux__) && !defined(__ANDROID__) && !defined(__bgq__)
        pid_t tid;
        tid = syscall(SYS_gettid);
        if (setpriority(PRIO_PROCESS, tid, 19))
        {
            HPX_THROWS_IF(ec, no_success, "topology::reduce_thread_priority",
                "setpriority returned an error");
            return false;
        }
#elif defined(WIN32) || defined(_WIN32) || defined(__WIN32__)

        if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST))
        {
            HPX_THROWS_IF(ec, no_success, "topology::reduce_thread_priority",
                "SetThreadPriority returned an error");
            return false;
        }
#elif defined(__bgq__)
        ThreadPriority_Low();
#endif
#endif
        return true;
    }

    topology const& get_topology()
    {
        runtime* rt = get_runtime_ptr();
        if (rt == nullptr)
        {
            HPX_THROW_EXCEPTION(invalid_status, "hpx::threads::get_topology",
                "the hpx runtime system has not been initialized yet");
        }
        return rt->get_topology();
    }

    ///////////////////////////////////////////////////////////////////////////
    struct hardware_concurrency_tag {};

    struct hw_concurrency
    {
        hw_concurrency()
#if defined(__ANDROID__) && defined(ANDROID)
          : num_of_cores_(::android_getCpuCount())
#elif defined(HPX_HAVE_HWLOC)
          : num_of_cores_(detail::hwloc_hardware_concurrency())
#else
          : num_of_cores_(compat::thread::hardware_concurrency())
#endif
        {
            if (num_of_cores_ == 0)
                num_of_cores_ = 1;
        }

        std::size_t num_of_cores_;
    };

    std::size_t hardware_concurrency()
    {
        util::static_<hw_concurrency, hardware_concurrency_tag> hwc;
        return hwc.get().num_of_cores_;
    }
}}

