//  Copyright (c) 2016 John Biddiscombe
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_POLICIES_LIBFABRIC_CONTROLLER_HPP
#define HPX_PARCELSET_POLICIES_LIBFABRIC_CONTROLLER_HPP

// config
#include <hpx/config/defines.hpp>
//
#include <hpx/lcos/local/shared_mutex.hpp>
#include <hpx/lcos/promise.hpp>
#include <hpx/lcos/future.hpp>
//
#include <plugins/parcelport/parcelport_logging.hpp>
#include <plugins/parcelport/libfabric/rdma_locks.hpp>
#include <plugins/parcelport/libfabric/locality.hpp>
#include <plugins/parcelport/libfabric/libfabric_endpoint.hpp>
//
#include <plugins/parcelport/unordered_map.hpp>
//
#include <memory>
#include <deque>
#include <chrono>
#include <iostream>
#include <functional>
#include <map>
#include <atomic>
#include <string>
#include <utility>
#include <cstdint>
//
#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>
#include "fabric_error.hpp"

// @TODO : Remove the client map pair as we have a copy in the libfabric_parcelport class
// that does almost the same job.
// @TODO : Most of this code could be moved into the main parcelport, or the endpoint
// classes
namespace hpx {
namespace parcelset {
namespace policies {
namespace libfabric
{

    class libfabric_controller
    {
    public:
        typedef hpx::lcos::local::spinlock mutex_type;
        typedef hpx::parcelset::policies::libfabric::unique_lock<mutex_type> unique_lock;
        typedef hpx::parcelset::policies::libfabric::scoped_lock<mutex_type> scoped_lock;

    	locality here_;

        struct fi_info    *fabric_hints_;
        struct fi_info    *fabric_info_;
        struct fi_info    *fabric_info_active_;
        struct fid_fabric *fabric_;
        struct fid_domain *fabric_domain_;
        struct fid_pep    *ep_passive_;
        struct fid_ep     *ep_active_;

        // we will use just one event queue for all connections
        struct fid_eq     *event_queue_;
        struct fid_cq *txcq, *rxcq;
        struct fid_av *av;

        struct fi_context tx_ctx, rx_ctx;
        uint64_t remote_cq_data;

        uint64_t tx_seq, rx_seq, tx_cq_cntr, rx_cq_cntr;

        struct fi_av_attr av_attr;
        struct fi_eq_attr eq_attr;
        struct fi_cq_attr cq_attr;


        void display_fabric_info()
        {
            LOG_BOOST_MSG(
                "Running pingpong test with the %s endpoint through a %s provider", %
                fi_tostr(&fabric_info_->ep_attr->type , FI_TYPE_EP_TYPE) %
                fabric_info_->fabric_attr->prov_name);
            LOG_BOOST_MSG(" * Fabric Attributes:",);
            LOG_BOOST_MSG("  - %-20s: %s", % "name" % fabric_info_->fabric_attr->name);
            LOG_BOOST_MSG("  - %-20s: %s", % "prov_name" %
                 fabric_info_->fabric_attr->prov_name);
            LOG_BOOST_MSG("  - %-20s: %u", % "prov_version" %
                 fabric_info_->fabric_attr->prov_version);
            LOG_BOOST_MSG(" * Domain Attributes:",);
            LOG_BOOST_MSG("  - %-20s: %s", % "name" % fabric_info_->domain_attr->name);
            LOG_BOOST_MSG("  - %-20s: %u", % "cq_cnt" % fabric_info_->domain_attr->cq_cnt);
            LOG_BOOST_MSG("  - %-20s: %u", % "cq_data_size" %
                 fabric_info_->domain_attr->cq_data_size);
            LOG_BOOST_MSG("  - %-20s: %u", % "ep_cnt" % fabric_info_->domain_attr->ep_cnt);
            LOG_BOOST_MSG(" * Endpoint Attributes:",);
            LOG_BOOST_MSG("  - %-20s: %s", % "type" %
                 fi_tostr(&fabric_info_->ep_attr->type , FI_TYPE_EP_TYPE));
            LOG_BOOST_MSG("  - %-20s: %u", % "protocol" %
                 fabric_info_->ep_attr->protocol);
            LOG_BOOST_MSG("  - %-20s: %u", % "protocol_version" %
                 fabric_info_->ep_attr->protocol_version);
            LOG_BOOST_MSG("  - %-20s: %u", % "max_msg_size" %
                 fabric_info_->ep_attr->max_msg_size);
            LOG_BOOST_MSG("  - %-20s: %u", % "max_order_raw_size" %
                 fabric_info_->ep_attr->max_order_raw_size);
        }

