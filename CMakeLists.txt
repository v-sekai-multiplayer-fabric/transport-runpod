cmake_minimum_required(VERSION 3.22)
project(transport-runpod CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)

# The transport, as a library: `rp_worker_run(weft_interactor_t)`. An interactor repo vendors
# this repo as a subtree and links this target rather than forking the job loop.
add_library(rp-worker STATIC src/worker.cpp third_party/interactor/src/cbor.c)
target_link_libraries(rp-worker PUBLIC CURL::libcurl)
target_include_directories(rp-worker PUBLIC include third_party/interactor/include)

# The standalone echo binary this repo proves the queue protocol with.
add_executable(rp-worker-echo src/main.cpp)
target_link_libraries(rp-worker-echo PRIVATE rp-worker)

# The bus-backed interactor: a worker whose interactor is another process on the same machine.
# The harness is the only thing that knows what iceoryx2 is, and it dlopens it, so nothing
# here links it -- see third_party/harness/README.md. Its own proof programs are not built,
# because this repository is not the place to compile somebody else's test.
set(WEFT_HARNESS_PROOF OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/harness)

add_library(rp-bus STATIC src/bus_ask.cpp src/port_iox2.cpp)
target_link_libraries(rp-bus PUBLIC rp-worker weft::harness)

add_executable(rp-worker-bus src/main_bus.cpp)
target_link_libraries(rp-worker-bus PRIVATE rp-bus)

# The correlation, the deadline and the malformed shapes, against a table of canned messages.
# It needs no bus, no interactor and no network, so it runs in CI and on a laptop.
enable_testing()
add_executable(rp-bus-roundtrip proof/roundtrip.cpp)
target_link_libraries(rp-bus-roundtrip PRIVATE rp-bus)
target_compile_options(rp-bus-roundtrip PRIVATE -Wall -Wextra)
add_test(NAME roundtrip COMMAND rp-bus-roundtrip)
