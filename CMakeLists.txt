cmake_minimum_required(VERSION 3.20)
project(transport-runpod CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)

# The transport, as a library: `rp_worker_run(weft_interactor_t)`. An interactor
# repo (interactor-qwen35-defiant, interactor-gemma4-composer, ...) vendors this
# repo as a subtree and links this target rather than forking the job loop.
add_library(rp-worker STATIC src/worker.cpp third_party/interactor/src/cbor.c)
target_link_libraries(rp-worker PUBLIC CURL::libcurl)
target_include_directories(rp-worker PUBLIC include third_party/interactor/include)

# The standalone echo binary this repo proves itself with.
add_executable(rp-worker-echo src/main.cpp)
target_link_libraries(rp-worker-echo PRIVATE rp-worker)