        // --------------------------------------------------------------------
        // constructor gets info from device and sets up all necessary
        // maps, queues and server endpoint etc
        libfabric_controller(
        	std::string provider,
			std::string domain,
        	std::string endpoint, int port)
        {
            FUNC_START_DEBUG_MSG;

            fabric_hints_ = nullptr;
            fabric_info_  = nullptr;
            fabric_       = nullptr;
            ep_passive_   = nullptr;
            event_queue_        = nullptr;
            fabric_domain_      = nullptr;
			//
            fabric_info_active_ = nullptr;
            ep_active_          = nullptr;
            //
            txcq = nullptr;
            rxcq = nullptr;
            av   = nullptr;
            tx_ctx = {};
            rx_ctx = {};
            remote_cq_data = 0;
            tx_seq = 0;
			rx_seq = 0;
			tx_cq_cntr = 0;
			rx_cq_cntr = 0;

            av_attr = {};
            eq_attr = {};
            cq_attr = {};
			//
            here_ = open_fabric(provider, domain, endpoint);

            FUNC_END_DEBUG_MSG;
        }

        // clean up all resources
        ~libfabric_controller()
        {
            fi_close(&fabric_->fid);
            fi_close(&ep_passive_->fid);
            fi_close(&event_queue_->fid);
            fi_close(&fabric_domain_->fid);
			// clean up
            fi_freeinfo(fabric_info_);
            fi_freeinfo(fabric_hints_);
		}

        // --------------------------------------------------------------------
        locality open_fabric(
        	std::string provider, std::string domain, std::string endpoint_type)
        {
        	fabric_info_  = nullptr;
            fabric_hints_ = fi_allocinfo();
            if (!fabric_hints_) {
                throw fabric_error(-1, "Failed to allocate fabric hints");
            }
            // we require message and RMA support, so ask for them
            fabric_hints_->caps 				  = FI_MSG | FI_RMA;
            fabric_hints_->mode                   = ~0; // FI_CONTEXT | FI_LOCAL_MR;
            fabric_hints_->fabric_attr->prov_name = strdup(provider.c_str());
            fabric_hints_->domain_attr->name      = strdup(domain.c_str());

            // use infiniband type basic registration for now
            fabric_hints_->domain_attr->mr_mode   = FI_MR_BASIC;

            if (endpoint_type=="msg") {
            	fabric_hints_->ep_attr->type = FI_EP_MSG;
            } else if (endpoint_type=="rdm") {
            	fabric_hints_->ep_attr->type = FI_EP_RDM;
            } else if (endpoint_type=="dgram") {
            	fabric_hints_->ep_attr->type = FI_EP_DGRAM;
            }
            else {
            	LOG_DEBUG_MSG("endpoint type not set, using DGRAM");
            	fabric_hints_->ep_attr->type = FI_EP_DGRAM;
            }
            // @TODO remove this test var
            //fabric_hints_->ep_attr->max_msg_size = 0x4000;

			// no idea why this is here.
            //eq_attr.wait_obj = FI_WAIT_UNSPEC;

            LOG_DEBUG_MSG("fabric provider " << fabric_hints_->fabric_attr->prov_name);
            LOG_DEBUG_MSG("fabric domain "   << fabric_hints_->domain_attr->name);

            uint64_t flags = 0;
            LOG_DEBUG_MSG("Getting info about fabric using passive endpoint");
            int ret = fi_getinfo(FI_VERSION(1,4), NULL, NULL,
            	flags, fabric_hints_, &fabric_info_);
            if (ret) {
                throw fabric_error(ret, "Failed to get fabric info");
            }

            display_fabric_info();

            LOG_DEBUG_MSG("Creating fabric object");
            ret = fi_fabric(fabric_info_->fabric_attr, &fabric_, NULL);
            if (ret) {
                throw fabric_error(ret, "Failed to get fi_fabric");
            }

            LOG_DEBUG_MSG("Creating passive endpoint");
            ret = fi_passive_ep(fabric_, fabric_info_, &ep_passive_, NULL);
            if (ret) {
                throw fabric_error(ret, "Failed to get fi_passive_ep");
            }

            LOG_DEBUG_MSG("Fetching local address");
            locality::locality_data local_addr;
            std::size_t addrlen = sizeof(local_addr);
            ret = fi_getname(&ep_passive_->fid, local_addr.data(), &addrlen);
            if (ret || (addrlen>sizeof(local_addr))) {
                fabric_error(ret, "fi_getname - size error or other problem");
            }

            LOG_DEBUG_MSG("Name length " << decnumber(addrlen));
            LOG_DEBUG_MSG("address info is "
                    << ipaddress(local_addr[0]) << " "
                    << ipaddress(local_addr[1]) << " "
                    << ipaddress(local_addr[2]) << " "
                    << ipaddress(local_addr[3]) << " ");
            return locality(local_addr);
        }

