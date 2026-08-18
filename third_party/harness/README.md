# fabric-harness

The weft plane harness. Every plane and every edge links this, and none of them links
iceoryx2.

Split out of [`fabric-weft-plane`](https://github.com/v-sekai-multiplayer-fabric/fabric-weft-plane) with its
history. weft keeps only the data plane and the NIF the BEAM loads. A plane is its own
process, its own repository, and its own container.


Goal: one runtime model for every plane. A thin C++ thread-per-core loop over iceoryx2.

State: a first loop exists, compiled against the real ABI, not yet run.

- **Built.** `weft::harness`, a library every plane and edge links. It holds the bus, the
  limits, and the payload type.
- **Proved.** `proof/` passes a `Snapshot` between two processes with no copy and no
  daemon. The run is below.
- **Built and proved on Windows, not yet on Linux.** `weft/loop.hpp`'s `run_command_loop` —
  a command in, reply bytes out, over a new payload variant (`iox2_type_variant_e_DYNAMIC`,
  a byte slice, not `Snapshot`'s fixed struct) and `weft/command.hpp`'s request-id-prefixed
  envelope. `proof/command_publisher.cpp`/`command_subscriber.cpp` ran for real on Windows
  11 against iceoryx2 v0.9.3 built from source — both exit 0, 8/8 round trips, through the
  actual dlopen stub table this repo generates, not a proxy for it. See "The command/reply
  proof" below for the two real bugs (one iceoryx2's, one MinGW's) that had to be worked
  around to get there, and for what's still not wired into `CMakeLists.txt` as a result.
  This repo's primary target is Linux and that run hasn't happened yet.
- One thread, not thread-per-core: the goal above names the eventual shape, and a
  single-process, likely-GPU-bound interactor (one plane, this loop's first intended
  caller) has no per-core work to split. `run_command_loop`, not `run_loop`, so it doesn't
  claim to be that eventual answer.

## One harness, not one for each plane

weft has several planes and two edges, and the number grows. Each one needs the bus and
each one needs the limits.

Left alone, each would grow its own copy of both, and the copies would drift the way a
decision written twice always drifts. That is the failure `Weft.VocabularyTest` was
written for, in a different form.

So there is one. A plane repository brings this in with `git subtree add --prefix=thirdparty/harness`,
and links `weft::harness`.

| what it gives | where | why it is shared |
| --- | --- | --- |
| the bus | `iceoryx2.sigs`, and the table generated from it | one C ABI, one dispatch table |
| the limits | `include/weft/limits.hpp` | every value is `Weft.Limits`, which is rivet's |
| the payloads | `include/weft/snapshot.hpp`, `include/weft/store.hpp` | both ends of a service must agree exactly |

`Weft.PlaneNetworkingTest` holds that shape. It fails if a second `.sigs` file appears, if
a plane declares a limit of its own, or if a directory with a `CMakeLists.txt` is missing
from the root build.

## Nothing links iceoryx2

`iceoryx2.sigs` lists the 40 C ABI functions the harness calls. Chromium's
`generate_stubs.py`, vendored at `thirdparty/generate_stubs`, turns that list
into a dlsym dispatch table. The pattern comes from `fabric-godot-core`, which uses it for
GStreamer.

So the harness builds on a machine that has never seen iceoryx2, and it fails at start
rather than at link when the library is absent. `ldd` on either binary lists no iceoryx2.

That matters more here than it did for GStreamer. iceoryx2 is Rust, and weft writes no
Rust. A dlopen keeps the Rust artifact out of weft's build graph as well as out of its
source.

Three generated pieces come from that one file, and each has a reason.

- `iceoryx2_stubs.cc`, the dispatch table.
- `iox2_api.h`, the prototypes. `generate_stubs.py` emits none, because Chromium's callers
  include the real library headers. A prototype that disagrees with the table it calls
  through is a crash with no diagnostic, so both come from the same file.
- `src/iox2_decls.h` is hand-written, and it is the one piece that is not generated. It
  declares the opaque types. A handle is a pointer, so an incomplete struct is exact. A
  storage struct, `iox2_..._t`, is a real sized struct, and transcribing it would be a
  silent memory bug the day upstream adds a field. It stays incomplete, and the harness
  passes NULL for every one. iceoryx2 then allocates on the heap, which is the documented
  contract.

## What the proof is

Two programs and one struct.

- `src/snapshot.hpp` holds `weft::Snapshot`: a tick, an entity, and three positions in
  micrometres. This is the fixed point the data plane already uses.
- `src/publisher.cpp` loans a sample, writes the struct, and sends the loan. It does not
  serialize and it does not copy. `loan_uninit` returns memory the subscriber already
  maps, so a send is a pointer handoff.
- `src/subscriber.cpp` receives and checks. It checks the tick order and the payload it
  derives from the tick.

The check is the point. A bus that delivers garbage must fail here, and not print a count.

## How to run it

The build needs no iceoryx2. The run does. Build and install iceoryx2 v0.9.3 somewhere the
loader can find at run time. The prefix below matches the one `.gitignore` already
excludes.

    cmake -S <iceoryx2-src> -B <iceoryx2-src>/build \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/.iceoryx
    cmake --build <iceoryx2-src>/build -j
    cmake --install <iceoryx2-src>/build

Then build the proof and run both ends. Start the subscriber first.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j

    export LD_LIBRARY_PATH=$PWD/.iceoryx/lib64   # or set WEFT_ICEORYX2_PATH
    ./build/weft-harness-subscriber 8 &
    ./build/weft-harness-publisher 8

## The run

One machine, 16 cores, Fedora, GCC 16.1.1, iceoryx2 v0.9.3, Release.

    publisher: sent 8
    subscriber: tick 1 entity 42 x 1000 z -250
    ...
    subscriber: tick 8 entity 42 x 8000 z -2000
    subscriber: received 8, in order, intact

The subscriber exits 0. No daemon runs, and none is started.

## The command/reply proof (`run_command_loop`) -- run, on Windows

`weft-harness-command_subscriber` runs `weft::run_command_loop` itself (server, echo
`ask`), `weft-harness-command_publisher` sends "ping N" and checks "pong N" comes back on
the reply topic, correlated by request id.

    ./build/weft-harness-command_subscriber 8 &
    ./build/weft-harness-command_publisher 8

**Run for real on Windows 11 Pro (10.0.26200), llvm-mingw g++/clang 22.1.8, iceoryx2 v0.9.3
built from source (`cargo build --release -p iceoryx2-ffi-c`, no prebuilt Windows release
exists).** Two things had to be fixed first, neither in this repository's own code:

1. **iceoryx2 v0.9.3 hardcodes `C:\Temp\` on Windows** and fails with no clear error when
   it doesn't exist on a stock install -- exactly [issue #1868](
   https://github.com/eclipse-iceoryx/iceoryx2/issues/1868), closed 2026-08-04 (after
   v0.9.3). Fix: `mkdir -p /c/Temp/iceoryx2/shm` before running anything. This is an
   iceoryx2 bug, not a harness one, but every Windows caller will hit it running the exact
   proof below until an iceoryx2 release past that fix exists.
2. **MinGW has no `<dlfcn.h>`**, which `iceoryx2_stubs.cc` needs. Fixed here by vendoring
   [`dlfcn-win32`](https://github.com/dlfcn-win32/dlfcn-win32) v1.4.2 (MIT, implements
   `dlopen`/`dlsym`/`dlclose` over `LoadLibrary`/`GetProcAddress`) at
   `thirdparty/dlfcn-win32/`, included only on Windows. This keeps `posix_stubs`' generated
   code completely unchanged -- no need for Chromium's separate `windows_lib_x64` stub
   type after all, the *code* the generator already produces just needed a
   `<dlfcn.h>` to compile against.

With both of those, `WEFT_ICEORYX2_PATH` set to the built `iceoryx2_ffi_c.dll`:

    command_publisher: tick 1 ok
    ...
    command_publisher: tick 8 ok
    command_publisher: sent and confirmed 8
    command_subscriber: answered 8, in order

Both exit 0. `run_command_loop`'s actual code path -- `weft::load_bus()`, the dlopen stub
table, `iox2_type_variant_e_DYNAMIC`, `loan_slice_uninit`'s element-count loan, the 8-byte
request-id correlation -- all really ran, not a proxy for it. (A separate same-process
direct-link test against the raw DLL, bypassing the stub table entirely, was written first
as a faster sanity check before wiring up the two-process version above; it also passed
8/8 and is not included here since the two-process run through the real harness code
supersedes it as evidence.)

Two Win32 API warnings appear during the run (`FindNextFileA`/`RemoveDirectoryA`, "no more
files" / "directory is not empty") from iceoryx2's own Windows resource-cleanup path --
cosmetic in this run (both processes still exit 0 with correct results), not investigated
further here, and not new: they're inside iceoryx2 itself, not this repository.

**`dlfcn-win32` is wired into `CMakeLists.txt`** behind `if(WIN32)`, so `cmake -B build -G
Ninja && cmake --build build` on Windows needs no manual flags -- verified with a clean
`Remove-Item -Recurse -Force build` before both the configure and the run above, so this is
what a plane actually gets, not a manual workaround left as prose. One CMake bug found and
fixed getting there: `project(weft-harness CXX)` never enables a C compiler, so
`dlfcn.c` (a C file) got silently dropped from the build with zero errors -- CMake
generated no rule for it at all, and the first sign was `lld-link: error: undefined symbol:
dlopen` at final link, nothing at compile time. Fixed with `enable_language(C)` inside the
`if(WIN32)` block, scoped there since nothing else in this repo is C.

Still open: `WEFT_ICEORYX2_PATH` on Windows needs to point at the `.dll` directly (the
hardcoded fallback names in `bus.hpp`, `libiceoryx2_ffi_c.so`/`.so.0`, are Linux sonames and
don't match a Windows `iceoryx2_ffi_c.dll`) -- no Windows-specific guidance for that is
written down yet the way `LD_LIBRARY_PATH` is documented for Linux above. And the `C:\Temp`
bug is iceoryx2's, not fixable from this repo; every Windows caller needs
`mkdir -p /c/Temp/iceoryx2/shm` (or the Windows equivalent) until an iceoryx2 release past
[#1868](https://github.com/eclipse-iceoryx/iceoryx2/issues/1868) exists.

**Not yet run: on Linux**, which is this project's primary target and everything above was
written for originally. If you run this and it works, replace this paragraph with a real
Linux run log, the same way the
`Snapshot` proof above has one. If it doesn't, that's exactly what this section is for.

`ldd build/weft-harness-publisher` lists no iceoryx2. The library arrives
through `dlopen` at start.

## What this run does not measure

The latency and the rate. Eight messages at a 20 ms cycle measures that a message
arrives, and nothing else. A number belongs in `../logbook/data_plane.md` with the machine
and the settings that produced it, and this run produces none.

## Why iceoryx2 and not iceoryx v1

iceoryx v2.0.8, which is the C++ project, does not build here. Its Linux platform layer
includes `<sys/acl.h>` and links `acl`, and libacl is not allowed.

Neither part can be turned off from the command line. `LINUX` is a normal CMake variable
that shadows the cache, so `-DLINUX=OFF` does nothing, and `ICEORYX_PLATFORM` is a
`CACHE PATH FORCE`. Upstream ships an ACL-free `unix` layer, and reaching it needs a patch
to two build files.

weft's `docs/essays/runtime-choice.md` holds the full reversal, and the cost of it.

## What comes next

1. The thread-per-core loop, once, because every plane uses it.
2. iceoryx2 in the container image, so CI runs this proof rather than a person.
3. The first plane behind it. `zone-server-h2o` is the candidate, because its zone tick
   has no input at all until the bus carries one.

## Waiting

`iox2_node_wait` is a periodic sleep. It cannot wake on a packet, so a process holding both a
bus and a socket would need a second event loop for the network, and two event loops in one
process is the thing an edge should not have.

So the harness binds the **WaitSet**. It takes an arbitrary file descriptor through
`iox2_file_descriptor_new`, which accepts a raw `int`, and `iox2_waitset_attach_notification`
returns a guard that identifies which attachment fired. A UDP socket, a `timerfd` and the bus
therefore wait in one place.

The signatures are transcribed from `iceoryx2-ffi/c/src/api` at v0.9.3, which is the C ABI
itself rather than a generated header, because iceoryx2 generates its header at build time and
this repository must build without it.
