/*
Copyright 2022 The Photon Authors
Licensed under the Apache License, Version 2.0
*/
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <photon/common/alog.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/net/socket.h>
#include <photon/net/security-context/tls-stream.h>
//#include <photon/net/base_socket.h>  // For ISocketBase

using namespace photon;

// WebSocket connection state
struct WebSocketConnection {
    std::string symbol;
    net::ISocketStream* tls = nullptr;
    int sockfd = -1;
    
    // Frame processing state
    std::vector<char> recv_buffer;
    std::string fragmented_message;
    bool in_fragmented_message = false;
    uint8_t fragmented_opcode = 0;
    
    // Connection health
    time_t last_activity = 0;
    bool connected = false;
    
    WebSocketConnection(const std::string& sym) : symbol(sym) {
        recv_buffer.reserve(8192);
        last_activity = time(nullptr);
    }
    
    ~WebSocketConnection() {
        if (tls) delete tls;
    }
};

class MultiWebSocketManager {
private:
    int epfd = -1;
    int evfd = -1;
    struct epoll_event events[32]; // Increased for multiple connections
    std::unordered_map<int, std::unique_ptr<WebSocketConnection>> connections;
    std::vector<std::string> symbols;
    
    net::TLSContext* ctx = nullptr;
    net::ISocketClient* cli = nullptr;
    
public:
    MultiWebSocketManager(const std::vector<std::string>& syms) : symbols(syms) {}
    
    ~MultiWebSocketManager() {
        cleanup();
    }
    
