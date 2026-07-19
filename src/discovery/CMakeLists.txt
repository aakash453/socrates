target_sources(socrates PRIVATE
    udp_discovery.cpp
    bluetooth_discovery.cpp
    discovery_service.cpp
)

if(APPLE OR (UNIX AND NOT ANDROID))
    target_sources(socrates PRIVATE mdns_discovery.cpp)
else()
    target_sources(socrates PRIVATE mdns_discovery_stub.cpp)
endif()

if(APPLE OR (UNIX AND NOT ANDROID))
    target_sources(socrates PRIVATE mdns_discovery.cpp)
endif()
