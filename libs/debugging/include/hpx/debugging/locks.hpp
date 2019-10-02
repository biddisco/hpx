// Copyright (C) 2019 John Biddiscombe
//
//  SPDX-License-Identifier: BSL-1.0
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at

#ifndef HPX_DEBUG_LOCKS_HPP
#define HPX_DEBUG_LOCKS_HPP

//
#include <hpx/debugging/print.hpp>
#include <mutex>

namespace hpx { namespace debug {

    namespace detail {
        template<typename Mutex>
        struct scoped_lock: std::lock_guard<Mutex>
        {
            hpx::debug::enable_print<true> print;
            //
            scoped_lock(Mutex &m) : std::lock_guard<Mutex>(m), print("SCOPED_")
            {
                print.debug("Creating scoped_lock RAII");
            }

            ~scoped_lock()
            {
                print.debug("Destroying scoped_lock RAII");
            }
        };

        template<typename Mutex>
        struct unique_lock: std::unique_lock<Mutex>
        {
            hpx::debug::enable_print<true> print;
            //
            unique_lock(Mutex &m)
                : std::unique_lock<Mutex>(m), print("UNIQUE_")
            {
                print.debug("Creating unique_lock RAII");
            }

            unique_lock(Mutex& m, std::try_to_lock_t t)
                : std::unique_lock<Mutex>(m, t), print("UNIQUE_")
            {
                if (this->owns_lock()) {
                    print.debug("Creating unique_lock (try_to_lock_t) RAII");
                }
            }

            unique_lock(Mutex& m, std::defer_lock_t t)
                : std::unique_lock<Mutex>(m, t), print("UNIQUE_")
            {
                // print.debug("Creating unique_lock (defer_lock_t) RAII");
            }

            ~unique_lock()
            {
                if (this->owns_lock()) {
                    print.debug("Destroying unique_lock RAII");
                }
            }
        };
    }

    // when false, locks revert to normal locks
    template <typename Mutex, bool debug=false>
    struct lock
    {
        using scoped_lock = std::lock_guard<Mutex>;
        using unique_lock = std::unique_lock<Mutex>;
    };

    // when true each lock/unlock event produces a debug output
    template <typename Mutex>
    struct lock<Mutex, true>
    {
        using scoped_lock = detail::scoped_lock<Mutex>;
        using unique_lock = detail::unique_lock<Mutex>;
    };

}}

#endif