    int init() {
        // Initialize epoll
        epfd = epoll_create1(EPOLL_CLOEXEC);
        if (epfd < 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to create epoll");
        }
        
        evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (evfd < 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to create eventfd");
        }
        
        // Add eventfd to epoll for shutdown signaling
        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = evfd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, evfd, &ev) < 0) {
            LOG_ERRNO_RETURN(0, -1, "failed to add eventfd to epoll");
        }
        
        // Initialize TLS context
        ctx = net::new_tls_context(nullptr, nullptr, nullptr);
        if (!ctx) {
            LOG_ERROR_RETURN(0, -1, "TLS context creation failed");
        }
        
        cli = net::new_tls_client(ctx, net::new_iouring_tcp_client(), true);
        if (!cli) {
            LOG_ERROR_RETURN(0, -1, "TLS client creation failed");
        }
        
        return 0;
    }
    
    void cleanup() {
        connections.clear();
        if (cli) { delete cli; cli = nullptr; }
        if (ctx) { delete ctx; ctx = nullptr; }
        if (epfd >= 0) { epfd = -1; }
        if (evfd >= 0) { evfd = -1; }
    }
    
    net::IPAddr resolve_domain(const char* hostname) {
        struct addrinfo hints = {};
        struct addrinfo* result = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_ALL | AI_V4MAPPED;
        hints.ai_family = AF_UNSPEC;

        int ret = getaddrinfo(hostname, nullptr, &hints, &result);
        if (ret != 0) {
            LOG_ERROR("Failed to resolve `, error: `", hostname, gai_strerror(ret));
            return net::IPAddr();
        }

        net::IPAddr addr;
        for (auto* cur = result; cur != nullptr; cur = cur->ai_next) {
            if (cur->ai_family == AF_INET) {
                auto sock_addr = (struct sockaddr_in*)cur->ai_addr;
                addr = net::IPAddr(sock_addr->sin_addr);
                break;
            }
        }
        freeaddrinfo(result);
        return addr;
    }
    
    // Extract socket FD from TLS stream using ISocketBase interface
    int get_socket_fd(net::ISocketStream* stream) {
        // Try to cast to ISocketBase since TLSSocketStream implements it
        auto* socket_base = dynamic_cast<photon::net::ISocketBase*>(stream);
        if (!socket_base) {
            // Alternative: Try calling get_underlay_fd directly if the method exists
            // Some implementations might have this method without ISocketBase interface
            LOG_ERROR("Stream does not implement ISocketBase interface");
            return -1;
        }
        
        int fd = socket_base->get_underlay_fd();
        if (fd < 0) {
            LOG_ERROR("Failed to get underlying file descriptor");
            return -1;
        }
        
        LOG_DEBUG("Retrieved socket fd: `", fd);
        return fd;
    }
    
    bool connect_websocket(const std::string& symbol) {
        auto conn = std::make_unique<WebSocketConnection>(symbol);
        
        // DNS resolution with retry
        net::IPAddr addr;
        for (int attempt = 0; attempt < 3; ++attempt) {
            addr = resolve_domain("stream.binance.com");
            if (!addr.undefined()) break;
            LOG_WARN("DNS resolution failed for `, retry `", symbol.c_str(), attempt);
            photon::thread_sleep(1);
        }
        
        if (addr.undefined()) {
            LOG_ERROR("Failed to resolve domain for `", symbol.c_str());
            return false;
        }
        
        // Connect
        conn->tls = cli->connect(net::EndPoint{addr, 9443});
        if (!conn->tls) {
            LOG_ERROR("Failed to connect for `", symbol.c_str());
            return false;
        }
        
        // Get socket FD and add to epoll
        conn->sockfd = get_socket_fd(conn->tls);
        if (conn->sockfd < 0) {
            LOG_ERROR("Failed to get socket fd for `", symbol.c_str());
            return false;
        }
        
        LOG_INFO("Got socket fd ` for ` connection", conn->sockfd, symbol.c_str());
        
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
        ev.data.fd = conn->sockfd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, conn->sockfd, &ev) < 0) {
            LOG_ERRNO_RETURN(0, false, "failed to add socket to epoll for `", symbol.c_str());
        }
        
        // WebSocket handshake
        const char* handshake = "GET /ws HTTP/1.1\r\n"
                                "Host: stream.binance.com\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                "Sec-WebSocket-Version: 13\r\n"
                                "\r\n";
        
        if (conn->tls->send(handshake, strlen(handshake)) < 0) {
            LOG_ERROR("Failed to send handshake for `", symbol.c_str());
            return false;
        }
        
        // Receive handshake response
        char buf[1024];
        ssize_t n = conn->tls->recv(buf, sizeof(buf));
        if (n <= 0) {
            LOG_ERROR("Failed to receive handshake response for `", symbol.c_str());
            return false;
        }
        buf[n] = '\0';
        LOG_INFO("Handshake response for `: `", symbol.c_str(), buf);
        
        // Send subscription
        std::string subscribe_msg = "{\"method\":\"SUBSCRIBE\",\"params\":[\"" + symbol + "@trade\"],\"id\":" + std::to_string(connections.size() + 1) + "}";
        if (send_websocket_frame(conn->tls, subscribe_msg.c_str(), subscribe_msg.size()) < 0) {
            LOG_ERROR("Failed to send subscription for `", symbol.c_str());
            return false;
        }
        
        conn->connected = true;
        conn->last_activity = time(nullptr);
        
        // Store connection
        int sockfd = conn->sockfd;
        connections[sockfd] = std::move(conn);
        
        LOG_INFO("Successfully connected WebSocket for ` on fd `", symbol.c_str(), sockfd);
        return true;
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
    
    ssize_t send_pong_frame(net::ISocketStream* tls, const char* data, size_t len) {
        char frame[128];
        size_t frame_len = 0;
        frame[frame_len++] = 0x8A; // Pong opcode, FIN bit set
        frame[frame_len++] = (char)len;
        memcpy(frame + frame_len, data, len);
        frame_len += len;
        return tls->send(frame, frame_len);
    }
    
    void process_websocket_frame(WebSocketConnection* conn, const std::string& payload, uint8_t opcode, bool fin) {
        if (opcode == 0x1 || opcode == 0x2) { // Text or Binary frame
            if (fin) {
                if (opcode == 0x1) {
                    std::cout << "[" << conn->symbol << "] < " << payload << std::endl;
                }
            } else {
                conn->in_fragmented_message = true;
                conn->fragmented_opcode = opcode;
                conn->fragmented_message = payload;
            }
        } else if (opcode == 0x0) { // Continuation frame
            if (conn->in_fragmented_message) {
                conn->fragmented_message += payload;
                if (fin) {
                    if (conn->fragmented_opcode == 0x1) {
                        std::cout << "[" << conn->symbol << "] < " << conn->fragmented_message << std::endl;
                    }
                    conn->in_fragmented_message = false;
                    conn->fragmented_message.clear();
                    conn->fragmented_opcode = 0;
                }
            }
        } else if (opcode == 0x9) { // Ping frame
            if (send_pong_frame(conn->tls, payload.c_str(), payload.size()) < 0) {
                LOG_ERROR("Failed to send pong for `", conn->symbol.c_str());
            } else {
                LOG_DEBUG("Sent pong for `", conn->symbol.c_str());
            }
        } else if (opcode == 0xA) { // Pong frame
            LOG_DEBUG("Received pong for `", conn->symbol.c_str());
        } else if (opcode == 0x8) { // Close frame
            LOG_INFO("Received close frame for `", conn->symbol.c_str());
            conn->connected = false;
        } else {
            LOG_WARN("Unknown opcode ` for `", opcode, conn->symbol.c_str());
        }
    }
    
    void handle_socket_data(int sockfd) {
        auto it = connections.find(sockfd);
        if (it == connections.end()) return;
        
        auto& conn = it->second;
        char temp_buf[4096];
        ssize_t n = conn->tls->recv(temp_buf, sizeof(temp_buf));
        
        if (n <= 0) {
            LOG_ERROR("Connection error for `, removing", conn->symbol.c_str());
            epoll_ctl(epfd, EPOLL_CTL_DEL, sockfd, nullptr);
            connections.erase(it);
            return;
        }
        
        conn->last_activity = time(nullptr);
        conn->recv_buffer.insert(conn->recv_buffer.end(), temp_buf, temp_buf + n);
        
        // Process complete frames
        size_t processed = 0;
        while (processed < conn->recv_buffer.size()) {
            if (conn->recv_buffer.size() - processed < 2) break;

            uint8_t first_byte = static_cast<uint8_t>(conn->recv_buffer[processed]);
            uint8_t second_byte = static_cast<uint8_t>(conn->recv_buffer[processed + 1]);
            
            uint8_t fin = (first_byte & 0x80) >> 7;
            uint8_t opcode = first_byte & 0x0F;
            uint8_t mask = (second_byte & 0x80) >> 7;
            uint64_t payload_len = second_byte & 0x7F;
            size_t header_len = 2;

            if (mask != 0) {
                LOG_ERROR("Received masked frame from server for `", conn->symbol.c_str());
                processed += header_len;
                continue;
            }

            if (payload_len == 126) {
                if (conn->recv_buffer.size() - processed < 4) break;
                payload_len = (static_cast<uint64_t>(conn->recv_buffer[processed + 2]) << 8) | 
                             static_cast<uint8_t>(conn->recv_buffer[processed + 3]);
                header_len = 4;
            } else if (payload_len == 127) {
                if (conn->recv_buffer.size() - processed < 10) break;
                payload_len = 0;
                for (int i = 2; i < 10; ++i) {
                    payload_len = (payload_len << 8) | static_cast<uint8_t>(conn->recv_buffer[processed + i]);
                }
                header_len = 10;
            }

            if (conn->recv_buffer.size() - processed < header_len + payload_len) {
                break; // Wait for more data
            }

            std::string payload(conn->recv_buffer.begin() + processed + header_len, 
                              conn->recv_buffer.begin() + processed + header_len + payload_len);

            process_websocket_frame(conn.get(), payload, opcode, fin);
            
            if (!conn->connected) {
                // Connection was closed
                epoll_ctl(epfd, EPOLL_CTL_DEL, sockfd, nullptr);
                connections.erase(it);
                return;
            }

            processed += header_len + payload_len;
        }

        // Remove processed data
        if (processed > 0) {
            conn->recv_buffer.erase(conn->recv_buffer.begin(), conn->recv_buffer.begin() + processed);
        }

        // Prevent buffer overflow
        if (conn->recv_buffer.size() > 16384) {
            LOG_WARN("Buffer too large for `, clearing", conn->symbol.c_str());
            conn->recv_buffer.clear();
            conn->fragmented_message.clear();
            conn->in_fragmented_message = false;
            conn->fragmented_opcode = 0;
        }
    }
    
    void send_ping_to_all() {
        unsigned char ping_frame[] = {0x89, 0x00}; // Ping frame with no payload
        for (auto& [sockfd, conn] : connections) {
            if (conn->connected) {
                if (conn->tls->send((char*)ping_frame, sizeof(ping_frame)) < 0) {
                    LOG_ERROR("Failed to send ping to `", conn->symbol.c_str());
                }
            }
        }
    }
    
    void run() {
        // Connect to all symbols
        for (const auto& symbol : symbols) {
            if (!connect_websocket(symbol)) {
                LOG_ERROR("Failed to connect to `", symbol.c_str());
            }
            photon::thread_sleep(1); // Small delay between connections
        }
        
        LOG_INFO("Connected to ` WebSocket streams", connections.size());
        
        time_t last_ping = time(nullptr);
        
        // Main event loop
        while (!connections.empty()) {
            int nfds = epoll_wait(epfd, events, 32, 30000); // 30 second timeout
            
            if (nfds < 0) {
                if (errno == EINTR) continue;
                LOG_ERRNO_RETURN(0, , "epoll_wait failed");
            }
            
            if (nfds == 0) {
                // Timeout - send ping to keep connections alive
                LOG_DEBUG("Timeout, sending ping to all connections");
                send_ping_to_all();
                last_ping = time(nullptr);
                continue;
            }
            
            // Process events
            for (int i = 0; i < nfds; i++) {
                int fd = events[i].data.fd;
                uint32_t ev = events[i].events;
                
                if (fd == evfd) {
                    // Shutdown signal
                    LOG_INFO("Shutdown signal received");
                    return;
                }
                
                if (ev & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                    LOG_WARN("Connection error on fd `, removing", fd);
                    auto it = connections.find(fd);
                    if (it != connections.end()) {
                        LOG_INFO("Removing connection for `", it->second->symbol.c_str());
                        connections.erase(it);
                    }
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    continue;
                }
                
                if (ev & EPOLLIN) {
                    handle_socket_data(fd);
                }
            }
            
            // Send periodic pings
            time_t now = time(nullptr);
            if (now - last_ping > 120) { // Every 2 minutes
                send_ping_to_all();
                last_ping = now;
            }
        }
        
        LOG_INFO("All connections closed, exiting");
    }
    
    void shutdown() {
        eventfd_write(evfd, 1);
    }
};

void* multi_websocket_thread(void* arg) {
    std::vector<std::string> symbols = {
        "btcusdt", "ethusdt", "adausdt", "dotusdt", "linkusdt",
        "bnbusdt", "ltcusdt", "xrpusdt", "solusdt", "avaxusdt"
    };
    
    MultiWebSocketManager manager(symbols);
    if (manager.init() < 0) {
        LOG_ERROR("Failed to initialize WebSocket manager");
        return nullptr;
    }
    
    manager.run();
    return nullptr;
}

int main(int argc, char** argv) {
    if (photon::init(INIT_EVENT_IOURING, INIT_IO_NONE)) {
        LOG_ERROR_RETURN(0, -1, "Photon init failed");
    }
    DEFER(photon::fini());

    photon::thread_create(&multi_websocket_thread, nullptr);

    while (true) {
        photon::thread_usleep(1000 * 1000);
    }
    return 0;
}