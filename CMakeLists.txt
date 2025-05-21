cmake_minimum_required(VERSION 3.14)
project(my_project)

# Photon settings
set(PHOTON_ENABLE_URING ON CACHE INTERNAL "Enable io_uring")
set(PHOTON_CXX_STANDARD 14 CACHE INTERNAL "C++ standard")

# Fetch Photon source
include(FetchContent)
FetchContent_Declare(
    photon
    GIT_REPOSITORY https://github.com/alibaba/PhotonLibOS.git
    GIT_TAG main
)
FetchContent_MakeAvailable(photon)

# Your app
#add_executable(client client.cpp)
#target_link_libraries(client photon_static)

#add_executable(demo demo.cpp)
#target_link_libraries(demo photon_static)

#add_executable(clientWSS clientWSS.cpp)
#target_link_libraries(clientWSS photon_static)

#add_executable(server server.cpp)
#target_link_libraries(server photon_static)

add_executable(main_tls main_tls.cpp)
target_link_libraries(main_tls photon_static)

add_executable(client_tls client_tls.cpp)
target_link_libraries(client_tls photon_static)