//  Copyright (c) 2017 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/plugins/parcelport/ucx/ucx_context.hpp>

#include <hpx/runtime/parcelset/locality.hpp>
#include <hpx/plugins/parcelport/ucx/locality.hpp>
#include <hpx/plugins/parcelport/ucx/ucx_error.hpp>
#include <hpx/plugins/parcelport/ucx/rdma_logging.hpp>

namespace hpx { namespace parcelset { namespace policies { namespace ucx
{
    bool ucx_context::find_ifaces(std::string const& domain)
    {
        ucs_status_t status;
        uct_md_resource_desc_t *md_resources = nullptr;
        uct_tl_resource_desc_t *tl_resources = nullptr;
        uct_iface_h iface = nullptr;
        unsigned num_md_resources = 0;

        LOG_DEBUG_MSG("Finding interfaces matching " << domain);

        status = uct_query_md_resources(&md_resources, &num_md_resources);
        HPX_PARCELPORT_UCX_THROW_IF(status, UCS_OK);

        for (unsigned i = 0; i != num_md_resources; ++i) {
            LOG_DEBUG_MSG("\tinterface " << decnumber(i) << md_resources[i].md_name);
        }

        try
        {
            // Iterate through the protection domains and find the one
            // matching the name in the config...
            for (unsigned i = 0; i != num_md_resources; ++i)
            {
                if (domain != md_resources[i].md_name)
                    continue;

                LOG_DEBUG_MSG("found interface matching " << md_resources[i].md_name);

                uct_md_config_t *md_config = nullptr;
                pd_ = nullptr;

                status = uct_md_config_read(
                    md_resources[i].md_name, NULL, NULL, &md_config);
                HPX_PARCELPORT_UCX_THROW_IF(status, UCS_OK);

                status = uct_md_open(md_resources[i].md_name, md_config, &pd_);
                uct_config_release(md_config);
                HPX_PARCELPORT_UCX_THROW_IF(status, UCS_OK);

                unsigned num_tl_resources = 0;
                status = uct_md_query_tl_resources(pd_, &tl_resources,
                    &num_tl_resources);
                HPX_PARCELPORT_UCX_THROW_IF(status, UCS_OK);

                // Iterate over the available transports.
                for (unsigned j = 0; j != num_tl_resources; ++j)
                {
                    uct_iface_config_t *iface_config;
                    uct_iface_params_t iface_params;
                    iface_params.tl_name = tl_resources[j].tl_name;
                    iface_params.dev_name = tl_resources[j].dev_name;
                    iface_params.stats_root = nullptr;
                    iface_params.rx_headroom = 0;
                    // @TODO: set proper mask here.
                    UCS_CPU_ZERO(&iface_params.cpu_mask);

                    // Read transport specific interface configuration
                    status = uct_iface_config_read(
                        iface_params.tl_name, NULL, NULL, &iface_config);
                    HPX_PARCELPORT_UCX_THROW_IF(status, UCS_OK);

                    // Open Communication Interface
                    status = uct_iface_open(
                        pd_, worker_, &iface_params, iface_config, &iface);
                    uct_config_release(iface_config);
                    HPX_PARCELPORT_UCX_THROW_IF(status, UCS_OK);

                    // Reading interface attributes...
                    uct_iface_attr_t iface_attr;
                    status = uct_iface_query(iface, &iface_attr);
                    HPX_PARCELPORT_UCX_THROW_IF(status, UCS_OK);

                    LOG_DEBUG_MSG("Interface name " << iface_params.tl_name
                        << "dev " << iface_params.dev_name);

                    // allow for multiple interfaces to be open
                    // only some might support all we need. On Aries,
                    // we need to have two, one for doing AM, one for
                    // RDMA...
                    //
                    // We need:
                    //  - Active message short support to signal new RDMA gets
                    //  - We need to be able to do zero copy gets to
                    //    retrieve our arguments
                    //  - We need to be able to connect to an iface directly
                    //    as point-to-point endpoints would require OOB
                    //    communication

                    bool handles_AM =
                        (iface_attr.cap.flags & UCT_IFACE_FLAG_AM_SHORT) &&
                        (iface_attr.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_IFACE);
                    bool handles_RDMA =
                        (iface_attr.cap.flags & UCT_IFACE_FLAG_GET_ZCOPY);
                    bool handles_BOTH = handles_AM && handles_RDMA;
                    bool print_iface = false;

                    if (handles_BOTH) {
                        LOG_DEBUG_MSG("found RDMA+AM transport: "
                            << iface_params.dev_name << ":"
                            << iface_params.tl_name);
                        print_iface = true;
                        am_iface_ = iface;
                        std::memcpy(&am_iface_attr_, &iface_attr, sizeof(iface_attr));
                        rma_iface_ = iface;
                        std::memcpy(&rma_iface_attr_, &iface_attr, sizeof(iface_attr));
                    }
                    else if (handles_AM && am_iface_ == nullptr) {
                        LOG_DEBUG_MSG("found AM transport: "
                            << iface_params.dev_name << ":"
                            << iface_params.tl_name);
                        print_iface = true;
                        am_iface_ = iface;
                        std::memcpy(&am_iface_attr_, &iface_attr, sizeof(iface_attr));
                    }
                    else if (handles_RDMA && rma_iface_ == nullptr) {
                        LOG_DEBUG_MSG("found RDMA transport: "
                            << iface_params.dev_name << ":"
                            << iface_params.tl_name);
                        print_iface = true;
                        rma_iface_ = iface;
                        std::memcpy(&rma_iface_attr_, &iface_attr, sizeof(iface_attr));
                    }
                    if (print_iface) {
                        LOG_DEBUG_MSG("Interface am  "
                            << "max_short "     << hexnumber(iface_attr.cap.am.max_short)
                            << "max_bcopy "     << hexnumber(iface_attr.cap.am.max_bcopy)
                            << "min/max_zcopy " << hexuint64(iface_attr.cap.am.min_zcopy)
                            << ", "             << hexuint64(iface_attr.cap.am.max_zcopy)
                            << "hdr "           << hexnumber(iface_attr.cap.am.max_hdr)
                            << "iov "           << decnumber(iface_attr.cap.am.max_iov));

                        LOG_DEBUG_MSG("Interface put "
                            << "max_short "     << hexnumber(iface_attr.cap.put.max_short)
                            << "max_bcopy "     << hexnumber(iface_attr.cap.put.max_bcopy)
                            << "min/max_zcopy " << hexuint64(iface_attr.cap.put.min_zcopy)
                            << ", "             << hexuint64(iface_attr.cap.put.max_zcopy)
                            << "iov "           << decnumber(iface_attr.cap.put.max_iov));

                        LOG_DEBUG_MSG("Interface get "
                            << "max_bcopy "     << hexnumber(iface_attr.cap.get.max_bcopy)
                            << "min/max_zcopy " << hexuint64(iface_attr.cap.get.min_zcopy)
                            << ", "             << hexuint64(iface_attr.cap.get.max_zcopy)
                            << "iov "           << decnumber(iface_attr.cap.get.max_iov));
                    }

                    if (rma_iface_ && am_iface_) break;

                    if (!rma_iface_ && !am_iface_)
                        uct_iface_close(iface);
                    iface = nullptr;
                }

                uct_release_tl_resource_list(tl_resources);
                tl_resources = nullptr;

                if (rma_iface_ && am_iface_) break;
            }
        }
        catch(...)
        {
            LOG_ERROR_MSG("exception in interface check" );

            if (tl_resources != nullptr)
            {
                uct_release_tl_resource_list(tl_resources);
            }

            if(iface != nullptr)
            {
                uct_iface_close(iface);
            }

            if (pd_ != nullptr)
            {
                uct_md_close(pd_);
                pd_ = nullptr;
            }

            uct_release_md_resource_list(md_resources);

            throw;
        }

        uct_release_md_resource_list(md_resources);

        return rma_iface_ && am_iface_;
    }

