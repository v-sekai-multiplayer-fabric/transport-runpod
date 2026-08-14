# A RunPod Serverless worker in C++, and nothing else in the image.
#
# No GPU, no weights, no Python. This image exists to answer one question -- does
# a C++ worker speak RunPod's queue protocol -- and an image carrying anything
# else would let a failure be blamed on the anything else.
FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake libcurl4-openssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY third_party ./third_party
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j --target rp-worker-echo

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
# libcurl for the wire, ca-certificates because every RunPod webhook is https and
# a worker with no roots fails on the first job-take with nothing in the log that
# says why.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libcurl4 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/rp-worker-echo /usr/local/bin/rp-worker-echo

# Unbuffered stderr: RunPod collects worker logs, and a buffered worker that dies
# mid-job takes its explanation with it.
CMD ["/usr/local/bin/rp-worker-echo"]
