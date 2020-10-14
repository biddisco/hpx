// Copyright (c) 2020 John Biddiscombe
// Copyright (c) 2020 Nikunj Gupta
// Copyright (c) 2016 Thomas Heller
//
// SPDX-License-Identifier: BSL-1.0
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <hpx/include/lcos.hpp>

#include <array>
#include <cstddef>

template <typename T>
struct communicator
{
    enum neighbour {
        left = 0,
        right = 1,
    };

    using channel_type = hpx::lcos::channel<T>;

    // rank: our rank in the system
    // num: number of participating partners
    communicator(std::size_t rank, std::size_t size)
    {
        static const char* left_name = "/ch/L/";
        static const char* right_name = "/ch/R/";

        // Only set neighbour channels if we have more than one partner
        if (size > 1)
        {
            // We have a left neighbour if our rank is greater than zero.
            // and a right neighbour if we are not the last
            if (rank > 0)
            {
                // get ID of channel using name + (rank-1) to receive from left
                recv[left] = hpx::find_from_basename<
                                channel_type>(right_name, rank - 1);

                // Create and register our left channel for sending
                send[left] = channel_type(hpx::find_here());
                hpx::register_with_basename(left_name, send[left], rank);
            }
            if (rank < size - 1)
            {
                // get ID of channel using name + (rank+1) ro receive from right
                recv[right] = hpx::find_from_basename<
                                channel_type>(left_name, rank + 1);

                // Create and register our right channel for sending
                send[right] = channel_type(hpx::find_here());
                hpx::register_with_basename(right_name, send[right], rank);
            }
        }
    }

    bool has_neighbour(neighbour n) const
    {
        return recv[n] && send[n];
    }

    // send data to neighbour
    void set(neighbour n, T t, std::size_t step)
    {
        send[n].set(t, step);
    }

    // return a future to data from neighbour
    hpx::future<T> get(neighbour n, std::size_t step)
    {
        return recv[n].get(hpx::launch::async, step);
    }

    std::array<hpx::lcos::channel<T>, 2> recv;
    std::array<hpx::lcos::channel<T>, 2> send;
};
