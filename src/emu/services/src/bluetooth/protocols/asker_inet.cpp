/*
 * Copyright (c) 2022 EKA2L1 Team
 * 
 * This file is part of EKA2L1 project.
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <services/bluetooth/protocols/asker_inet.h>
#include <services/bluetooth/protocols/btmidman_inet.h>
#include <services/bluetooth/protocols/common_inet.h>

#include <common/log.h>

extern "C" {
#include <uv.h>
}

namespace eka2l1::epoc::bt {
    static constexpr std::uint32_t MAX_RETRY_ATTEMPT = 15;

    asker_inet::asker_inet(midman_inet *midman)
        : asker_(nullptr)
        , asker_retry_timer_(nullptr)
        , retry_times_(0)
        , callback_(nullptr)
        , in_transfer_data_callback_(true)
        , asker_id_(midman->new_asker_id()) {
        request_done_evt_.set();
    }
    
    asker_inet::~asker_inet() {
    }

    void asker_inet::handle_request_failure() {
        if (retry_times_ >= MAX_RETRY_ATTEMPT) {
            asker_retry_timer_->stop();

            request_done_evt_.set();
            callback_(nullptr, BT_COMM_INET_ERROR_RECV_FAILED);
        } else {
            keep_sending_data();
        }
        return;
    }

    void asker_inet::listen_to_data() {
        asker_->on<uvw::udp_data_event>([this](const uvw::udp_data_event &event, uvw::udp_handle &handle) {
            if (event.length < 4) {
                return;
            }

            if (memcmp(event.data.get(), &asker_id_, sizeof(std::uint32_t)) != 0) {
                return;
            }

            in_transfer_data_callback_ = true;

            if (callback_(event.data.get() + 4, event.length - 4)) {
                // Callback may turn it off through another request call
                if (in_transfer_data_callback_) {
                    request_done_evt_.set();
                    asker_retry_timer_->stop();
                }

                in_transfer_data_callback_ = false;

                return;
            }
        });

        asker_->on<uvw::error_event>([this](const uvw::error_event &event, uvw::udp_handle &handle) {
            handle_request_failure();
        });

        asker_->recv();
    }

    void asker_inet::keep_sending_data() {
        asker_retry_timer_->stop();
        retry_times_++;

        if (asker_->send(*reinterpret_cast<const sockaddr *>(&dest_), buffer_.data(), static_cast<std::uint32_t>(buffer_.size())) < 0) {
            handle_request_failure();
        } else {
            asker_retry_timer_->on<uvw::timer_event>([this](const uvw::timer_event &event, uvw::timer_handle &handle) {
                handle_request_failure();
            });

            asker_retry_timer_->start(std::chrono::milliseconds(SEND_TIMEOUT_RETRY), std::chrono::milliseconds(SEND_TIMEOUT_RETRY));
        }
    }

    void asker_inet::send_request_with_retries(const epoc::socket::saddress &addr, char *request, const std::size_t request_size,
        response_callback response_cb, const bool sync) {
        if (!in_transfer_data_callback_) {
            request_done_evt_.wait();
        }

        // Check address validity
        sockaddr *addr_host = nullptr;
        GUEST_TO_BSD_ADDR(addr, addr_host);

        if (addr_host == nullptr) {
            callback_(nullptr, BT_COMM_INET_INVALID_ADDR);
            return;
        }

        request_done_evt_.reset();
        in_transfer_data_callback_ = false;

        retry_times_ = 0;
        callback_ = response_cb;
        dest_addr_ = addr;

        buffer_.clear();
        buffer_.insert(buffer_.begin(), reinterpret_cast<char*>(&asker_id_), reinterpret_cast<char*>(&asker_id_ + 1));
        buffer_.insert(buffer_.end(), request, request + request_size);

        if (!send_data_task_) {
            send_data_task_ = libuv::create_task([this]() {
                sockaddr *addr_host = nullptr;
                GUEST_TO_BSD_ADDR(dest_addr_, addr_host);

                auto default_loop = uvw::loop::get_default();

                if (!asker_) {
                    asker_ = default_loop->resource<uvw::udp_handle>();

                    sockaddr_in6 bind;
                    std::memset(&bind, 0, sizeof(sockaddr_in6));
                    bind.sin6_family = AF_INET6;

                    asker_->bind(*reinterpret_cast<sockaddr*>(&bind));
                }

                if (!asker_retry_timer_) {
                    asker_retry_timer_ = default_loop->resource<uvw::timer_handle>();
                }

                if (addr_host->sa_family == AF_INET) {
                    std::memcpy(&dest_, addr_host, sizeof(sockaddr_in));
                } else {
                    std::memcpy(&dest_, addr_host, sizeof(sockaddr_in6));
                }

                listen_to_data();
                keep_sending_data();
            });
        }

        libuv::default_looper->post_task(send_data_task_);

        if (sync) {
            request_done_evt_.wait();
        }
    }

    std::uint32_t asker_inet::ask_for_routed_port(const std::uint16_t virtual_port, const epoc::socket::saddress &dev_addr) {
        std::uint32_t local_result = 0;

        ask_for_routed_port_async(virtual_port, dev_addr, [&local_result, this](const std::int64_t res) {
            if (res <= 0) {
                local_result = 0;
            } else {
                local_result = static_cast<std::uint32_t>(res);
            }
        });

        request_done_evt_.wait();
        return local_result;
    }

    void asker_inet::ask_for_routed_port_async(const std::uint16_t virtual_port, const epoc::socket::saddress &dev_addr, port_ask_done_callback cb) {
        char buf[3];
        buf[0] = QUERY_OPCODE_GET_REAL_PORT_FROM_VIRTUAL_PORT;
        buf[1] = static_cast<char>(virtual_port & 0xFF);
        buf[2] = static_cast<char>((virtual_port >> 8) & 0xFF);

        send_request_with_retries(dev_addr, buf, 3, [cb](const char *response, const ssize_t size) {
            if (size <= 0) {
                cb(static_cast<std::int64_t>(size));
                return false;
            } else {
                if (response[0] != (QUERY_OPCODE_GET_REAL_PORT_FROM_VIRTUAL_PORT + QUERY_OPCODE_RESULT_START)) {
                    return false;
                }

                std::uint32_t result = 0;
                std::memcpy(&result, response + 1, 4);

                cb(static_cast<std::int64_t>(result));
                return true;
            }
        });
    }

    bool asker_inet::check_is_real_port_mapped(const epoc::socket::saddress &addr, const std::uint32_t real_port) {
        std::vector<char> request_data;
        request_data.push_back(QUERY_OPCODE_IS_REAL_PORT_MAPPED_TO_VIRTUAL_PORT);
        request_data.push_back(static_cast<char>(real_port));
        request_data.push_back(static_cast<char>(real_port >> 8));
        request_data.push_back(static_cast<char>(real_port >> 16));
        request_data.push_back(static_cast<char>(real_port >> 24));

        bool result = false;
        send_request_with_retries(addr, request_data.data(), request_data.size(), [&result](const char *buf, const std::int64_t nread) {
            if ((nread <= 0) || (buf[0] != (QUERY_OPCODE_RESULT_START + QUERY_OPCODE_IS_REAL_PORT_MAPPED_TO_VIRTUAL_PORT))) {
                return false;
            }

            result = (buf[1] == '1');
            return true;
        }, true);

        return result;
    }

    std::optional<device_address> asker_inet::get_device_address(const epoc::socket::saddress &dest_friend) {
        std::optional<device_address> addr_result;
        char buffer = QUERY_OPCODE_GET_VIRTUAL_BLUETOOTH_ADDRESS;

        send_request_with_retries(dest_friend, &buffer, 1, [&addr_result](const char *buf, const std::int64_t nread) {
            if ((nread <= 0) || (buf[0] != (QUERY_OPCODE_RESULT_START + QUERY_OPCODE_GET_VIRTUAL_BLUETOOTH_ADDRESS))) {
                return false;
            }

            if (nread < sizeof(device_address) + 1) {
                LOG_WARN(SERVICE_BLUETOOTH, "Get bluetooth MAC (virtual) packet size is insufficient!");
                return false;
            }

            device_address result;
            std::memcpy(&result, buf + 1, sizeof(device_address));

            addr_result = result;
            return true;
        }, true);

        return addr_result;
    }

    void asker_inet::get_device_address_async(const epoc::socket::saddress &dest_friend, device_address_get_done_callback callback) {
        char buffer = QUERY_OPCODE_GET_VIRTUAL_BLUETOOTH_ADDRESS;
        send_request_with_retries(dest_friend, &buffer, 1, [callback](const char *buf, const std::int64_t nread) {
            if (nread <= 0) {
                callback(nullptr);
                return false;
            }
            
            if (buf[0] != (QUERY_OPCODE_RESULT_START + QUERY_OPCODE_GET_VIRTUAL_BLUETOOTH_ADDRESS)) {
                return false;
            }

            if (nread < sizeof(device_address) + 1) {
                LOG_WARN(SERVICE_BLUETOOTH, "Get bluetooth MAC (virtual) packet size is insufficient!");
                return false;
            }

            device_address result;
            std::memcpy(&result, buf + 1, sizeof(device_address));

            callback(&result);
            return true;
        }, false);
    }

    void asker_inet::get_device_name_async(const epoc::socket::saddress &dest_friend, name_get_done_callback callback) {
        char buffer = QUERY_OPCODE_GET_NAME;
        send_request_with_retries(dest_friend, &buffer, 1, [callback](const char *buf, const std::int64_t nread) {
            if (nread <= 0) {
                callback(nullptr, -1);
                return false;
            }
            
            if (buf[0] != (QUERY_OPCODE_RESULT_START + QUERY_OPCODE_GET_NAME)) {
                return false;
            }

            if (nread < buf[1] + 2) {
                LOG_WARN(SERVICE_BLUETOOTH, "Get name packet size is insufficient!");
                return false;
            }

            callback(buf + 2, buf[1]);
            return true;
        }, false);
    }
}
