/*
Copyright 2022 The Photon Authors
Licensed under the Apache License, Version 2.0
*/
#include <vector> 
#include <iostream>
#include <string>
#include <iomanip>
#include <cctype>
#include <arpa/inet.h>
#include <netdb.h> // For getaddrinfo
#include <photon/common/alog.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/net/socket.h>
#include <photon/net/security-context/tls-stream.h>

using namespace photon;

// Convert IPAddr to string for logging
std::string ipaddr_to_string(const net::IPAddr& addr) {
    if (addr.is_ipv4()) {
        struct in_addr in;
        in.s_addr = addr.to_nl();
        return inet_ntoa(in);
    } else if (addr.is_ipv6()) {
        LOG_WARN("IPv6 address detected, not fully supported for string conversion");
        return "ipv6-unsupported";
    }
    return "unknown";
}

// DNS resolution using getaddrinfo (standard POSIX, not Photon)
net::IPAddr resolve_domain(const char* hostname) {
    struct addrinfo hints = {};
    struct addrinfo* result = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ALL | AI_V4MAPPED;
    hints.ai_family = AF_UNSPEC;

    int ret = getaddrinfo(hostname, nullptr, &hints, &result);
    if (ret != 0) {
        LOG_ERROR("Failed to resolve `, error: `", hostname, gai_strerror(ret));
        return net::IPAddr(); // Undefined IPAddr on failure
    }

    net::IPAddr addr;
    for (auto* cur = result; cur != nullptr; cur = cur->ai_next) {
        if (cur->ai_family == AF_INET) {
            auto sock_addr = (struct sockaddr_in*)cur->ai_addr;
            addr = net::IPAddr(sock_addr->sin_addr);
            break; // Take the first IPv4 address
        } else if (cur->ai_family == AF_INET6) {
            auto sock_addr = (struct sockaddr_in6*)cur->ai_addr;
            addr = net::IPAddr(sock_addr->sin6_addr);
            break; // Take the first IPv6 address if no IPv4
        }
    }
    freeaddrinfo(result);
    return addr;
}

ssize_t send_pong_frame(net::ISocketStream* tls, const char* data, size_t len) {
    char frame[128];
    size_t frame_len = 0;
    frame[frame_len++] = 0x8A; // Pong opcode, FIN bit set
    frame[frame_len++] = (char)len;
    memcpy(frame + frame_len, data, len);
    frame_len += len;
    return tls->send(frame, frame_len);
}

ssize_t send_websocket_frame(net::ISocketStream* tls, const char* data, size_t len) {
    char frame[4096];
    size_t frame_len = 0;
    frame[frame_len++] = 0x81; // Text frame, FIN bit set
    if (len <= 125) {
        frame[frame_len++] = (char)len;
    } else if (len <= 65535) {
        frame[frame_len++] = 126;
        frame[frame_len++] = (len >> 8) & 0xFF;
        frame[frame_len++] = len & 0xFF;
    } else {
        frame[frame_len++] = 127;
        for (int i = 7; i >= 0; --i) {
            frame[frame_len++] = (len >> (i * 8)) & 0xFF;
        }
    }
    memcpy(frame + frame_len, data, len);
    frame_len += len;
    return tls->send(frame, frame_len);
}

