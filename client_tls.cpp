/*
Copyright 2022 The Photon Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include <iostream>
#include <string> 
#include <iomanip>
#include <cctype>

#include <photon/common/alog.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/net/socket.h>
#include <photon/net/security-context/tls-stream.h>


using namespace photon;

void printHex(char c) {
    unsigned char byte = static_cast<unsigned char>(c);
    // std::cout << std::hex << std::setw(2) << std::setfill('0')
    //         << static_cast<int>(byte) << " ";
    // if (std::isprint(byte)) {
    //     std::cout << "(" << byte << ")";
    // } else {
    //     std::cout << "(.)";
    // }
    // std::cout << "\n";
    if (std::isprint(byte)) {
        std::cout << byte;
    } else {
        std::cout << ".";
    }
}
// Parse WebSocket frame
bool parse_websocket_frame(const char* buf, size_t len, std::string& payload, uint8_t& opcode) {
    if (len < 2) return false;
    opcode = buf[0] & 0x0F;
    uint8_t fin = buf[0] & 0x80;
    uint64_t payload_len = buf[1] & 0x7F;
    size_t header_len = 2;
    if (payload_len == 126) {
        if (len < 4) return false;
        payload_len = (static_cast<uint64_t>(buf[2]) << 8) | buf[3];
        header_len = 4;
    } else if (payload_len == 127) {
        if (len < 10) return false;
        payload_len = 0;
        for (int i = 2; i < 10; ++i) {
            payload_len = (payload_len << 8) | static_cast<uint8_t>(buf[i]);
        }
        header_len = 10;
    }
    if (len < header_len + payload_len || !fin) return false;
    payload.assign(buf + header_len, payload_len);
    return true;
}

// Send a WebSocket pong frame (opcode 0xA)
ssize_t send_pong_frame(net::ISocketStream* tls, const char* data, size_t len) {
    char frame[128];
    size_t frame_len = 0;

    frame[frame_len++] = 0x8A; // FIN=1, opcode=0xA
    frame[frame_len++] = (char)len;
    memcpy(frame + frame_len, data, len);
    frame_len += len;

    for (size_t i = 0; i < frame_len; ++i) {
        printHex(frame[i]);
    }
    std::cout << "\n";

    return tls->send(frame, frame_len);
}

// Simple function to send a WebSocket text frame (opcode 0x1)
ssize_t send_websocket_frame(net::ISocketStream* tls, const char* data, size_t len) {
    char frame[4096];
    size_t frame_len = 0;

    // WebSocket frame: opcode 0x1 (text), FIN bit set
    frame[frame_len++] = 0x81; // FIN=1, opcode=0x1

    // Payload length
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

    // Payload
    memcpy(frame + frame_len, data, len);
    frame_len += len;

    for (size_t i = 0; i < frame_len; ++i) {
        printHex(frame[i]);
    }
    std::cout << "\n";
    // Send frame
    return tls->send(frame, frame_len);
}

// Convert IPAddr to string (IPv4 only for simplicity, as Binance uses IPv4)
std::string ipaddr_to_string(const net::IPAddr& addr) {
    if (addr.is_ipv4()) {
        uint32_t nl = addr.to_nl(); // Network byte order
        struct in_addr in;
        in.s_addr = nl;
        return inet_ntoa(in);
    }
    return "unknown"; // Fallback for IPv6 or undefined
}

int main(int argc, char** argv) {
    if (photon::init(INIT_EVENT_IOURING, INIT_IO_NONE)) {
        LOG_ERROR_RETURN(0, -1, "Photon init failed");
    }
    DEFER(photon::fini());

    auto ctx = net::new_tls_context(nullptr, nullptr, nullptr);
    if (!ctx) {
        LOG_ERROR_RETURN(0, -1, "TLS context creation failed");
    }
    DEFER(delete ctx);

    auto cli = net::new_tls_client(ctx, net::new_iouring_tcp_client(), true);
    DEFER(delete cli);

    net::ISocketStream* tls = nullptr; 
    for (int attempt = 0; attempt < 3; ++attempt) {
        net::IPAddr addr("52.194.53.108");
        std::string ip_str = ipaddr_to_string(addr);
        LOG_INFO("Attempt `, Resolved IP: `", attempt + 1, ip_str.c_str());
        if (addr.undefined() || ip_str == "0.0.0.0") {
            LOG_WARN("DNS resolution failed");
            photon::thread_sleep(1);
            continue;
        }

        tls = cli->connect(net::EndPoint{addr, 9443});
        if (tls) break;
        LOG_ERROR("Failed to connect to `:9443, retrying, errno=`", ip_str.c_str(), errno);
        photon::thread_sleep(1);
    }
    if (!tls) {
        LOG_ERROR("Failed to connect after retries, errno=`", errno);
        return -1;
    }
    DEFER(delete tls);

    // WebSocket handshake
    const char* handshake = "GET /ws HTTP/1.1\r\n"
                            "Host: stream.binance.com\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                            "Sec-WebSocket-Version: 13\r\n"
                            "\r\n";
    if (tls->send(handshake, strlen(handshake)) < 0) {
        LOG_ERROR_RETURN(0, -1, "Failed to send WebSocket handshake");
    }

    // Receive handshake response
    char buf[4096];
    ssize_t n = tls->recv(buf, sizeof(buf));
    if (n <= 0) {
        LOG_ERROR_RETURN(0, -1, "Failed to receive handshake response");
    }
    buf[n] = '\0';
    LOG_INFO("Handshake response: `", buf);

    // Subscribe to btcusdt@trade stream
    const char* subscribe = "{\"method\":\"SUBSCRIBE\",\"params\":[\"btcusdt@trade\"],\"id\":1}";
    if (send_websocket_frame(tls, subscribe, strlen(subscribe)) < 0) {
        LOG_ERROR_RETURN(0, -1, "Failed to send subscription");
    }

    // Receive stream data
    while (true) {
        n = tls->recv(buf, sizeof(buf));
        if (n <= 0) {
            LOG_ERROR("Connection closed or error");
            break;
        }
        buf[n] = '\0';
        LOG_INFO("Received: ", buf);
        // std::cout << buf << "\n";
    }

    return 0;
}