        // --------------------------------------------------------------------
		const locality & here() { return here_; }

        // --------------------------------------------------------------------
        // initiate a listener for connections
        int startup()
        {
        	int ret;
            LOG_DEBUG_MSG("Creating event queue");
            ret = fi_eq_open(fabric_, &eq_attr, &event_queue_, NULL);
            if (ret) throw fabric_error(ret, "fi_eq_open");

            LOG_DEBUG_MSG("Binding event queue to passive endpoint");
            ret = fi_pep_bind(ep_passive_, &event_queue_->fid, 0);
            if (ret) throw fabric_error(ret, "fi_pep_bind");

            LOG_DEBUG_MSG("Ppassive endpoint : listen");
            ret = fi_listen(ep_passive_);
            if (ret) throw fabric_error(ret, "fi_listen");

            // Allocate a domain.
            LOG_DEBUG_MSG("Allocating domain ");
            ret = fi_domain(fabric_, fabric_info_, &fabric_domain_, NULL);
            if (ret) throw fabric_error(ret, "fi_domain");

            // Create a memory pool for pinned buffers
            memory_pool_ = std::make_shared<rdma_memory_pool> (fabric_domain_);
        	return 0;
        }

        // --------------------------------------------------------------------
        // returns true when all connections have been disconnected and none are active
        bool isTerminated() {
            return (qp_endpoint_map_.size() == 0);
        }

        // types we need for connection and disconnection callback functions
        // into the main parcelport code.
        typedef std::function<void(libfabric_endpoint_ptr)>       ConnectionFunction;
        typedef std::function<int(libfabric_endpoint_ptr client)> DisconnectionFunction;

        // --------------------------------------------------------------------
        // Set a callback which will be called immediately after
        // RDMA_CM_EVENT_ESTABLISHED has been received.
        // This should be used to initialize all structures for handling a new connection
        void setConnectionFunction(ConnectionFunction f) {
            this->connection_function_ = f;
        }

        // --------------------------------------------------------------------
        // currently not used.
        void setDisconnectionFunction(DisconnectionFunction f) {
            this->disconnection_function_ = f;
        }

        // --------------------------------------------------------------------
        // This is the main polling function that checks for work completions
        // and connection manager events, if stopped is true, then completions
        // are thrown away, otherwise the completion callback is triggered
        int poll_endpoints(bool stopped=false) {

        	uint32_t event;
        	struct fi_eq_cm_entry entry;
        	ssize_t rd = fi_eq_read(event_queue_, &event, &entry, sizeof(entry), 0);
       		if (rd > 0) {
       			LOG_DEBUG_MSG("got event " << event);
       			switch (event) {
       				case FI_CONNREQ:
       					LOG_DEBUG_MSG("Got FI_CONNREQ");
       					break;
       				case FI_CONNECTED:
       					LOG_DEBUG_MSG("Got FI_CONNECTED");
       					break;
       				case FI_NOTIFY:
       					LOG_DEBUG_MSG("Got FI_NOTIFY");
       					break;
       				case FI_SHUTDOWN:
       					LOG_DEBUG_MSG("Got FI_SHUTDOWN");
       					break;
       				case FI_MR_COMPLETE:
       					LOG_DEBUG_MSG("Got FI_MR_COMPLETE");
       					break;
       				case FI_AV_COMPLETE:
       					LOG_DEBUG_MSG("Got FI_AV_COMPLETE");
       					break;
       				case FI_JOIN_COMPLETE:
       					LOG_DEBUG_MSG("Got FI_JOIN_COMPLETE");
       					break;
       			}
       			HPX_ASSERT(rd == sizeof(entry));
       			HPX_ASSERT(entry.fid == &event_queue_->fid);
        	}
        	return 0;
        }

        int poll_for_work_completions(bool stopped=false) { return 0; }
/*
        inline libfabric_completion_queue *get_completion_queue() const {
            return completion_queue_.get();
        }

        inline libfabric_endpoint *get_client_from_completion(struct fi_cq_msg_entry &completion)
        {
            return qp_endpoint_map_.at(completion.qp_num).get();
        }
*/
        inline struct fid_domain * get_domain() {
            return this->fabric_domain_;
        }

        libfabric_endpoint* get_endpoint(uint32_t remote_ip) {
            return (std::get<0>(connections_started_.find(remote_ip)->second)).get();

        }

        inline rdma_memory_pool_ptr get_memory_pool() {
            return memory_pool_;
        }

