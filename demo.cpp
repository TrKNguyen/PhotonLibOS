// #include <photon/thread/thread.h>
// #include <photon/net/websocket/websocket.h>
// #include <photon/net/http/client.h>
// #include <photon/net/security-context/security-context.h>
// #include <photon/sync/mutex.h>
// #include <photon/sync/condition.h>
// #include <photon/common/alog.h>
// #include <vector>
// #include <queue>

// class BinanceExchange {
// public:
//     BinanceExchange(const std::string& api_key, 
//                    const std::string& api_secret)
//         : m_api_key(api_key),
//           m_api_secret(api_secret),
//           m_ssl_ctx(photon::net::new_ssl_ctx_client()),
//           m_rest_pool(photon::net::http::new_client()) {
//         configure_tls_1_3();
//         init_connections();
//     }

//     ~BinanceExchange() {
//         stop();
//     }

//     void start() {
//         // Start market data listeners
//         for (auto& client : m_market_ws) {
//             photon::thread_create([this, client] {
//                 market_data_listener(client);
//             });
//         }
        
//         // Start order ws listener
//         photon::thread_create([this] {
//             order_ws_listener(m_order_ws);
//         });
//     }

//     void stop() {
//         m_running = false;
//     }

//     // Market data interface
//     void subscribe_market(const std::string& symbol) {
//         std::string msg = R"({"method":"SUBSCRIBE","params":[")" 
//                         + symbol + R"(@depth"],"id":)" 
//                         + std::to_string(generate_nonce()) + "}";
//         // Round-robin across market connections
//         static std::atomic<uint32_t> counter{0};
//         auto idx = counter++ % m_market_ws.size();
//         async_ws_send(m_market_ws[idx], msg);
//     }

//     // Order management
//     void send_order(const std::string& order_msg) {
//         async_ws_send(m_order_ws, order_msg);
//     }

//     void cancel_order(const std::string& cancel_msg) {
//         async_ws_send(m_order_ws, cancel_msg);
//     }

//     void send_rest_order(const std::string& order_query) {
//         photon::thread_create([this, order_query] {
//             auto client = get_rest_client();
//             if (!client) return;

//             try {
//                 auto conn = client->connect("https://api.binance.com");
//                 photon::net::http::Request req;
//                 req.uri.path = "/api/v3/order";
//                 req.method = "POST";
//                 req.headers.insert({"X-MBX-APIKEY", m_api_key});
                
//                 std::string full_query = order_query + 
//                     "&timestamp=" + std::to_string(generate_nonce()) +
//                     "&signature=" + generate_signature(order_query);
//                 req.uri.query = full_query;
                
//                 auto resp = conn->call(req);
//                 handle_rest_response(resp->body().str());
//             } catch (...) {
//                 LOG_ERROR("REST order failed");
//             }
            
//             return_rest_client(client);
//         });
//     }

// private:
//     // Connection configurations
//     static constexpr size_t MARKET_WS_COUNT = 3;
//     static constexpr size_t MAX_REST_CONNS = 16;
    
//     // WebSocket connections
//     std::vector<photon::net::websocket::Client*> m_market_ws;
//     photon::net::websocket::Client* m_order_ws = nullptr;
    
//     // REST connection pool
//     std::queue<photon::net::http::Client*> m_rest_pool;
//     photon::sync::Mutex m_rest_mutex;
//     size_t m_rest_count = 0;
    
//     // Security
//     std::string m_api_key;
//     std::string m_api_secret;
//     photon::net::SecurityContext* m_ssl_ctx;
    
//     std::atomic<bool> m_running{true};
//     std::atomic<uint64_t> m_nonce{photon::now / 1000};

//     void configure_tls_1_3() {
//         m_ssl_ctx->set_min_version(TLS1_3_VERSION);
//         m_ssl_ctx->set_cipher_suites("TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256");
//     }

//     void init_connections() {
//         // Initialize market data WS connections
//         for (size_t i = 0; i < MARKET_WS_COUNT; ++i) {
//             auto client = photon::net::websocket::new_client(
//                 m_ssl_ctx, "wss://stream.binance.com:9443/ws");
//             client->connect();
//             m_market_ws.push_back(client);
//         }

//         // Initialize order WS connection
//         m_order_ws = photon::net::websocket::new_client(
//             m_ssl_ctx, "wss://stream.binance.com:9443/ws");
//         m_order_ws->connect();
//     }

//     void market_data_listener(photon::net::websocket::Client* client) {
//         while (m_running) {
//             photon::net::websocket::Message msg;
//             if (client->recv(msg) continue;
            
//             // Process market data update
//             photon::thread_create([msg = msg.str()] {
//                 LOG_DEBUG("Market update: ", msg);
//                 // Add your market data processing logic
//             });
//         }
//     }

//     void order_ws_listener(photon::net::websocket::Client* client) {
//         while (m_running) {
//             photon::net::websocket::Message msg;
//             if (client->recv(msg)) continue;
            
//             // Process order response
//             photon::thread_create([msg = msg.str()] {
//                 LOG_INFO("Order response: ", msg);
//                 // Add your order processing logic
//             });
//         }
//     }

//     photon::net::http::Client* get_rest_client() {
//         photon::sync::LockGuard lock(m_rest_mutex);
//         if (!m_rest_pool.empty()) {
//             auto client = m_rest_pool.front();
//             m_rest_pool.pop();
//             return client;
//         }
//         if (m_rest_count < MAX_REST_CONNS) {
//             m_rest_count++;
//             return photon::net::http::new_client();
//         }
//         return nullptr;
//     }

//     void return_rest_client(photon::net::http::Client* client) {
//         photon::sync::LockGuard lock(m_rest_mutex);
//         m_rest_pool.push(client);
//     }

