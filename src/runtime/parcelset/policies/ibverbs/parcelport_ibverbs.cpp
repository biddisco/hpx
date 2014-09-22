//  Copyright (c) 2014 Thomas Heller
//  Copyright (c) 2007-2014 Hartmut Kaiser
//  Copyright (c) 2007 Richard D Guidry Jr
//  Copyright (c) 2011 Bryce Lelbach
//  Copyright (c) 2011 Katelyn Kufahl
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/runtime/parcelset/policies/ibverbs/parcelport.hpp>
#include <hpx/runtime/parcelset/encode_parcels.hpp>
#include <hpx/runtime/parcelset/decode_parcels.hpp>

#include <boost/assign/std/vector.hpp>

namespace hpx { namespace parcelset { namespace policies { namespace ibverbs {
    std::vector<std::string> parcelport_impl::runtime_configuration()
    {
        std::vector<std::string> lines;

        using namespace boost::assign;
        lines +=
            "ifname = ${HPX_PARCEL_IBVERBS_IFNAME:" HPX_PARCELPORT_IBVERBS_IFNAME "}",
            "memory_chunk_size = ${HPX_PARCEL_IBVERBS_MEMORY_CHUNK_SIZE:"
                BOOST_PP_STRINGIZE(HPX_PARCELPORT_IBVERBS_MEMORY_CHUNK_SIZE) "}",
            "max_memory_chunks = ${HPX_PARCEL_IBVERBS_MAX_MEMORY_CHUNKS:"
                BOOST_PP_STRINGIZE(HPX_PARCELPORT_IBVERBS_MAX_MEMORY_CHUNKS) "}",
            "zero_copy_optimization = 0",
            "io_pool_size = 1",
            "use_io_pool = 1"
            ;

        return lines;
    }

    parcelport_impl::parcelport_impl(util::runtime_configuration const& ini,
        HPX_STD_FUNCTION<void(std::size_t, char const*)> const& on_start_thread,
        HPX_STD_FUNCTION<void()> const& on_stop_thread)
      : parcelport(ini, connection_handler_name())
      , io_service_pool_(thread_pool_size(ini),
            on_start_thread, on_stop_thread, pool_name(), pool_name_postfix())
      , archive_flags_(boost::archive::no_header)
    {
#ifdef BOOST_BIG_ENDIAN
        std::string endian_out = get_config_entry("hpx.parcel.endian_out", "big");
#else
        std::string endian_out = get_config_entry("hpx.parcel.endian_out", "little");
#endif
        if (endian_out == "little")
            archive_flags_ |= util::endian_little;
        else if (endian_out == "big")
            archive_flags_ |= util::endian_big;
        else {
            HPX_ASSERT(endian_out =="little" || endian_out == "big");
        }

        if (!this->allow_array_optimizations()) {
            archive_flags_ |= util::disable_array_optimization;
            archive_flags_ |= util::disable_data_chunking;
        }
        else {
            if (!this->allow_zero_copy_optimizations())
                archive_flags_ |= util::disable_data_chunking;
        }
    }

    bool parcelport_impl::do_run()
    {
        // Bootstrap the parcelport to accept connections.

        // Notes: Here some functions which might be helpful that you can use:
        //  * Accessing the IP or whatever we want to listen on:
        //     - naming::locality::iterator_type begin =
        //          accept_begin(here_, io_service_pool_.get_io_service(0), true);
        //     - naming::locality::iterator_type end =
        //          accept_end(here_);
        //     *begin will give you boost::asio::ip::tcp::endpoint which you can use
        //     to start listening. To get the IP and Port, use this snippet:
        //        std::string host = ep.address().to_string();
        //        std::string port = boost::lexical_cast<std::string>(ep.port());
        //  * In order to not block, use the io_service_pool_ member you inherited
        //    from parcelport. Example to post a piece of work to the pool:
        //      boost::asio::io_service& io_service = io_service_pool_.get_io_service(0);
        //      io_service.post(util::bind(&connection_handler::handle_accepts, this));
        //
        //    Where handle_accepts is a function that will accept new connections, for example.
        //    The size of the pool is configurable via hpx.parcel.ibverbs.io_pool_size
        //
        //    This function also needs to start the background task to receive incoming messages!
        //
        //    In order to decode incoming messages into a vector of parcels, you can use the following:
        //    template <typename Parcelport, typename Connection, typename Buffer>
        //    void decode_parcels(
        //          Parcelport & parcelport          // *this
        //        , Connection & connection          // The connection the message
        //                                           // was received on (can be
        //                                           // ignored when security was turned off
        //        , boost::shared_ptr<Buffer> buffer // The buffer which holds the message data
        //    )
        //    When this function returns, all parcels which were send with this buffer have been decoded and
        //    turned back into HPX threads.
        //
        //    If you don't like this, just adapt decode_parcels to your needs (hpx/runtime/parcelset/decode_parcels.hpp
        //
        //    return true if everything is started correctly.

        // Parcelport could not be started ... :(
        return false;
    }

    void parcelport_impl::put_parcel(parcel p, parcelport_impl::write_handler_type f)
    {
        naming::locality const& locality_id = p.get_destination_locality();

        // This is the function which is responsible for actually sending the
        // parcel.
        //
        // You can encode a parcel to a std::vector<char> using the encode_parcels function:
        //
        // template <typename Connection>
        // void encode_parcels(
        //      std::vector<parcel> const& parcels // The parcels to encode
        //    , Connection & connection // The destination connection. This class
        //                              // needs to provide the buffer and the
        //                              // destination. A template which can be used
        //                              // for this, can be found in hpx/parcelset/parcelport_connection.hpp
        //    , int archive_flags
        //    , bool enable_security)
        //
        //    If you don't like this, just adapt encode_parcels to your needs (hpx/runtime/parcelset/encode_parcels.hpp
        //
        //    Not that this function needs to be called from a HPX thread
        //
        //    This function should return as soon as possible.
    }
}}}}
