//  Copyright (c) 2014-2017 John Biddiscombe
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#define hexbyte(p)    p
#define hexpointer(p) p
#define hexuint32(p)  p
#define hexuint64(p)  p
#define hexlength(p)  p
#define binary8(p)    p
#define hexnumber(p)  p
#define decnumber(p)  p
#define ipaddress(p)  p

#define LOG_DEBUG_MSG(x)
#define LOG_INFO_MSG(x)
#define LOG_WARN_MSG(x)
#define LOG_TRACE_MSG(x)
#define LOG_FORMAT_MSG(x)
#define LOG_DEVEL_MSG(x)
#define LOG_TIMED_INIT(name)
#define LOG_TIMED_MSG(name, level, delay, x)
#define LOG_TIMED_BLOCK(name, level, delay, x)
#define LOG_ERROR_MSG(x)                                   \
    std::cout << "00: <ERROR> " << x << " " << __FILE__    \
              << " " << std::dec << __LINE__ << std::endl;
#define LOG_FATAL_MSG(x) LOG_ERROR_MSG(x)

#define LOG_EXCLUSIVE(x)

#define FUNC_START_DEBUG_MSG
#define FUNC_END_DEBUG_MSG

