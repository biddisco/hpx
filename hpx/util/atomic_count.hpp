//  Copyright 2013 Peter Dimov
//  Copyright (c) 2016 Agustin Berge
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_UTIL_ATOMIC_COUNT_HPP
#define HPX_UTIL_ATOMIC_COUNT_HPP

#include <hpx/config.hpp>

#include <atomic>

namespace hpx { namespace util
{
    class atomic_count
    {
    public:
        HPX_NON_COPYABLE(atomic_count);

    public:
        explicit atomic_count(long value)
          : value_(value)
        {}

        atomic_count& operator=(long value)
        {
            value_.store(value);
            return *this;
        }

        long operator++()
        {
            return value_.fetch_add(1) + 1;
        }

        long operator--()
        {
            return value_.fetch_sub(1) - 1;
        }

        atomic_count& operator+=(long n)
        {
            value_.fetch_add(n);
            return *this;
        }

        atomic_count& operator-=(long n)
        {
            value_.fetch_sub(n);
            return *this;
        }

        operator long() const
        {
            return value_.load();
        }

    private:
        std::atomic<long> value_;
    };
}}

#endif /*HPX_UTIL_ATOMIC_COUNT_HPP*/
