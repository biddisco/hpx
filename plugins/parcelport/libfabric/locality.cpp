//  Copyright (c) 2015 John Biddiscombe
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/runtime/parcelset/locality.hpp>
#include <hpx/serialization/serialize.hpp>
#include <hpx/serialization/array.hpp>
#include <plugins/parcelport/libfabric/parcelport_libfabric.hpp>
#include <plugins/parcelport/libfabric/controller.hpp>
#include <hpx/distributed/runtime.hpp>
#include <hpx/runtime_distributed.hpp>
//
#include <utility>
#include <cstring>
#include <cstdint>
#include <array>
#include <rdma/fabric.h>

namespace hpx {
namespace parcelset {
namespace policies {
namespace libfabric
{

    // when loading a locality - it will have been transmitted from another node
    // and the fi_address will not be valid, so we must look it up and put
    // the correct value from this node's libfabric address vector.
    // this is only called at bootstrap time, so do not worry about overheads
    void locality::load(serialization::input_archive & ar) {
        ar >> data_;
        ar >> fi_address_;
        parcelset::parcelhandler &ph
                = hpx::get_runtime_distributed().get_parcel_handler();
        std::shared_ptr<parcelset::parcelport> pp
                = ph.get_bootstrap_parcelport();
        std::shared_ptr<libfabric::parcelport> lf
                = std::dynamic_pointer_cast<libfabric::parcelport>(pp);
        if (!lf->controller_->resolve_address(*this)) {
            HPX_THROW_EXCEPTION(bad_parameter, "libfabric::locality",
                "serialization load lookup error");
        }
    }

}}}}

