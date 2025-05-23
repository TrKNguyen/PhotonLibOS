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

#include <photon/common/alog.h>
#include <photon/io/fd-events.h>
#include <photon/thread/thread.h>
#include <photon/common/timeout.h>
#include <photon/net/socket.h>
#include <photon/net/security-context/tls-stream.h>

using namespace photon;

int main(int argc, char** argv) {
    if (photon::init(photon::INIT_EVENT_IOURING, photon::INIT_IO_NONE))
        return -1;
    DEFER(photon::fini());

    auto ctx = net::new_tls_context(nullptr, nullptr, "Just4Test");
    if (!ctx) return -1;
    DEFER(delete ctx);
    auto cli = net::new_tls_client(ctx, net::new_iouring_tcp_client(), true);
    DEFER(delete cli);
    char buff[4096];
    for (int i = 0; i < 4096; i++) buff[i] = 't';
    auto tls = cli->connect(net::EndPoint{net::IPAddr("18.178.153.80"), 9443});
    if (!tls) {
        LOG_ERRNO_RETURN(0, -1, "failed to connect");
    }
    DEFER(delete tls);
    int timeout_sec = 30;
    Timeout tmo(timeout_sec * 1000 * 1000);
    uint64_t cnt = 0;
    LOG_INFO(tmo.timeout());
    while (photon::now < tmo.expiration()) {
        auto ret = tls->send(buff, 4096);
        if (ret < 0) LOG_ERROR_RETURN(0, -1, "Failed to send");
        cnt += ret;
    }
    LOG_INFO("Send ` in ` seconds, ` MB/s", cnt, timeout_sec, cnt / 1024 / 1024 / timeout_sec);
}