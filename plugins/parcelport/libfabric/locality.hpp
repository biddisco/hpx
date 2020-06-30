//  Copyright (c) 2015 John Biddiscombe
//
//  SPDX-License-Identifier: BSL-1.0
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <hpx/runtime/parcelset/locality.hpp>
#include <hpx/serialization/serialize.hpp>
#include <hpx/serialization/array.hpp>
#include <hpx/util/ios_flags_saver.hpp>
//
#include <utility>
#include <cstring>
#include <cstdint>
#include <array>
#include <rdma/fabric.h>

// Different providers use different address formats that we must accommodate
// in our locality object.
#ifdef HPX_PARCELPORT_LIBFABRIC_GNI
# define HPX_PARCELPORT_LIBFABRIC_LOCALITY_SIZE 48
#endif

#if defined(HPX_PARCELPORT_LIBFABRIC_VERBS) || \
    defined(HPX_PARCELPORT_LIBFABRIC_SOCKETS) || \
    defined(HPX_PARCELPORT_LIBFABRIC_PSM2)
# define HPX_PARCELPORT_LIBFABRIC_LOCALITY_SIZE 16
# define HPX_PARCELPORT_LIBFABRIC_LOCALITY_SOCKADDR
#endif

#include <hpx/debugging/print.hpp>
namespace hpx {
    // cppcheck-suppress ConfigurationNotChecked
    static hpx::debug::enable_print<false> loc_deb("LOCALTY");
}   // namespace hpx

namespace hpx {
namespace parcelset {
namespace policies {
namespace libfabric
{

    struct locality;

    struct iplocality {
        iplocality(const hpx::parcelset::policies::libfabric::locality& a);
        const hpx::parcelset::policies::libfabric::locality& data;
        friend std::ostream& operator<<(std::ostream& os, const iplocality& p);
    };

// --------------------------------------------------------------------
// Locality, in this structure we store the informartion required by
// libfabric to make a connection to another node.
// With libfabric 1.4.x the array contains the fabric ip address stored
// as the second uint32_t in the array. For this reason we use an
// array of uint32_t rather than uint8_t/char so we can easily access
// the ip for debug/validation purposes
// --------------------------------------------------------------------
struct locality {

    // the number of 32bit ints stored in our array
    static const uint32_t array_length = HPX_PARCELPORT_LIBFABRIC_LOCALITY_SIZE/4;
    static const uint32_t array_size = HPX_PARCELPORT_LIBFABRIC_LOCALITY_SIZE;

    // array type of our locality data
    typedef std::array<uint32_t, array_length> locality_data;

    static const char *type() {
        return "libfabric";
    }

    explicit locality(const locality_data &in_data)
    {
        std::memcpy(&data_[0], &in_data[0], array_size);
        fi_address_ = 0;
        loc_deb.trace(debug::str<>("explicit"), iplocality((*this)));
    }

    locality() {
        std::memset(&data_[0], 0x00, array_size);
        fi_address_ = 0;
        loc_deb.trace(debug::str<>("default"), iplocality((*this)));
    }

    locality(const locality &other)
        : data_(other.data_)
        , fi_address_(other.fi_address_)
    {
        loc_deb.trace(debug::str<>("copy"), iplocality((*this)));
    }

    locality(const locality &other, fi_addr_t addr)
        : data_(other.data_)
        , fi_address_(addr)
    {
        loc_deb.trace(debug::str<>("copy + fi_addr"), iplocality((*this)));
    }

    locality(locality &&other)
        : data_(std::move(other.data_))
        , fi_address_(other.fi_address_)
    {
        loc_deb.trace(debug::str<>("move"), iplocality((*this)));
    }

    // provided to support sockets mode bootstrap
    explicit locality(const std::string &address,  const std::string &portnum)
    {
        struct sockaddr_in socket_data;
        memset (&socket_data, 0, sizeof (socket_data));
        socket_data.sin_family      = AF_INET;
        socket_data.sin_port        = htons(std::stol(portnum));
        inet_pton(AF_INET, address.c_str(), &(socket_data.sin_addr));
        //
        std::memcpy(&data_[0], &socket_data, array_size);
        fi_address_ = 0;
        loc_deb.trace(debug::str<>("string"), iplocality((*this)));
    }

    // some condition marking this locality as valid
    explicit inline operator bool() const {
        loc_deb.trace(debug::str<>("bool operator")
                , iplocality((*this)));
        return (ip_address() != 0);
    }