void* websocket_handler(void* arg) {
    std::string symbol = static_cast<const char*>(arg);
    std::string subscribe_msg = "{\"method\":\"SUBSCRIBE\",\"params\":[\"" + symbol + "@trade\"],\"id\":" + (symbol == "btcusdt" ? "1" : "2") + "}";

    auto ctx = net::new_tls_context(nullptr, nullptr, nullptr);
    if (!ctx) {
        LOG_ERROR_RETURN(0, nullptr, "TLS context creation failed");
    }
    DEFER(delete ctx);

    auto cli = net::new_tls_client(ctx, net::new_iouring_tcp_client(), true);
    DEFER(delete cli);

    net::ISocketStream* tls = nullptr;
    net::IPAddr addr;
    for (int attempt = 0; attempt < 3; ++attempt) {
        addr = resolve_domain("stream.binance.com"); // Use getaddrinfo instead of Photon
        if (addr.undefined()) {
            LOG_WARN("DNS resolution failed for `", symbol.c_str());
            photon::thread_sleep(1);
            continue;
        }
        LOG_INFO("Resolved stream.binance.com to ` for `", ipaddr_to_string(addr).c_str(), symbol.c_str());
        tls = cli->connect(net::EndPoint{addr, 9443});
        if (tls) break;
        LOG_ERROR("Failed to connect for `, retrying, errno=`", symbol.c_str(), errno);
        photon::thread_sleep(1);
    }
    if (!tls) {
        LOG_ERROR("Failed to connect for ` after retries, errno=`", symbol.c_str(), errno);
        return nullptr;
    }
    DEFER(delete tls);

    const char* handshake = "GET /ws HTTP/1.1\r\n"
                            "Host: stream.binance.com\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                            "Sec-WebSocket-Version: 13\r\n"
                            "\r\n";
    if (tls->send(handshake, strlen(handshake)) < 0) {
        LOG_ERROR_RETURN(0, nullptr, "Failed to send WebSocket handshake for `", symbol.c_str());
    }

    char buf[1024]; // Increased buffer size
    ssize_t n = tls->recv(buf, sizeof(buf));
    if (n <= 0) {
        LOG_ERROR_RETURN(0, nullptr, "Failed to receive handshake response for `", symbol.c_str());
    }
    buf[n] = '\0';
    LOG_INFO("Handshake response for `: `", symbol.c_str(), buf);

    if (send_websocket_frame(tls, subscribe_msg.c_str(), subscribe_msg.size()) < 0) {
        LOG_ERROR_RETURN(0, nullptr, "Failed to send subscription for `", symbol.c_str());
    }

    // Buffer for accumulating data
    std::vector<char> recv_buffer;
    recv_buffer.reserve(8192);
    
    // Fragment reassembly state
    std::string fragmented_message;
    bool in_fragmented_message = false;
    uint8_t fragmented_opcode = 0;

    while (true) {
        char temp_buf[4096];
        n = tls->recv(temp_buf, sizeof(temp_buf));
        if (n <= 0) {
            LOG_ERROR("Connection closed or error for `, errno=`", symbol.c_str(), errno);
            break;
        }
        
        // Append new data to buffer
        recv_buffer.insert(recv_buffer.end(), temp_buf, temp_buf + n);

        // Process complete frames
        size_t processed = 0;
        while (processed < recv_buffer.size()) {
            // Need at least 2 bytes for the header
            if (recv_buffer.size() - processed < 2) break;

            // Decode WebSocket frame header
            uint8_t first_byte = static_cast<uint8_t>(recv_buffer[processed]);
            uint8_t second_byte = static_cast<uint8_t>(recv_buffer[processed + 1]);
            
            uint8_t fin = (first_byte & 0x80) >> 7;
            uint8_t opcode = first_byte & 0x0F;
            uint8_t mask = (second_byte & 0x80) >> 7;
            uint64_t payload_len = second_byte & 0x7F;
            size_t header_len = 2;

            // Validate mask bit (should be 0 for server-to-client messages)
            if (mask != 0) {
                LOG_ERROR("Received masked frame from server for `, which is invalid", symbol.c_str());
                processed += header_len;
                continue;
            }

            // Calculate payload length and header length
            if (payload_len == 126) {
                if (recv_buffer.size() - processed < 4) break;
                payload_len = (static_cast<uint64_t>(recv_buffer[processed + 2]) << 8) | 
                             static_cast<uint8_t>(recv_buffer[processed + 3]);
                header_len = 4;
            } else if (payload_len == 127) {
                if (recv_buffer.size() - processed < 10) break;
                payload_len = 0;
                for (int i = 2; i < 10; ++i) {
                    payload_len = (payload_len << 8) | static_cast<uint8_t>(recv_buffer[processed + i]);
                }
                header_len = 10;
            }

            // Check if we have the full frame
            if (recv_buffer.size() - processed < header_len + payload_len) {
                break; // Wait for more data
            }

            // Extract the payload
            std::string payload(recv_buffer.begin() + processed + header_len, 
                              recv_buffer.begin() + processed + header_len + payload_len);

            LOG_DEBUG("Frame: opcode=`, fin=`, payload_len=`", opcode, fin, payload_len);

            // Process the frame based on opcode and fragmentation
            if (opcode == 0x1 || opcode == 0x2) { // Text or Binary frame (start of message)
                if (fin) {
                    // Complete message in single frame
                    if (opcode == 0x1) { // Text frame
                        std::cout << "< " << payload << std::endl;
                    }
                } else {
                    // Start of fragmented message
                    in_fragmented_message = true;
                    fragmented_opcode = opcode;
                    fragmented_message = payload;
                }
            } else if (opcode == 0x0) { // Continuation frame
                if (in_fragmented_message) {
                    fragmented_message += payload;
                    if (fin) {
                        // End of fragmented message
                        if (fragmented_opcode == 0x1) { // Text message
                            std::cout << "< " << fragmented_message << std::endl;
                        }
                        in_fragmented_message = false;
                        fragmented_message.clear();
                        fragmented_opcode = 0;
                    }
                }
            } else if (opcode == 0x9) { // Ping frame
                if (send_pong_frame(tls, payload.c_str(), payload.size()) < 0) {
                    LOG_ERROR("Failed to send pong for `", symbol.c_str());
                    break;
                }
                LOG_INFO("Sent pong for `", symbol.c_str());
            } else if (opcode == 0xA) { // Pong frame
                LOG_INFO("Received pong for `", symbol.c_str());
            } else if (opcode == 0x8) { // Close frame
                LOG_INFO("Received close frame for `", symbol.c_str());
                break;
            } else {
                LOG_WARN("Unknown opcode ` for `, payload_len=`", opcode, symbol.c_str(), payload_len);
            }

            // Move to the next frame
            processed += header_len + payload_len;
        }

        // Remove processed data from buffer
        if (processed > 0) {
            recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + processed);
        }

        // Prevent buffer from growing too large
        if (recv_buffer.size() > 16384) {
            LOG_WARN("Buffer too large, clearing for `", symbol.c_str());
            recv_buffer.clear();
            fragmented_message.clear();
            in_fragmented_message = false;
            fragmented_opcode = 0;
        }
    }

    return nullptr;
}

int main(int argc, char** argv) {
    if (photon::init(INIT_EVENT_IOURING, INIT_IO_NONE)) {
        LOG_ERROR_RETURN(0, -1, "Photon init failed");
    }
    DEFER(photon::fini());

    photon::thread_create(&websocket_handler, const_cast<char*>("ethusdt"));
    photon::thread_create(&websocket_handler, const_cast<char*>("btcusdt"));

    while (true) {
        photon::thread_usleep(1000 * 1000);
    }

    return 0;
}