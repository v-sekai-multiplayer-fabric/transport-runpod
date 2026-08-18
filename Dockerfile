# The RunPod Serverless workers, and nothing else in the image.
#
# No GPU, no weights, no Python. Two binaries come out of it: `rp-worker-echo`, which answers
# jobs itself and exists to test the queue protocol against a real endpoint with nothing else
# in the picture to blame, and `rp-worker-bus`, which answers them by asking an interactor in
# another process. A service image copies the second one out of this image rather than
# rebuilding it, so there is one copy of the job loop.
FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake libcurl4-openssl-dev ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY proof ./proof
COPY third_party ./third_party
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j --target rp-worker-echo rp-worker-bus rp-bus-roundtrip
# The proof runs in the build, so an image is never produced from a tree whose correlation is
# broken. It needs no bus and no network, which is what makes it runnable here at all.
RUN ./build/rp-bus-roundtrip

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
# libcurl for the wire, ca-certificates because every RunPod webhook is https and a worker with
# no roots fails on the first job-take with nothing in the log that says why.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libcurl4 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/rp-worker-echo /usr/local/bin/rp-worker-echo
COPY --from=build /src/build/rp-worker-bus /usr/local/bin/rp-worker-bus

# The echo worker, because this image on its own has no interactor to reach. A service image
# overrides this with `rp-worker-bus` beside the interactor it belongs to.
CMD ["/usr/local/bin/rp-worker-echo"]