//     void async_ws_send(photon::net::websocket::Client* client, 
//                       const std::string& msg) {
//         photon::thread_create([client, msg] {
//             client->send(msg, photon::net::websocket::TEXT_FRAME);
//         });
//     }

//     uint64_t generate_nonce() {
//         return m_nonce.fetch_add(1, std::memory_order_relaxed);
//     }

//     std::string generate_signature(const std::string& payload) {
//         // Implement HMAC-SHA256 signing
//         return "signature_placeholder";
//     }

//     void handle_rest_response(const std::string& response) {
//         photon::thread_create([response] {
//             LOG_INFO("REST response: ", response);
//             // Add response processing logic
//         });
//     }
// };

// // Demo usage
// int main() {
//     BinanceExchange client("api_key", "api_secret");
//     client.start();

//     // Subscribe to market data
//     client.subscribe_market("btcusdt");
//     client.subscribe_market("ethusdt");

//     // Send WS order
//     std::string order_msg = R"({"id":)" + std::to_string(time(nullptr)) 
//         + R"(,"method":"order.place","params":{"symbol":"BTCUSDT","side":"BUY"}})";
//     client.send_order(order_msg);

//     // Send REST order
//     std::string order_query = "symbol=BTCUSDT&side=BUY&type=LIMIT"
//         "&quantity=0.001&price=35000";
//     client.send_rest_order(order_query);

//     // Keep main thread alive
//     while (true) photon::thread_sleep(3600);
// }






// // // heartbeat ////////////////////////////////////////////////////////////////////////////////////////
// // class BinanceExchange {
// //     // ... [previous members] ...
// //     photon::sync::Mutex m_ws_send_mutex;
// //     std::atomic<uint64_t> m_last_pong{0};

// //     void init_connections() {
// //         // ... existing init code ...
        
// //         // Initialize order WS with heartbeat
// //         m_order_ws = photon::net::websocket::new_client(
// //             m_ssl_ctx, "wss://stream.binance.com:9443/ws");
// //         m_order_ws->connect();
// //         start_heartbeat();
// //     }

// //     void start_heartbeat() {
// //         photon::thread_create([this] {
// //             heartbeat_loop();
// //         }, photon::STACK_SIZE_DEFAULT, nullptr, photon::INHERIT_THREAD_FLAGS);
// //     }

// //     void heartbeat_loop() {
// //         while (m_running) {
// //             {
// //                 photon::sync::LockGuard lock(m_ws_send_mutex);
// //                 if (m_order_ws && m_order_ws->get_connection()->is_connected()) {
// //                     // Send Binance-specific ping message
// //                     const char ping_msg[] = R"({"method":"PING"})";
// //                     m_order_ws->send(ping_msg, photon::net::websocket::TEXT_FRAME);
// //                 }
// //             }
            
// //             // Sleep 30 seconds with precise timing
// //             photon::thread_sleep(30 * 1000000UL); // 30 seconds in microseconds
// //         }
// //     }

// //     void order_ws_listener(photon::net::websocket::Client* client) {
// //         while (m_running) {
// //             photon::net::websocket::Message msg;
// //             if (client->recv(msg)) {
// //                 LOG_ERROR("Order WS disconnected");
// //                 reconnect_order_ws();
// //                 continue;
// //             }
            
// //             const std::string& data = msg.str();
// //             if (data.find(R"("result":null)") != std::string::npos) {
// //                 // Handle pong response
// //                 m_last_pong.store(photon::now, std::memory_order_relaxed);
// //                 LOG_DEBUG("Received pong");
// //             } else {
// //                 process_order_response(data);
// //             }
// //         }
// //     }

// //     void reconnect_order_ws() {
// //         photon::sync::LockGuard lock(m_ws_send_mutex);
// //         if (m_order_ws) {
// //             m_order_ws->close();
// //             delete m_order_ws;
// //         }
// //         m_order_ws = photon::net::websocket::new_client(
// //             m_ssl_ctx, "wss://stream.binance.com:9443/ws");
// //         m_order_ws->connect();
// //     }

// //     void check_heartbeat_health() {
// //         const uint64_t now = photon::now;
// //         const uint64_t last_pong = m_last_pong.load(std::memory_order_relaxed);
        
// //         if (now - last_pong > 35 * 1000000UL) { // 35 seconds timeout
// //             LOG_WARN("No pong received for ", (now - last_pong)/1000000, " seconds");
// //             reconnect_order_ws();
// //         }
// //     }
// // };

// // /////////////////////// ping ////////////////////////////////////////////
// // class TradingWSClient {
// //     photon::net::websocket::Client* ws;
// //     std::atomic<uint64_t> last_pong{0};
    
// //     void ping_loop() {
// //         while (true) {
// //             // Send RFC6455 ping frame every 30s
// //             const char ping[] = "\x89\x00"; // Opcode 0x9 + empty payload
// //             ws->send(ping, sizeof(ping)-1, 
// //                     photon::net::websocket::CONTROL_FRAME);
            
// //             photon::thread_sleep(30 * 1000000UL);
// //         }
// //     }

// //     void message_loop() {
// //         photon::net::websocket::Message msg;
// //         while (ws->recv(msg) == 0) {
// //             if (msg.header.opcode == 0xA) { // Pong frame
// //                 last_pong.store(photon::now);
// //             } else {
// //                 process_message(msg);
// //             }
// //         }
// //     }

// //     void check_connection() {
// //         if (photon::now - last_pong > 35 * 1000000UL) {
// //             reconnect();
// //         }
// //     }
// // };

// // // For OKX exchange
// // void send_okx_ping() {
// //     const char okx_ping[] = R"({"op":"ping"})";
// //     ws->send(okx_ping, sizeof(okx_ping)-1, 
// //             photon::net::websocket::TEXT_FRAME);
// // }