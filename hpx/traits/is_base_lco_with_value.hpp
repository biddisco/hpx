//  Copyright (c) 2016 John Biddiscombe
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 
#ifndef IS_BASE_LCO_WITH_VALUE_HPP
#define IS_BASE_LCO_WITH_VALUE_HPP
  
#include <hpx/config.hpp>

#include <cstddef>
#include <type_traits>

namespace hpx { namespace traits
{  
    template <typename T> 
    struct is_base_lco_with_value : std::false_type {}; 
}}
 
#endif
 