        typedef std::function<int(struct fi_cq_msg_entry completion, libfabric_endpoint *client)>
            CompletionFunction;

        void setCompletionFunction(CompletionFunction f) {
            this->completion_function_ = f;
        }

        void refill_client_receives(bool force=true) {}

        // --------------------------------------------------------------------
        hpx::shared_future<libfabric_endpoint_ptr> connect_to_server(const locality &remote)
        {
        	int ret;
        	LOG_DEBUG_MSG("Setting info object with destination address size " << locality::array_size);

        	// now get it back and print it
        	char addr_str[INET_ADDRSTRLEN];
        	inet_ntop(AF_INET, &remote.ip_address(), addr_str, INET_ADDRSTRLEN);
        	std::string port_str = std::to_string(remote.port());
        	LOG_DEBUG_MSG("IP address " << addr_str << ":" << port_str);

            uint64_t flags = 0;
            ret = fi_getinfo(FI_VERSION(1,4), addr_str, port_str.c_str(),
            	flags, fabric_hints_, &fabric_info_active_);
            struct fi_info *the_info = fabric_info_active_;

            if (cq_attr.format == FI_CQ_FORMAT_UNSPEC)
                cq_attr.format = FI_CQ_FORMAT_CONTEXT;

            // open a completion queue on our fabric domain and set context ptr to tx queue
            cq_attr.wait_obj = FI_WAIT_NONE;
            cq_attr.size = the_info->tx_attr->size;
            LOG_DEBUG_MSG("Creating CQ with tx size " << fabric_info_->tx_attr->size);
            ret = fi_cq_open(fabric_domain_, &(cq_attr), &(txcq), &(txcq));
			if (ret) throw fabric_error(ret, "fi_cq_open");

            // open a completion queue on our fabric domain and set context ptr to rx queue
            cq_attr.size = the_info->rx_attr->size;
            LOG_DEBUG_MSG("Creating CQ with rx size " << fabric_info_->rx_attr->size);
            ret = fi_cq_open(fabric_domain_, &(cq_attr), &(rxcq), &(rxcq));
			if (ret) throw fabric_error(ret, "fi_cq_open");

            if (the_info->ep_attr->type == FI_EP_MSG) {
            	LOG_DEBUG_MSG("Endpoint type is MSG");
            }
            if (the_info->ep_attr->type == FI_EP_RDM || the_info->ep_attr->type == FI_EP_DGRAM) {
                if (the_info->domain_attr->av_type != FI_AV_UNSPEC)
                    av_attr.type = the_info->domain_attr->av_type;

                LOG_DEBUG_MSG("Creating address vector");
                ret = fi_av_open(fabric_domain_, &(av_attr), &(av), NULL);
    			if (ret) throw fabric_error(ret, "fi_av_open");
            }

            // create an 'active' endpoint that can be used for sending/receiving
            LOG_DEBUG_MSG("Creating active endpoint");
            ret = fi_endpoint(fabric_domain_, the_info, &(ep_active_), NULL);
			if (ret) throw fabric_error(ret, "fi_endpoint");



        	if (the_info->ep_attr->type == FI_EP_MSG) {
        		if (event_queue_) {
        			LOG_DEBUG_MSG("Binding endpoint to EQ");
        			ret = fi_ep_bind(ep_active_, &(event_queue_->fid), 0);
        			if (ret) throw fabric_error(ret, "bind event_queue_");
        		}
        	}

        	if (av) {
        		LOG_DEBUG_MSG("Binding endpoint to AV");
        		ret = fi_ep_bind(ep_active_, &(av->fid), 0);
        		if (ret) throw fabric_error(ret, "bind event_queue_");
        	}

        	if (txcq) {
        		LOG_DEBUG_MSG("Binding endpoint to TX CQ");
        		ret = fi_ep_bind(ep_active_, &(txcq->fid), FI_TRANSMIT);
        		if (ret) throw fabric_error(ret, "bind txcq");
        	}

        	if (rxcq) {
        		LOG_DEBUG_MSG("Binding endpoint to RX CQ");
        		ret = fi_ep_bind(ep_active_, &(rxcq->fid), FI_RECV);
        		if (ret) throw fabric_error(ret, "rxcq");
        	}

        	LOG_DEBUG_MSG("Enabling active endpoint");
        	ret = fi_enable(ep_active_);
        	if (ret)
        		throw hpx::parcelset::policies::libfabric::fabric_error(ret, "fi_enable");

        	auto region = memory_pool_->allocate_region(0);
        	void *desc = region->get_desc();
        	LOG_DEBUG_MSG("Posting recv memory descriptor " << hexpointer(desc));

        	ret = fi_recv(ep_active_, region->get_address(), region->get_size(), desc, 0, NULL);
            if (ret!=0) {
            	if (ret == -FI_EAGAIN) {
                	LOG_ERROR_MSG("We must repost");
            	} else {
	        		throw hpx::parcelset::policies::libfabric::fabric_error(ret, "pp_post_rx");
            	}
            }

        	const uint8_t *d = static_cast<const uint8_t*>(remote.fabric_data());
        	for (int i=0; i<16; ++i) {
        		std::cout << std::dec << int(d[i]) << ":";
        	}
        	std::cout << std::endl;

        	ret = fi_connect(ep_active_, d/*remote.fabric_data()*/, NULL, 0);
            if (ret) throw fabric_error(ret, "fi_connect");

        	struct fi_eq_cm_entry entry;
        	uint32_t event;
        	ssize_t rd;
        	/* Connect */
        	rd = fi_eq_sread(event_queue_, &event, &entry, sizeof(entry), -1, 0);
        	if (rd != sizeof(entry)) {
        		//        	        pp_process_eq_err(rd, eq, "fi_eq_sread");
        		ret = (int)rd;
        		throw fabric_error(ret, "fi_eq_sread");
        	}

        	if (event != FI_CONNECTED || entry.fid != &(ep_active_->fid)) {
        		fprintf(stderr, "Unexpected CM event %d fid %p (ep %p)\n",
        				event, entry.fid, ep_active_);
        		ret = -FI_EOTHER;
        		throw fabric_error(ret, "Unexpected event");
        	}


        	return hpx::make_ready_future(
        			std::make_shared<libfabric_endpoint>()
        	);
        }

