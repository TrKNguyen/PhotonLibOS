/*
Copyright 2022 The Photon Authors, modified for WSS client 2025

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

#include <chrono>
#include <cstring>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <photon/photon.h>
#include <photon/io/signal.h>
#include <photon/thread/thread11.h>
#include <photon/common/alog.h>
#include <photon/net/socket.h>
#include <unistd.h>
#include <fcntl.h>

static const char* SERVER_IP = "18.177.127.58"; // stream.binance.com
static const uint16_t SERVER_PORT = 9443;
static const char* SERVER_HOST = "stream.binance.com";
static const size_t CONNECTION_NUM = 8;
static const size_t BUF_SIZE = 512;
static const uint64_t STATS_INTERVAL = 1; // Seconds

static bool stop_test = false;
static uint64_t qps = 0;
static uint64_t time_cost = 0;

// Custom TLS stream wrapper for io_uring
class TLSSocketStream : public photon::net::ISocketStream {
public:
    photon::net::ISocketStream* underlay;
    SSL* ssl;
    bool ownership;

    TLSSocketStream(SSL* ssl_, photon::net::ISocketStream* stream, bool own = false)
        : underlay(stream), ssl(ssl_), ownership(own) {
        int fd = get_fd();
        if (fd >= 0) {
            SSL_set_fd(ssl, fd);
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        } else {
            LOG_ERROR("Failed to get socket fd for SSL; TLS may fail");
        }
    }

    ~TLSSocketStream() {
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        if (ownership && underlay) {
            delete underlay;
        }
    }

    int get_fd() {
        // TODO: Replace with actual fd extraction from Photon's io_uring client
        // Check ISocketStream or io_uring client for get_fd() or similar
        // Example (hypothetical): return underlay->get_fd();
        // Fallback: Inspect Photon source or use getsockopt
        return -1; // Must be implemented for SSL_set_fd to work
    }

    ssize_t recv(void* buf, size_t cnt, int flags = 0) override {
        ssize_t ret = SSL_read(ssl, buf, cnt);
        if (ret <= 0) {
            int ssl_err = SSL_get_error(ssl, ret);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                errno = EAGAIN;
            } else {
                char err_buf[256];
                ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
                LOG_ERROR("SSL read error: `", err_buf);
                errno = EIO;
            }
        }
        return ret;
    }

    ssize_t send(const void* buf, size_t cnt, int flags = 0) override {
        ssize_t ret = SSL_write(ssl, buf, cnt);
        if (ret <= 0) {
            int ssl_err = SSL_get_error(ssl, ret);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                errno = EAGAIN;
            } else {
                char err_buf[256];
                ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
                LOG_ERROR("SSL write error: `", err_buf);
                errno = EIO;
            }
        }
        return ret;
    }

    ssize_t write(const void* buf, size_t cnt) override {
        size_t written = 0;
        while (written < cnt && !stop_test) {
            ssize_t ret = send((char*)buf + written, cnt - written);
            if (ret > 0) {
                written += ret;
            } else if (errno == EAGAIN) {
                photon::thread_yield();
            } else {
                return -1;
            }
        }
        return written;
    }

    ssize_t read(void* buf, size_t cnt) override {
        size_t read = 0;
        while (read < cnt && !stop_test) {
            ssize_t ret = recv((char*)buf + read, cnt - read);
            if (ret > 0) {
                read += ret;
            } else if (errno == EAGAIN) {
                photon::thread_yield();
            } else {
                return -1;
            }
        }
        return read;
    }

    int close() override {
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (ownership && underlay) {
            int ret = underlay->close();
            delete underlay;
            underlay = nullptr;
            return ret;
        }
        return 0;
    }

    int shutdown(photon::net::ShutdownHow how) override {
        if (ssl) {
            SSL_shutdown(ssl);
        }
        return underlay ? underlay->shutdown(how) : 0;
    }

    // Implement pure virtual functions from ISocketBase
    photon::Object* get_underlay_object(uint64_t recursion = 0) override {
        return underlay ? underlay->get_underlay_object(recursion) : nullptr;
    }

    int setsockopt(int level, int option_name, const void* option_value, socklen_t option_len) override {
        return underlay ? underlay->setsockopt(level, option_name, option_value, option_len) : -1;
    }

    int getsockopt(int level, int option_name, void* option_value, socklen_t* option_len) override {
        return underlay ? underlay->getsockopt(level, option_name, option_value, option_len) : -1;
    }

    // Implement pure virtual functions from ISocketName
    int getsockname(photon::net::EndPoint& addr) override {
        return underlay ? underlay->getsockname(addr) : -1;
    }

    int getpeername(photon::net::EndPoint& addr) override {
        return underlay ? underlay->getpeername(addr) : -1;
    }

    int getsockname(char* path, size_t count) override {
        return underlay ? underlay->getsockname(path, count) : -1;
    }

    int getpeername(char* path, size_t count) override {
        return underlay ? underlay->getpeername(path, count) : -1;
    }

    // Stub implementations for other ISocketStream methods
    ssize_t recv(const struct iovec* iov, int iovcnt, int flags = 0) override {
        return read(iov[0].iov_base, iov[0].iov_len);
    }

    ssize_t send(const struct iovec* iov, int iovcnt, int flags = 0) override {
        return write(iov[0].iov_base, iov[0].iov_len);
    }

    ssize_t readv(const struct iovec* iov, int iovcnt) override {
        return read(iov[0].iov_base, iov[0].iov_len);
    }

    ssize_t writev(const struct iovec* iov, int iovcnt) override {
        return write(iov[0].iov_base, iov[0].iov_len);
    }

    ssize_t sendfile(int fd, off_t offset, size_t count) override {
        return -1; // Not supported
    }
};

static void handle_signal(int sig) {
    LOG_INFO("Gracefully stopping WSS client...");
    stop_test = true;
}

static void run_latency_loop() {
    while (!stop_test) {
        photon::thread_sleep(STATS_INTERVAL);
        uint64_t lat = (qps != 0) ? (time_cost / qps) : 0;
        LOG_INFO("Average latency: ` us", lat);
        qps = time_cost = 0;
    }
}

// Send WebSocket text frame (opcode 0x1)
static int send_ws_text(TLSSocketStream* stream, const char* data, size_t len) {
    char header[2] = { (char)0x81, (char)len }; // FIN=1, opcode=0x1, len < 126
    if (len > 125) {
        LOG_ERROR("Message too large for simple frame");
        return -1;
    }
    char frame[BUF_SIZE];
    memcpy(frame, header, 2);
    memcpy(frame + 2, data, len);
    ssize_t ret = stream->write(frame, 2 + len);
    if (ret != (ssize_t)(2 + len)) {
        LOG_ERROR("Failed to send WebSocket text frame");
        return -1;
    }
    return 0;
}

// Parse WebSocket frame, return 0 for text, 1 for ping, -1 for error
static int parse_ws_frame(const char* buf, size_t len, char* payload, size_t* payload_len) {
    if (len < 2) return -1;
    uint8_t opcode = buf[0] & 0x0F;
    uint8_t fin = buf[0] & 0x80;
    uint8_t payload_len_byte = buf[1] & 0x7F;
    if (!fin || payload_len_byte > len - 2) {
        LOG_ERROR("Unsupported WebSocket frame");
        return -1;
    }
    if (opcode == 0x1) { // Text
        memcpy(payload, buf + 2, payload_len_byte);
        payload[payload_len_byte] = '\0';
        *payload_len = payload_len_byte;
        return 0;
    } else if (opcode == 0x9) { // Ping
        memcpy(payload, buf + 2, payload_len_byte);
        *payload_len = payload_len_byte;
        return 1;
    }
    return -1;
}

// Extract price from aggTrade JSON (e.g., {"e":"aggTrade","p":"123.45",...})
static const char* extract_price(const char* json) {
    const char* price_key = "\"p\":\"";
    const char* start = strstr(json, price_key);
    if (!start) return nullptr;
    start += strlen(price_key);
    const char* end = strchr(start, '"');
    if (!end) return nullptr;
    static char price[32];
    size_t len = end - start;
    if (len >= sizeof(price)) return nullptr;
    strncpy(price, start, len);
    price[len] = '\0';
    return price;
}

static int wss_client() {
    photon::net::EndPoint ep{photon::net::IPAddr(SERVER_IP), SERVER_PORT};
    auto cli = photon::net::new_iouring_tcp_client();
    if (cli == nullptr) {
        LOG_ERROR("Failed to create io_uring client");
        return -1;
    }
    DEFER(delete cli);

    // Initialize OpenSSL
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        LOG_ERROR("Failed to create SSL context");
        return -1;
    }
    DEFER(SSL_CTX_free(ctx));

    // Configure TLS
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_cipher_list(ctx, "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384");
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
        LOG_ERROR("Failed to load default CA paths");
        return -1;
    }

    auto run_wss_connection = [&]() -> int {
        char buf[BUF_SIZE];
        char payload[BUF_SIZE];
        size_t payload_len;

        // Connect
        auto conn = cli->connect(ep);
        if (conn == nullptr) {
            LOG_ERROR("Failed to connect to ", ep);
            return -1;
        }
        DEFER(if (conn) delete conn);

        // Setup TLS
        SSL* ssl = SSL_new(ctx);
        if (!ssl) {
            LOG_ERROR("Failed to create SSL object");
            delete conn;
            conn = nullptr;
            return -1;
        }
        DEFER(SSL_free(ssl));

        // Set SNI
        if (SSL_set_tlsext_host_name(ssl, SERVER_HOST) != 1) {
            LOG_ERROR("Failed to set SNI");
            delete conn;
            conn = nullptr;
            return -1;
        }

        // Create TLS stream (ownership of conn is transferred)
        auto* tls_stream = new TLSSocketStream(ssl, conn, true);
        conn = nullptr; // Ownership transferred
        DEFER(delete tls_stream);

        // Perform TLS handshake with non-blocking handling
        while (!stop_test) {
            int ret = SSL_connect(ssl);
            if (ret == 1) {
                break; // Handshake successful
            }
            int ssl_err = SSL_get_error(ssl, ret);
            if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) {
                photon::thread_yield();
            } else {
                char err_buf[256];
                ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
                LOG_ERROR("SSL handshake failed: `", err_buf);
                return -1;
            }
        }

        // Verify server certificate
        if (SSL_get_verify_result(ssl) != X509_V_OK) {
            LOG_ERROR("Server certificate verification failed");
            return -1;
        }

        // WebSocket handshake
        const char* ws_key = "dGhlIHNhbXBsZSBub25jZQ==";
        const char* handshake = "GET /ws/btcusdt@aggTrade HTTP/1.1\r\n"
                                "Host: stream.binance.com\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                "Sec-WebSocket-Version: 13\r\n\r\n";
        ssize_t ret = tls_stream->write(handshake, strlen(handshake));
        if (ret != (ssize_t)strlen(handshake)) {
            LOG_ERROR("Failed to send WSS handshake");
            return -1;
        }

        ret = tls_stream->read(buf, BUF_END);
        if (ret <= 0 || strstr(buf, "HTTP/1.1 101") == nullptr) {
            LOG_ERROR("WSS handshake failed");
            return -1;
        }

        // Subscribe to btcusdt@aggTrade
        const char* subscribe = "{\"method\":\"SUBSCRIBE\",\"params\":[\"btcusdt@aggTrade\"],\"id\":1}";
        if (send_ws_text(tls_stream, subscribe, strlen(subscribe)) != 0) {
            LOG_ERROR("Failed to send subscription");
            return -1;
        }

        // Main loop: Handle market data and ping/pong
        while (!stop_test) {
            auto start = std::chrono::system_clock::now();
            ret = tls_stream->read(buf, BUF_SIZE);
            if (ret <= 0) {
                LOG_ERROR("Receive failed");
                return -1;
            }

            int frame_type = parse_ws_frame(buf, ret, payload, &payload_len);
            if (frame_type == 1) { // Ping
                char pong_header[2] = { (char)0x8A, (char)payload_len };
                char pong_frame[BUF_SIZE];
                memcpy(pong_frame, pong_header, 2);
                memcpy(pong_frame + 2, payload, payload_len);
                ret = tls_stream->write(pong_frame, 2 + payload_len);
                if (ret != (ssize_t)(2 + payload_len)) {
                    LOG_ERROR("Failed to send pong");
                    return -1;
                }
            } else if (frame_type == 0) { // Text
                if (strstr(payload, "\"result\":null") && strstr(payload, "\"id\":1")) {
                    LOG_INFO("Subscribed successfully");
                } else if (strstr(payload, "\"e\":\"aggTrade\"")) {
                    const char* price = extract_price(payload);
                    if (price) {
                        LOG_INFO("Price: `", price);
                    }
                }
            }

            auto end = std::chrono::system_clock::now();
            time_cost += std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            qps++;
        }

        return 0;
    };

    // Start latency monitoring
    photon::thread_create11(run_latency_loop);

    // Create coroutines for each connection
    for (size_t i = 0; i < CONNECTION_NUM; i++) {
        photon::thread_create11(run_wss_connection);
    }

    // Sleep until Ctrl+C
    while (!stop_test) {
        photon::thread_sleep(1'000'000); // 1 second
    }
    return 0;
}

int main() {
    set_log_output_level(ALOG_INFO);
    int ret = photon::init(photon::INIT_EVENT_IOURING, photon::INIT_IO_NONE);
    if (ret < 0) {
        LOG_ERROR("Failed to init photon environment");
        return -1;
    }
    DEFER(photon::fini());

    photon::sync_signal(SIGTERM, &handle_signal);
    photon::sync_signal(SIGINT, &handle_signal);

    return wss_client();
}