#include <photon/photon.h>
#include <photon/thread/std-compat.h>
#include <photon/io/iouring-wrapper.h>
#include <photon/common/alog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <iostream>
#include <cstdint>

int main() {
    // Initialize Photon environment
    int ret = photon::init(photon::INIT_EVENT_IOURING, photon::INIT_IO_NONE);
    if (ret != 0) {
        std::cerr << "Photon initialization failed" << std::endl;
        return -1;
    }
    DEFER(photon::fini());

    // Launch client in a Photon thread
    photon_std::thread client_thread([] {
        // Create TCP socket
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            LOG_ERRNO_RETURN(0, , "failed to create socket");
        }
        DEFER({ photon::iouring_close(fd); });
        std::cout << "Created TCP Socket\n\n\n";
        std::cout.flush();
        // Configure server address
        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(8080);
        inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
        std::cout << "Configured server address\n\n\n";
        // Debug io_uring state
        std::cout << "Master event engine: " << (void*)photon::get_vcpu()->master_event_engine << std::endl;
        std::cout << "Attempting io_uring connect (fd: " << fd << ")" << std::endl;

        // Connect using io_uring with 1-second timeout
        uint64_t timeout = 100ULL; // 1 second in nanoseconds
        char ip_str[INET_ADDRSTRLEN];
        const char* result = inet_ntop(AF_INET, &(server_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
        // std::cout << fd <<" "<< ip_str  << "\n";
        int conn_ret = photon::iouring_connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr), timeout);
        // std::cout << "1\n";
        if (conn_ret < 0) {
            LOG_ERRNO_RETURN(0, , "failed to connect to server");
        }
        std::cout << "Connected using io_uring with 1-second timeout";
        // Send HTTP GET request
        std::string get_request = "GET / HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n\r\n";
        ssize_t sent = photon::iouring_send(fd, get_request.c_str(), get_request.size(), 0, timeout);
        if (sent < 0) {
            LOG_ERROR("Failed to send GET request");
        } else if (sent != (ssize_t)get_request.size()) {
            LOG_ERROR("Incomplete GET send");
        } else {
            std::cout << "GET request sent successfully" << std::endl;
        }
        // Receive GET response
        char buf[1024];
        std::string full_get_response;
        while (true) {
            std::cout <<"0\n";
            ssize_t received = photon::iouring_recv(fd, buf, sizeof(buf), 0, timeout);
            std::cout <<"1\n";
            if (received > 0) {
                full_get_response += std::string(buf, received);
                std::cout << full_get_response<< "\n";
            } else if (received == 0) {
                // EOF
                break;
            } else {
                LOG_ERROR("Failed to receive GET response");
                break;
            }
        }
        std::cout << "GET response: '" << full_get_response << "'" << std::endl;
        std::cout << "End of GET request\n";


        // Send HTTP POST request
        std::string post_data = "Hello World";
        std::string post_request =
            "POST / HTTP/1.1\r\n"
            "Host: 127.0.0.1:8080\r\n"
            "Content-Length: " + std::to_string(post_data.size()) + "\r\n"
            "\r\n" + post_data;
        sent = photon::iouring_send(fd, post_request.c_str(), post_request.size(), 0, timeout);
        if (sent < 0) {
            LOG_ERROR("Failed to send POST request");
        } else if (sent != (ssize_t)post_request.size()) {
            LOG_ERROR("Incomplete POST send");
        } else {
            std::cout << "POST request sent successfully" << std::endl;
        }

        // Receive POST response
        std::string full_post_response;
        while (true) {
            ssize_t received = photon::iouring_recv(fd, buf, sizeof(buf), 0, timeout);
            if (received > 0) {
                full_post_response += std::string(buf, received);
            } else if (received == 0) {
                // EOF
                break;
            } else {
                LOG_ERROR("Failed to receive POST response");
                break;
            }
        }
        std::cout << "POST response: '" << full_post_response << "'" << std::endl;
        std::cout << "End of POST request\n";
    });

    // Wait for client thread to finish
    client_thread.join();
    return 0;
}