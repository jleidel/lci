@page apple_rdma Apple RDMA over Thunderbolt

[TOC]

# Overview

macOS 26.2 and later ships RDMA over Thunderbolt 5, exposed through a
libibverbs API in the macOS SDK (`<infiniband/verbs.h>`, linked as
`-lrdma`). LCI's ibv backend can run on it, so Macs connected by Thunderbolt 5
cables can communicate through LCI with RDMA.

Apple's verbs API is smaller than Linux rdma-core. In particular, it does not
expose shared receive queues (SRQ), thread domains, `ibv_advise_mr` or
DMA-BUF registration. LCI detects these features at configure time and adapts:

| Feature | Linux rdma-core | Apple libibverbs |
|---|---|---|
| QP transport (`LCI_ATTR_IBV_QP_TYPE=auto_select`) | RC | UC |
| Receive queue | one shared SRQ per device | one receive queue per QP, filled from a per-device buffer stash |
| Flow control | RC retransmission / RNR retry | receiver-advertised credits (see below) |
| `get` (RDMA read) | supported | **not supported** on UC |
| Thread domains, MR prefetch | supported | ignored |

# Requirements

1. Two or more Apple-silicon Macs with Thunderbolt 5, connected directly.
2. RDMA enabled on every Mac: boot into Recovery and run `rdma_ctl enable`.
   Check with `rdma_ctl status` and `ibv_devices` / `ibv_devinfo -v`.
3. Xcode or the Command Line Tools with a macOS SDK that contains
   `usr/include/infiniband/verbs.h`.
4. Any IP connectivity between the Macs (Ethernet, Wi-Fi or Thunderbolt
   Bridge) and ssh access, for bootstrap only.

# Building

Apple's RDMA library has only arm64 slices. Build for arm64; this matters if
your CMake runs under Rosetta, where the default architecture is x86_64:

```
cmake -S . -B build -G Ninja \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DLCI_NETWORK_BACKENDS=ibv,ofi
cmake --build build
```

The configure output reports what was found:

```
-- Found IBV: .../MacOSX.sdk/usr/include
-- libibverbs feature SRQ: no
...
-- libibverbs feature LINK_LAYER_THUNDERBOLT: yes
```

If RDMA is disabled or no device is present, the ibv backend reports itself
unavailable at runtime and LCI falls back to the next enabled backend (e.g.,
ofi).

# Checking the hardware

`tools/ibv_probe/ibv_probe.cpp` is a standalone program that reports what the
device supports and tests the RC and UC data paths between two Macs:

```
clang++ -std=c++17 -arch arm64 -O2 tools/ibv_probe/ibv_probe.cpp -lrdma -o ibv_probe
./ibv_probe -s                 # on Mac A
./ibv_probe <mac-a-hostname>   # on Mac B
```

If RC and RDMA_READ work, you can use `LCI_ATTR_IBV_QP_TYPE=rc` to get
`get` support and hardware reliability.

# Running

Use `tools/lcrun-ssh` to start ranks on several Macs. It bootstraps with the
LCT TCP PMI backend and forwards `LCI_*` / `LCT_*` environment variables:

```
LCI_ATTR_BACKEND=ibv tools/lcrun-ssh -H mac-a,mac-b -- \
    build/tests/unit/collective/lci_test_collective
```

The build directory must be at the same path on every Mac (or pass `-C`).

# Tuning and limitations

- `LCI_ATTR_IBV_RECV_DEPTH_PER_QP` (default 64) sets the receive queue depth
  of each QP. Each peer can have at most this many messages in flight to a
  rank before waiting for credits. The device's `net_max_recvs` buffers are
  shared among all QPs, so keep `net_max_recvs` at least
  `(number of ranks) x (depth)` for full throughput.
- **Credits.** UC drops messages that arrive when no receive is posted, so
  senders track how many receives each peer has posted for them. Receivers
  advertise this with a small RDMA write into the sender's credit array.
  Sends and put-with-immediate operations without credit return
  `retry_nomem` and are retried by LCI's backlog queue.
- **Reliability.** UC does not retransmit. A packet lost on the link shows up
  as a hang, not an error. Thunderbolt links are normally lossless, but this
  is the main risk of the UC transport.
- **No get.** `post_get` fails with a fatal error on UC. Send/receive,
  active messages and put (including the rendezvous protocol, which uses
  writes) do not depend on it.
