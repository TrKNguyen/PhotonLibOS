#include <photon/photon.h>
#include <photon/thread/std-compat.h>
#include <iostream>

int func(int a, char* b) {}

int main() {
    int ret = photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE); 
    if (ret != 0) {
        return -1;
    }
    DEFER(photon::fini()); 
    std::cout <<" checkkkkkkkkkkk\n";
}
