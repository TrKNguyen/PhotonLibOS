#include <photon/photon.h> 
#include <photon/thread/std-compat.h> 
#include <photon/net/socket.h> 
#include <photon/common/utility.h> 
#include <photon/fs/localfs.h>
#include <photon/common/alog.h>
#include <iostream>
int main() {
    int ret = photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE);
    if (ret != 0) {
        return -1; 
    }
    DEFER(photon::fini());
    photon_std::thread server_thread([]{
        auto server = photon::net::new_tcp_socket_server(); 
        if (server == nullptr) {
            LOG_ERRNO_RETURN(0, -1, "failed to create tcp server");
        }
        DEFER(delete server);
        auto handler = [&](photon::net::ISocketStream* stream) -> int {
            char buf[1024]; 
            ssize_t ret = stream->recv(buf, 1024);
            if (ret <= 0) {
                LOG_ERRNO_RETURN(0, -1, "failed to read socket");
            } else {
                std::cout <<"Receievd message\n"; 
            }
            return 0;
        }; 
        server-> set_handler(handler); 
        server->bind_v4localhost(9527); 
        server->listen();
        LOG_INFO("Server is listening for port ' ...", 9527); 
        server->start_loop(true);
    });
    server_thread.join();
    return 0;
}