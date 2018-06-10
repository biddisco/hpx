//  Copyright (c) 2018 Thomas Heller
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/runtime/parcelset/v2/fabric_status.hpp>
#include <hpx/runtime/parcelset/v2/fabric_domain.hpp>
#include <hpx/runtime/parcelset/v2/fabric_endpoint.hpp>
#include <hpx/runtime/parcelset/v2/parcelhandler.hpp>

#include <hpx/util/assert.hpp>

#include <rdma/fabric.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>
#include <string>

namespace hpx { namespace parcelset { inline namespace v2 {
}}}

namespace hpx { namespace parcelset { inline namespace v2 {

    enum class message_type : std::uint8_t
    {
        register_addr = 0,
        resolved_addr = 1,
        action = 2,
        quit = 3
    };

    struct message
    {
        static constexpr std::size_t size = 4096;
        struct header_type {
            message_type type;
            std::uint64_t context;
        };

        header_type header;
        std::array<char, size - sizeof(header)> payload;
    };

    fi_info* get_fabricinfo(hpx::parcelset::fabric_provider provider, const char* host, const char* port)
    {
        hpx::parcelset::fabric_status status;
        fi_info* hints = fi_allocinfo();

        hints->caps = FI_MSG | FI_RMA | FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE;
        hints->mode = FI_CONTEXT | FI_MSG_PREFIX | FI_ASYNC_IOV;
        hints->ep_attr->type = FI_EP_RDM;
        hints->domain_attr->mr_mode = FI_MR_BASIC;//~(FI_MR_BASIC | FI_MR_SCALABLE);
        // Disable the use of progress threads
        if (provider == hpx::parcelset::fabric_provider::sockets)
        {
            hints->domain_attr->control_progress = FI_PROGRESS_AUTO;
            hints->domain_attr->data_progress = FI_PROGRESS_AUTO;
        }
        else
        {
            hints->domain_attr->control_progress = FI_PROGRESS_MANUAL;
            hints->domain_attr->data_progress = FI_PROGRESS_MANUAL;
        }

        // Enable thread safe mode Does not work with psm2 provider
        hints->domain_attr->threading = FI_THREAD_SAFE;

        // Enable resource management
        hints->domain_attr->resource_mgmt = FI_RM_ENABLED;

        if (provider != fabric_provider::invalid)
        {
            hints->fabric_attr->prov_name = strdup(hpx::parcelset::fabric_provider_name(provider));
        }
        // by default, we will always want completions on both tx/rx events
        hints->tx_attr->op_flags = FI_COMPLETION;
        hints->rx_attr->op_flags = FI_COMPLETION;

        fi_info* info;
        status = fi_getinfo(FI_VERSION(1,6), host, port, 0, hints, &info);
        if (!status)
        {
            info = nullptr;
        }

        fi_freeinfo(hints);
        return info;
    }
}}}