    inline bool valid() const {
        loc_deb.trace(debug::str<>("valid operator")
                , iplocality((*this)));
        return (ip_address() != 0);
    }

    locality & operator = (const locality &other) {
        data_       = other.data_;
        fi_address_ = other.fi_address_;
        loc_deb.trace(debug::str<>("copy operator"), iplocality((*this)));
        return *this;
    }

    bool operator == (const locality &other) {
        loc_deb.trace(debug::str<>("equality operator")
                , iplocality((*this))
                , iplocality(other));
        return std::memcmp(&data_, &other.data_, array_size)==0;
    }

    bool less_than(const locality &other) {
        loc_deb.trace(debug::str<>("less_than operator")
                , iplocality((*this))
                , iplocality(other));
        if (ip_address() < other.ip_address()) return true;
        if (ip_address() ==other.ip_address()) return port()<other.port();
        return false;
    }

    const uint32_t & ip_address() const {
#if defined (HPX_PARCELPORT_LIBFABRIC_LOCALITY_SOCKADDR)
        return reinterpret_cast<const struct sockaddr_in*>
            (data_.data())->sin_addr.s_addr;
#elif defined(HPX_PARCELPORT_LIBFABRIC_GNI)
        return data_[0];
#else
        throw fabric_error(0, "unsupported fabric provider, please fix ASAP");
#endif
    }

    static const uint32_t & ip_address(const locality_data &data) {
#if defined (HPX_PARCELPORT_LIBFABRIC_LOCALITY_SOCKADDR)
        return reinterpret_cast<const struct sockaddr_in*>
            (&data)->sin_addr.s_addr;
#elif defined(HPX_PARCELPORT_LIBFABRIC_GNI)
        return data[0];
#else
        throw fabric_error(0, "unsupported fabric provider, please fix ASAP");
#endif
    }

    inline const fi_addr_t& fi_address() const {
        return fi_address_;
    }

    inline void set_fi_address(fi_addr_t fi_addr) {
        fi_address_ = fi_addr;
    }

    inline uint16_t port() const {
        uint16_t port = 256*reinterpret_cast<const uint8_t*>(data_.data())[2]
            + reinterpret_cast<const uint8_t*>(data_.data())[3];
        return port;
    }

    void save(serialization::output_archive & ar) const {
        ar << data_;
        ar << fi_address_;
    }

    // when loading a locality - it will have been transmitted from another node
    // and the fi_address will not be valid, so we must look it up and put
    // the correct value from this node's libfabric address vector.
    // this is only called at bootstrap time, so do not worry about overheads
    void load(serialization::input_archive & ar);

    inline const void *fabric_data() const { return data_.data(); }

    inline char *fabric_data_writable() { return reinterpret_cast<char*>(data_.data()); }

private:
    friend bool operator==(locality const & lhs, locality const & rhs) {
        loc_deb.trace(debug::str<>("equality friend")
            , iplocality(lhs) , iplocality(rhs));
        return (std::memcmp(&lhs.data_, &rhs.data_, array_size)==0)
                && (lhs.fi_address_ == rhs.fi_address_);
    }

    friend bool operator<(locality const & lhs, locality const & rhs) {
        const uint32_t &a1 = lhs.ip_address();
        const uint32_t &a2 = rhs.ip_address();
        const fi_addr_t &f1 = lhs.fi_address();
        const fi_addr_t &f2 = rhs.fi_address();
        loc_deb.trace(debug::str<>("less_than friend")
            , iplocality(lhs) , iplocality(rhs));
        return (a1<a2) || (a1==a2 && f1<f2);
    }

    friend std::ostream & operator<<(std::ostream & os, locality const & loc) {
        hpx::util::ios_flags_saver ifs(os);
        for (uint32_t i=0; i<array_length; ++i) {
            os << loc.data_[i];
        }
        return os;
    }

private:
    locality_data data_;
    fi_addr_t     fi_address_;
};

// ------------------------------------------------------------------
// format as ip address, port, libfabric address
// ------------------------------------------------------------------
inline iplocality::iplocality(const hpx::parcelset::policies::libfabric::locality& a)
  : data(a)
{
}

inline std::ostream& operator<<(std::ostream& os, const iplocality& p)
{
    os << std::dec
       << hpx::debug::ipaddr(&p.data.ip_address())
       << ":" << hpx::debug::dec<>(p.data.port())
       << "(" << hpx::debug::dec<>(p.data.fi_address()) << ")";
    return os;
}

}}}}