    ucx_context::ucx_context(std::string const& domain,
        hpx::parcelset::locality& here, bool enabled)
      : pd_(nullptr),
        rma_iface_(nullptr),
        am_iface_(nullptr),
        am_ep_(nullptr),
        worker_(nullptr)
    {
        ucs_status_t status;
        LOG_DEBUG_MSG("calling ucs_async_context_init");
        // Initialize our UCX context
        status = ucs_async_context_init(&context_, UCS_ASYNC_MODE_THREAD);
        HPX_PARCELPORT_UCX_THROW_IF(status, UCS_OK);

        // Initialize our UCX worker
        LOG_DEBUG_MSG("calling uct_worker_create");
        status = uct_worker_create(&context_, UCS_THREAD_MODE_MULTI, &worker_);
        HPX_PARCELPORT_UCX_THROW_IF(status, UCS_OK);

        // We need to find suitable network interfaces
        LOG_DEBUG_MSG("calling find_ifaces");
        if (find_ifaces(domain))
        {
            HPX_ASSERT(pd_ != nullptr);
            HPX_ASSERT(rma_iface_ != nullptr);
            HPX_ASSERT(am_iface_ != nullptr);

            // get the PD related attributes, needed for memory
            // registration
            status = uct_md_query(pd_, &pd_attr_);
            HPX_PARCELPORT_UCX_THROW_IF(status, UCS_OK);

            locality &l = here.get<locality>();

            // now get the addresses of the interfaces and set them to
            // the locality struct in order to be exchanged with other
            // localities through the bootstrap parcelport...

            l.rma_addr().set_iface_attr(rma_iface_attr_);
            uct_device_addr_t *rma_device_addr = l.rma_addr().device_addr();

            status = uct_iface_get_device_address(rma_iface_, rma_device_addr);
            HPX_PARCELPORT_UCX_THROW_IF(status, UCS_OK);
            if (rma_iface_attr_.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_IFACE)
            {
                HPX_ASSERT(l.rma_addr().iface_length_ != 0);
                uct_iface_addr_t *rma_iface_addr = l.rma_addr().iface_addr();
                status = uct_iface_get_address(rma_iface_, rma_iface_addr);
                HPX_PARCELPORT_UCX_THROW_IF(status, UCS_OK);
            }

            l.am_addr().set_iface_attr(am_iface_attr_);
            uct_iface_addr_t *am_iface_addr = l.am_addr().iface_addr();
            uct_device_addr_t *am_device_addr = l.am_addr().device_addr();

            status = uct_iface_get_device_address(am_iface_, am_device_addr);
            HPX_PARCELPORT_UCX_THROW_IF(status, UCS_OK);
//             HPX_ASSERT((am_iface_attr_.cap.flags & UCT_IFACE_FLAG_CONNECT_TO_IFACE) != 0)
            HPX_ASSERT(l.am_addr().iface_length_ != 0);
            status = uct_iface_get_address(am_iface_, am_iface_addr);
            HPX_PARCELPORT_UCX_THROW_IF(status, UCS_OK);
        }
        else if (enabled)
        {
            throw std::runtime_error(
                "No suitable UCX interface could have been found...");
        }
    }

    ucx_context::~ucx_context()
    {
        if (rma_iface_ != nullptr)
        {
            if (rma_iface_ == am_iface_)
            {
                am_iface_ = nullptr;
            }
            uct_iface_close(rma_iface_);
        }
        std::memset(&rma_iface_attr_, 0, sizeof(rma_iface_attr_));
        if (am_iface_ != nullptr)
        {
            uct_iface_close(am_iface_);
        }
        std::memset(&am_iface_attr_, 0, sizeof(am_iface_attr_));

        if (pd_ != nullptr)
        {
            uct_md_close(pd_);
        }
        if (worker_ != nullptr)
            uct_worker_destroy(worker_);

        ucs_async_context_cleanup(&context_);
    }

    void ucx_context::progress()
    {
        uct_worker_progress(worker_);
    }
}}}}