int main(int argc, char** argv)
{
    if (argc != 4)
    {
        std::cout << "Usage: " << argv[0] << " <port> <remote_host> <remote_port>\n";
        return 1;
    }

    hpx::parcelset::v2::parcelhandler ph;

    std::string port_s = argv[1];
    std::string remote_host = argv[2];
    std::string remote_port_s = argv[3];
    std::uint16_t port = std::stoi(port_s);
//     std::uint16_t remote_port = std::stoi(remote_port_s);
    std::uint64_t locality_id = port == 7190 ? 0 : 1;
    std::uint64_t remote_locality_id = port == 7190 ? 1 : 0;

    hpx::parcelset::fabric_status status;

    std::vector<hpx::parcelset::fabric_provider> providers{
        hpx::parcelset::fabric_provider::verbs,
        hpx::parcelset::fabric_provider::sockets
    };

    std::array<
        std::vector<hpx::parcelset::fabric_domain>,
        std::size_t(hpx::parcelset::fabric_provider::num)
    > domains;

    for (auto const& provider: providers)
    {
        fi_info* info = hpx::parcelset::get_fabricinfo(provider, nullptr, nullptr);
        if (!info) continue;

        std::cout << "Checking for " << provider << '\n';
        auto& domain = domains[static_cast<std::size_t>(provider)];
        for (fi_info *iter = info; iter != nullptr; iter = iter->next)
        {
            if (hpx::parcelset::get_fabric_provider(iter) != provider)
            {
                continue;
            }

            hpx::parcelset::fabric_domain dom(iter, port);
            auto it = std::find_if(domain.begin(), domain.end(),
                [&dom](hpx::parcelset::fabric_domain const& other)
                {
                    if (dom.info_->src_addrlen != other.info_->src_addrlen)
                    {
                        return false;
                    }
                    return std::memcmp(dom.info_->src_addr, other.info_->src_addr, dom.info_->src_addrlen) == 0;
                });
            if (it == domain.end())
            {
                domain.emplace_back(std::move(dom));
            }
        }
        std::cout << "Found " << domain.size() << '\n';
        fi_freeinfo(info);
    }

    std::vector<hpx::parcelset::fabric_endpoint> endpoints;

    // Initalize endpoints...
    for (auto const& provider: providers)
    {
        for (auto const& domain: domains[static_cast<std::size_t>(provider)])
        {
            endpoints.emplace_back(&domain);
        }
    }

    std::cout << "Total endpoints: " << endpoints.size() << '\n';

    // Prepare receives
    std::vector<hpx::parcelset::message> recv_messages(endpoints.size());
    std::vector<hpx::parcelset::message> send_messages(endpoints.size());
    fid_mr no_mr;
    std::vector<fid_mr *> mr_recv_messages(endpoints.size());
    std::vector<fid_mr *> mr_send_messages(endpoints.size());
    std::vector<fi_context> recv_contexts(endpoints.size());
    std::vector<fi_context> send_contexts(endpoints.size());

    for (std::size_t i = 0; i != endpoints.size(); ++i)
    {
        auto& ep = endpoints[i];
        auto& recv_message = recv_messages[i];
        auto& mr_recv = mr_recv_messages[i];
        auto& send_message = send_messages[i];
        auto& mr_send = mr_send_messages[i];
        auto& context = recv_contexts[i];

        // Register receive buffer
        if (ep.domain_->info_->domain_attr->mr_mode & FI_MR_BASIC)
        {
            status = fi_mr_reg(ep.domain_->domain_, &recv_message, sizeof(recv_message),
                FI_RECV, 0, 0, 0, &mr_recv, nullptr);
            if (!status)
            {
                std::cout << status.what() << '\n';
                throw status;
            }
            status = fi_mr_reg(ep.domain_->domain_, &send_message, sizeof(send_message),
                FI_SEND, 0, 0, 0, &mr_send, nullptr);
            if (!status)
            {
                std::cout << status.what() << '\n';
                throw status;
            }
        }
        else
        {
            mr_recv = &no_mr;
            mr_send = &no_mr;
        }

        // Post receive buffer
        while (true)
        {
            status = fi_recv(ep.endpoint_, &recv_message, sizeof(recv_message), fi_mr_desc(mr_recv),
                0, &context);
            if (status) break;
            if (status == hpx::parcelset::fabric_status::eagain()) continue;
            if (!status)
            {
                std::cout << status.what() << '\n';
                throw status;
            }
        }
    }

    // Getting Info about our remote host...
    fi_info* info = hpx::parcelset::get_fabricinfo(hpx::parcelset::fabric_provider::invalid, remote_host.c_str(), remote_port_s.c_str());
    if (info)
    {
        auto check_addr_format = [](fi_info* fi)
        {
            return fi->addr_format == FI_SOCKADDR_IN ||
                fi->addr_format == FI_SOCKADDR_IN6;
        };
        for (fi_info *iter = info; iter != nullptr; iter = iter->next)
        {
            if (!check_addr_format(iter)) continue;

            hpx::parcelset::fabric_provider remote_provider = hpx::parcelset::get_fabric_provider(iter);
            std::string remote_fabric_name(iter->fabric_attr->name);
            auto ep_iter = std::find_if(endpoints.begin(), endpoints.end(),
                [&check_addr_format, &remote_provider, &remote_fabric_name, iter](hpx::parcelset::fabric_endpoint const& ep)
                {
                    return check_addr_format(ep.domain_->info_) &&
                        ep.domain_->info_->src_addrlen == iter->src_addrlen &&
                        remote_provider == ep.domain_->provider_ &&
                        remote_fabric_name == ep.domain_->name_;
                });
            if (ep_iter != endpoints.end())
            {
                auto& ep = *ep_iter;
                std::size_t ep_idx = std::distance(endpoints.begin(), ep_iter);
                std::cout << "Found endpoint I can use to resolve addresses at index " << ep_idx << "!\n";
                std::cout << fi_tostr(iter, FI_TYPE_INFO) << '\n';

                // Insert address into our endpoints av if it doesn't already exist
                void *addr = iter->dest_addr;
                fi_addr_t& fi_addr = ep.add_address(addr);

                auto& message = send_messages[ep_idx];
                auto& mr = mr_send_messages[ep_idx];
                auto& context = send_contexts[ep_idx];

                message.header.type = hpx::parcelset::message_type::register_addr;
                auto local_addr = ep.get_local_address();
                std::memcpy(message.payload.data(), local_addr.data(), local_addr.size());

                // Post message
                while (true)
                {
                    status = fi_send(ep.endpoint_, &message,
                        sizeof(hpx::parcelset::message::header_type) + local_addr.size(),
                        fi_mr_desc(mr), fi_addr, &context);
                    if (status) break;
                    if (status == hpx::parcelset::fabric_status::eagain()) continue;
                    if (!status)
                    {
                        std::cout << status.what() << '\n';
                        throw status;
                    }
                }
                break;
            }
        }
        fi_freeinfo(info);
    }
    else
    {
        std::cout << "Could not find infos about remote host...\n";
    }

    bool running = true;

    std::cout << "Starting message handling loop!\n";

    enum class app_state {
        initializing, pingpong1
    };

    app_state state = app_state::initializing;

    // The main message handling loop ...
    while (running)
    {
        // Poll all endpoints
        for(std::size_t idx = 0; idx != endpoints.size(); ++idx)
        {
            auto& ep = endpoints[idx];
            fi_cq_msg_entry entry;
            // First look if we received something!
            ssize_t num_read = fi_cq_read(ep.rxcq_, &entry, 1);
            if (num_read == 1)
            {
                std::cout << "Got message " << ep.domain_->provider_ << "\n";
                if (entry.flags & FI_RECV)
                {
                        std::cout << "  FI_RECV\n";
                }
                if (entry.flags & FI_SEND)
                {
                        std::cout << "  FI_SEND\n";
                }
                if (entry.flags & FI_MSG)
                {
                        std::cout << "  FI_MSG\n";
                }
                if (entry.flags & FI_RMA)
                {
                        std::cout << "  FI_RMA\n";
                }
                std::cout << "   length: " << entry.len << '\n';
                auto& message = recv_messages[idx];
                auto& mr = mr_recv_messages[idx];
                auto& context = recv_contexts[idx];
                switch (message.header.type)
                {
                    case hpx::parcelset::message_type::register_addr:
                        {
                            std::cout << "Packing up endpoint addresses!\n";
                            auto& send_message = send_messages[idx];
                            auto& send_mr = mr_send_messages[idx];
                            auto& send_context = send_contexts[idx];

                            send_message.header.type = hpx::parcelset::message_type::resolved_addr;

                            char *payload = send_message.payload.data();

                            std::uint32_t num_endpoints = endpoints.size();
                            std::memcpy(payload, &num_endpoints, sizeof(std::uint32_t));
                            payload += sizeof(std::uint32_t);
                            for (auto& ep: endpoints)
                            {
                                std::memcpy(payload, &ep.domain_->provider_, sizeof(hpx::parcelset::fabric_provider));
                                payload += sizeof(hpx::parcelset::fabric_provider);

                                std::uint64_t length = ep.domain_->name_.size();
                                std::memcpy(payload, &length, sizeof(std::uint64_t));
                                payload += sizeof(length);

                                std::memcpy(payload, ep.domain_->name_.data(), length);
                                payload += length;

                                auto local_addr = ep.get_local_address();
                                length = local_addr.size();
                                std::memcpy(payload, &length, sizeof(std::uint64_t));
                                payload += sizeof(length);

                                std::memcpy(payload, local_addr.data(), length);
                                payload += length;
                                if (payload > send_message.payload.end())
                                {
                                    std::cerr << "Got too many endpoints!\n";
                                    break;
                                }
                            }

                            fi_addr_t& fi_addr = ep.add_address(message.payload.data());

                            // Post message
                            while (true)
                            {
                                status = fi_send(ep.endpoint_, &send_message, payload - send_message.payload.data(),
                                    fi_mr_desc(send_mr), fi_addr, &send_context);
                                if (status) break;
                                if (status == hpx::parcelset::fabric_status::eagain()) continue;
                                if (!status)
                                {
                                    std::cout << status.what() << '\n';
                                    throw status;
                                }
                            }
                        }
                        break;
                    case hpx::parcelset::message_type::resolved_addr:
                        {
                            std::cout << "resolved address!\n";
                            char *payload = message.payload.data();
                            std::uint32_t num_remote_endpoints = 0;
                            std::memcpy(&num_remote_endpoints, payload, sizeof(std::uint32_t));
                            payload += sizeof(std::uint32_t);

                            std::cout << "Receiving " << num_remote_endpoints << " endpoints\n";

                            hpx::parcelset::fabric_provider preferred_provider
                                = hpx::parcelset::fabric_provider::invalid;
                            std::size_t preferred_eps = 0;
                            for (std::uint32_t i = 0; i != num_remote_endpoints; ++i)
                            {
                                hpx::parcelset::fabric_provider provider;
                                std::memcpy(&provider, payload, sizeof(provider));
                                payload += sizeof(provider);
                                std::uint64_t length = 0;
                                std::memcpy(&length, payload, sizeof(length));
                                payload += sizeof(length);
                                std::string name(length, '\0');
                                std::memcpy(name.data(), payload, length);
                                payload += length;

                                std::memcpy(&length, payload, sizeof(length));
                                payload += sizeof(length);
                                std::vector<char> remote_addr(length);
                                std::memcpy(remote_addr.data(), payload, length);
                                payload += length;

                                std::cout << "received: " << provider << ", " << name << '\n';

                                if (domains[static_cast<std::size_t>(provider)].empty())
                                    continue;
                                for (auto& ep : endpoints)
                                {
                                    if (ep.domain_->provider_ != provider)
                                        continue;
                                    if (ep.domain_->name_ != name)
                                        continue;

                                    if (preferred_provider == hpx::parcelset::fabric_provider::invalid)
                                    {
                                        preferred_provider = provider;
                                    }
                                    else if (preferred_provider != provider)
                                    {
                                        break;
                                    }

                                    preferred_eps++;

                                    ep.add_address(remote_addr.data());
                                }
                            }
                            std::cout << "Preferred: " << preferred_provider << " " << preferred_eps << '\n';
                            state = app_state::pingpong1;
                        }
                        break;
                    case hpx::parcelset::message_type::action:
                        break;
                    case hpx::parcelset::message_type::quit:
                        running = false;
                        break;
                    default:
                        std::abort();
                }

                // Post receive buffer
                while (true)
                {
                    status = fi_recv(ep.endpoint_, &message, sizeof(message), fi_mr_desc(mr),
                        0, &context);
                    if (status) break;
                    if (status == hpx::parcelset::fabric_status::eagain()) continue;
                    if (!status)
                    {
                        std::cout << status.what() << '\n';
                        throw status;
                    }
                }
            }
            else
            {
                if (num_read != 0)
                {
                    status = num_read;
                    if (status != hpx::parcelset::fabric_status::eagain())
                    {
                        std::cout << status.what() << '\n';
                        throw status;
                    }
                }
            }
            // Second look if some transfer completed...!
            num_read = fi_cq_read(ep.txcq_, &entry, 1);
            if (num_read == 1)
            {
                std::cout << "Sent message " << ep.domain_->provider_ << "\n";
            }
            else
            {
                if (num_read != 0)
                {
                    status = num_read;
                    if (status != hpx::parcelset::fabric_status::eagain())
                    {
                        std::cout << status.what() << '\n';
                        throw status;
                    }
                }
            }
        }
    }

    // Closing all MRs
    for (auto& mr: mr_recv_messages)
    {
        if (mr != &no_mr)
            fi_close(&mr->fid);
    }
    for (auto& mr: mr_send_messages)
    {
        if (mr != &no_mr)
            fi_close(&mr->fid);
    }
}