        void disconnect_from_server(libfabric_endpoint_ptr client) {}

        void disconnect_all() {}

        bool active() { return false; }

    private:
        void debug_connections() {}

        int handle_event(struct rdma_cm_event *cm_event, libfabric_endpoint *client) { return 0; }

        int handle_connect_request(
            struct rdma_cm_event *cm_event, std::uint32_t remote_ip) { return 0; }

        int start_server_connection(uint32_t remote_ip) { return 0; }

        // store info about local device
        std::string           device_;
        std::string           interface_;
        sockaddr_in           local_addr_;

        // callback functions used for connection event handling
        ConnectionFunction    connection_function_;
        DisconnectionFunction disconnection_function_;

        // callback function for handling a completion event
        CompletionFunction    completion_function_;

        // Pinned memory pool used for allocating buffers
        rdma_memory_pool_ptr        memory_pool_;
        // Server/Listener for RDMA connections.
        libfabric_endpoint_ptr          server_endpoint_;
        // Shared completion queue for all endoints
//        libfabric_completion_queue_ptr  completion_queue_;
        // Count outstanding receives posted to SRQ + Completion queue
        std::atomic<uint16_t>       preposted_receives_;

        // only allow one thread to handle connect/disconnect events etc
        mutex_type            controller_mutex_;

        typedef std::tuple<
            libfabric_endpoint_ptr,
            hpx::promise<libfabric_endpoint_ptr>,
            hpx::shared_future<libfabric_endpoint_ptr>
        > promise_tuple_type;
        //
        typedef std::pair<const uint32_t, promise_tuple_type> ClientMapPair;
        typedef std::pair<const uint32_t, libfabric_endpoint_ptr> QPMapPair;

        // Map of all active clients indexed by queue pair number.
//        hpx::concurrent::unordered_map<uint32_t, promise_tuple_type> clients_;

        typedef hpx::concurrent::unordered_map<uint32_t, promise_tuple_type>
            ::map_read_lock_type map_read_lock_type;
        typedef hpx::concurrent::unordered_map<uint32_t, promise_tuple_type>
            ::map_write_lock_type map_write_lock_type;

        // Map of connections started, needed during address resolution until
        // qp is created, and then to hold a future to an endpoint that the parcelport
        // can get and wait on
        hpx::concurrent::unordered_map<uint32_t, promise_tuple_type> connections_started_;

        typedef std::pair<uint32_t, libfabric_endpoint_ptr> qp_map_type;
        hpx::concurrent::unordered_map<uint32_t, libfabric_endpoint_ptr> qp_endpoint_map_;

        // used to skip polling event channel too frequently
        typedef std::chrono::time_point<std::chrono::system_clock> time_type;
        time_type event_check_time_;
        uint32_t  event_pause_;

    };

    // Smart pointer for libfabric_controller obje
    typedef std::shared_ptr<libfabric_controller> libfabric_controller_ptr;

}}}}

#endif
