// Copyright (c) 2026 The LCI Project Authors
// SPDX-License-Identifier: NCSA

// ibv_probe: report which verbs features a device actually supports, so the
// LCI ibv backend can be configured for it (in particular Apple RDMA over
// Thunderbolt, whose libibverbs is a reduced API).
//
// Build (standalone, not part of liblci):
//   macOS:  clang++ -std=c++17 -arch arm64 -O2 ibv_probe.cpp -lrdma -o
//   ibv_probe Linux:  g++ -std=c++17 -O2 ibv_probe.cpp -libverbs -o ibv_probe
//
// Usage:
//   ibv_probe [-d dev] [-i port] [-g gid_idx]              # local report only
//   ibv_probe [-d dev] [-i port] [-g gid_idx] -s [-p tcp]  # server side
//   ibv_probe [-d dev] [-i port] [-g gid_idx] [-p tcp] <server-host>
//
// The two-node mode connects one RC and one UC queue pair over the given
// device and tests SEND_WITH_IMM, inline SEND, RDMA_WRITE,
// RDMA_WRITE_WITH_IMM and RDMA_READ on each, printing a result table on the
// client. Address exchange and result reporting use a TCP socket.

#include <infiniband/verbs.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#ifdef __APPLE__
// Apple's libibverbs exports SRQ entry points but does not declare them. The
// declarations below mirror upstream rdma-core and are used only to probe;
// LCI itself never relies on them.
extern "C" {
struct lci_probe_srq_attr {
  uint32_t max_wr;
  uint32_t max_sge;
  uint32_t srq_limit;
};
struct lci_probe_srq_init_attr {
  void* srq_context;
  struct lci_probe_srq_attr attr;
};
struct ibv_srq* ibv_create_srq(struct ibv_pd* pd,
                               struct lci_probe_srq_init_attr* attr);
int ibv_destroy_srq(struct ibv_srq* srq);
}
#define PROBE_HAVE_SRQ_DECL 1
#else
#define PROBE_HAVE_SRQ_DECL 1
typedef struct ibv_srq_init_attr lci_probe_srq_init_attr;
#endif

namespace
{
const int kBufSize = 4096;
const int kNumRecvs = 8;
const int kTimeoutMs = 2000;

struct options_t {
  const char* dev_name = nullptr;
  int ib_port = 1;
  int gid_idx = -1;
  int tcp_port = 18600;
  bool server = false;
  const char* server_host = nullptr;
};

const char* link_layer_str(uint8_t ll)
{
  switch (ll) {
    case IBV_LINK_LAYER_INFINIBAND:
      return "InfiniBand";
    case IBV_LINK_LAYER_ETHERNET:
      return "Ethernet";
    case 100:
      return "Thunderbolt";
    default:
      return "unspecified";
  }
}

const char* gid_type_str(uint32_t t)
{
  switch (t) {
    case IBV_GID_TYPE_IB:
      return "IB";
    case IBV_GID_TYPE_ROCE_V1:
      return "RoCEv1";
    case IBV_GID_TYPE_ROCE_V2:
      return "RoCEv2";
    default:
      return "?";
  }
}

std::string gid_str(const union ibv_gid& gid)
{
  char buf[64];
  inet_ntop(AF_INET6, gid.raw, buf, sizeof(buf));
  return buf;
}

bool gid_is_zero(const union ibv_gid& gid)
{
  for (unsigned char c : gid.raw)
    if (c) return false;
  return true;
}

long now_ms()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ------------------------------------------------------------------------ */
/* TCP helpers                                                              */
/* ------------------------------------------------------------------------ */
bool write_all(int fd, const void* buf, size_t n)
{
  const char* p = static_cast<const char*>(buf);
  while (n > 0) {
    ssize_t r = write(fd, p, n);
    if (r <= 0) return false;
    p += r;
    n -= r;
  }
  return true;
}

bool read_all(int fd, void* buf, size_t n)
{
  char* p = static_cast<char*>(buf);
  while (n > 0) {
    ssize_t r = read(fd, p, n);
    if (r <= 0) return false;
    p += r;
    n -= r;
  }
  return true;
}

int tcp_connect(const char* host, int port)
{
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res;
  std::string port_str = std::to_string(port);
  if (getaddrinfo(host, port_str.c_str(), &hints, &res) != 0) return -1;
  int fd = -1;
  for (auto* ai = res; ai; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  return fd;
}

int tcp_accept(int port)
{
  int lfd = socket(AF_INET6, SOCK_STREAM, 0);
  if (lfd < 0) return -1;
  int on = 1, off = 0;
  setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  setsockopt(lfd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
  struct sockaddr_in6 addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  addr.sin6_port = htons(port);
  if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
      listen(lfd, 1) != 0) {
    close(lfd);
    return -1;
  }
  printf("Waiting for client on TCP port %d ...\n", port);
  int fd = accept(lfd, nullptr, nullptr);
  close(lfd);
  return fd;
}

/* ------------------------------------------------------------------------ */
/* Local report                                                             */
/* ------------------------------------------------------------------------ */
void report_local(struct ibv_context* ctx, struct ibv_pd* pd, int port)
{
  struct ibv_device_attr da;
  if (ibv_query_device(ctx, &da) == 0) {
    printf(
        "Device: fw %s, max_qp %d, max_qp_wr %d, max_sge %d, max_cq %d, "
        "max_cqe %d, max_mr %d, max_mr_size %llu, max_srq %d, "
        "max_srq_wr %d, max_qp_rd_atom %d, phys_port_cnt %d\n",
        da.fw_ver, da.max_qp, da.max_qp_wr, da.max_sge, da.max_cq, da.max_cqe,
        da.max_mr, (unsigned long long)da.max_mr_size, da.max_srq,
        da.max_srq_wr, da.max_qp_rd_atom, da.phys_port_cnt);
  } else {
    printf("ibv_query_device failed: %s\n", strerror(errno));
  }
  struct ibv_device_attr_ex dax;
  memset(&dax, 0, sizeof(dax));
  if (ibv_query_device_ex(ctx, nullptr, &dax) == 0) {
    printf("ODP: general_caps 0x%llx rc 0x%x uc 0x%x ud 0x%x\n",
           (unsigned long long)dax.odp_caps.general_caps,
           dax.odp_caps.per_transport_caps.rc_odp_caps,
           dax.odp_caps.per_transport_caps.uc_odp_caps,
           dax.odp_caps.per_transport_caps.ud_odp_caps);
  } else {
    printf("ibv_query_device_ex: not supported (%s)\n", strerror(errno));
  }

  struct ibv_port_attr pa;
  if (ibv_query_port(ctx, port, &pa) != 0) {
    printf("ibv_query_port(%d) failed: %s\n", port, strerror(errno));
    return;
  }
  printf(
      "Port %d: state %s, link_layer %s (%u), lid 0x%x, active_mtu %d, "
      "max_mtu %d, max_msg_sz %u, active_width %u, active_speed %u, "
      "gid_tbl_len %d\n",
      port, ibv_port_state_str(pa.state), link_layer_str(pa.link_layer),
      pa.link_layer, pa.lid, 128 << pa.active_mtu, 128 << pa.max_mtu,
      pa.max_msg_sz, pa.active_width, pa.active_speed, pa.gid_tbl_len);
  for (int i = 0; i < pa.gid_tbl_len; ++i) {
    struct ibv_gid_entry e;
    if (ibv_query_gid_ex(ctx, port, i, &e, 0) == 0) {
      printf("  gid[%d] %s type %s ifindex %u\n", i, gid_str(e.gid).c_str(),
             gid_type_str(e.gid_type), e.ndev_ifindex);
    } else {
      union ibv_gid gid;
      if (ibv_query_gid(ctx, port, i, &gid) == 0 && !gid_is_zero(gid))
        printf("  gid[%d] %s (type unknown)\n", i, gid_str(gid).c_str());
    }
  }

  // QP type creation probes
  struct ibv_cq* cq = ibv_create_cq(ctx, 16, nullptr, nullptr, 0);
  if (!cq) {
    printf("ibv_create_cq failed: %s\n", strerror(errno));
    return;
  }
  const struct {
    enum ibv_qp_type type;
    const char* name;
  } qpts[] = {{IBV_QPT_RC, "RC"}, {IBV_QPT_UC, "UC"}, {IBV_QPT_UD, "UD"}};
  for (const auto& q : qpts) {
    struct ibv_qp_init_attr ia;
    memset(&ia, 0, sizeof(ia));
    ia.send_cq = cq;
    ia.recv_cq = cq;
    ia.qp_type = q.type;
    ia.cap.max_send_wr = 16;
    ia.cap.max_recv_wr = 16;
    ia.cap.max_send_sge = 1;
    ia.cap.max_recv_sge = 1;
    ia.cap.max_inline_data = 64;
    struct ibv_qp* qp = ibv_create_qp(pd, &ia);
    if (qp) {
      printf("create %s QP: ok (max_inline %u)\n", q.name,
             ia.cap.max_inline_data);
      ibv_destroy_qp(qp);
    } else {
      printf("create %s QP: FAILED (%s)\n", q.name, strerror(errno));
    }
  }
#if PROBE_HAVE_SRQ_DECL
  lci_probe_srq_init_attr sa;
  memset(&sa, 0, sizeof(sa));
  sa.attr.max_wr = 16;
  sa.attr.max_sge = 1;
  struct ibv_srq* srq = ibv_create_srq(pd, &sa);
  if (srq) {
    printf("create SRQ: ok\n");
    ibv_destroy_srq(srq);
  } else {
    printf("create SRQ: FAILED (%s)\n", strerror(errno));
  }
#endif
  ibv_destroy_cq(cq);
}

/* ------------------------------------------------------------------------ */
/* Two-node data path tests                                                 */
/* ------------------------------------------------------------------------ */
struct wire_info_t {
  uint32_t qpn[2];  // RC, UC
  uint32_t rkey;
  uint32_t lid;
  uint64_t addr;
  uint8_t gid[16];
};

struct endpoint_t {
  struct ibv_context* ctx;
  struct ibv_pd* pd;
  int port;
  int gid_idx;
  struct ibv_port_attr pa;
  struct ibv_cq* cq;
  struct ibv_qp* qp[2] = {nullptr, nullptr};
  char* buf;  // [0, kBufSize): RDMA target; then recv buffers; then send buf
  struct ibv_mr* mr;
  char* rdma_buf() { return buf; }
  char* recv_buf(int i) { return buf + kBufSize * (1 + i); }
  char* send_buf() { return buf + kBufSize * (1 + kNumRecvs); }
};

bool modify_init(struct ibv_qp* qp, int port, bool rc)
{
  struct ibv_qp_attr a;
  memset(&a, 0, sizeof(a));
  a.qp_state = IBV_QPS_INIT;
  a.pkey_index = 0;
  a.port_num = port;
  a.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                      (rc ? IBV_ACCESS_REMOTE_READ : 0);
  int rc_ = ibv_modify_qp(
      qp, &a,
      IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
  if (rc_) printf("  modify to INIT failed: %s\n", strerror(rc_));
  return rc_ == 0;
}

bool modify_rtr_rts(endpoint_t& ep, struct ibv_qp* qp, bool rc,
                    const wire_info_t& remote, int idx)
{
  struct ibv_qp_attr a;
  memset(&a, 0, sizeof(a));
  a.qp_state = IBV_QPS_RTR;
  a.path_mtu = ep.pa.active_mtu;
  a.dest_qp_num = remote.qpn[idx];
  a.rq_psn = 0;
  a.ah_attr.dlid = remote.lid;
  a.ah_attr.port_num = ep.port;
  union ibv_gid rgid;
  memcpy(rgid.raw, remote.gid, 16);
  if (!gid_is_zero(rgid)) {
    a.ah_attr.is_global = 1;
    a.ah_attr.grh.dgid = rgid;
    a.ah_attr.grh.sgid_index = ep.gid_idx;
    a.ah_attr.grh.hop_limit = 1;
  }
  int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
              IBV_QP_RQ_PSN;
  if (rc) {
    a.max_dest_rd_atomic = 1;
    a.min_rnr_timer = 12;
    flags |= IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
  }
  int r = ibv_modify_qp(qp, &a, flags);
  if (r) {
    printf("  modify to RTR failed: %s\n", strerror(r));
    return false;
  }
  memset(&a, 0, sizeof(a));
  a.qp_state = IBV_QPS_RTS;
  a.sq_psn = 0;
  flags = IBV_QP_STATE | IBV_QP_SQ_PSN;
  if (rc) {
    a.timeout = 14;
    a.retry_cnt = 7;
    a.rnr_retry = 7;
    a.max_rd_atomic = 1;
    flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
             IBV_QP_MAX_QP_RD_ATOMIC;
  }
  r = ibv_modify_qp(qp, &a, flags);
  if (r) {
    printf("  modify to RTS failed: %s\n", strerror(r));
    return false;
  }
  return true;
}

// Poll for one completion. Returns true and fills wc on success.
bool poll_one(struct ibv_cq* cq, struct ibv_wc* wc, int timeout_ms)
{
  long deadline = now_ms() + timeout_ms;
  while (now_ms() < deadline) {
    int n = ibv_poll_cq(cq, 1, wc);
    if (n < 0) return false;
    if (n == 1) return true;
  }
  return false;
}

bool post_recvs(endpoint_t& ep, struct ibv_qp* qp)
{
  for (int i = 0; i < kNumRecvs; ++i) {
    struct ibv_sge sge = {(uint64_t)ep.recv_buf(i), (uint32_t)kBufSize,
                          ep.mr->lkey};
    struct ibv_recv_wr wr, *bad;
    memset(&wr, 0, sizeof(wr));
    wr.wr_id = 1000 + i;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    int r = ibv_post_recv(qp, &wr, &bad);
    if (r) {
      printf("  ibv_post_recv failed: %s\n", strerror(r));
      return false;
    }
  }
  return true;
}

enum test_id_t {
  T_SEND_IMM,
  T_SEND_INLINE,
  T_WRITE,
  T_WRITE_IMM,
  T_READ,
  T_NUM
};
const char* test_names[T_NUM] = {"SEND_WITH_IMM", "SEND (inline)", "RDMA_WRITE",
                                 "RDMA_WRITE_WITH_IMM", "RDMA_READ"};

void fill_pattern(char* p, int n, int seed)
{
  for (int i = 0; i < n; ++i) p[i] = (char)((i * 7 + seed) & 0xff);
}
bool check_pattern(const char* p, int n, int seed)
{
  for (int i = 0; i < n; ++i)
    if (p[i] != (char)((i * 7 + seed) & 0xff)) return false;
  return true;
}

// Client side of one test: post the operation and wait for its completion.
std::string client_op(endpoint_t& ep, struct ibv_qp* qp, int t,
                      const wire_info_t& remote)
{
  const int len = (t == T_SEND_INLINE) ? 32 : 1024;
  fill_pattern(ep.send_buf(), len, t + 1);
  struct ibv_sge sge = {(uint64_t)ep.send_buf(), (uint32_t)len, ep.mr->lkey};
  struct ibv_send_wr wr, *bad;
  memset(&wr, 0, sizeof(wr));
  wr.wr_id = 42;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;
  switch (t) {
    case T_SEND_IMM:
      wr.opcode = IBV_WR_SEND_WITH_IMM;
      wr.imm_data = htonl(0x1234);
      break;
    case T_SEND_INLINE:
      wr.opcode = IBV_WR_SEND;
      wr.send_flags |= IBV_SEND_INLINE;
      break;
    case T_WRITE:
      wr.opcode = IBV_WR_RDMA_WRITE;
      break;
    case T_WRITE_IMM:
      wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
      wr.imm_data = htonl(0x5678);
      break;
    case T_READ:
      wr.opcode = IBV_WR_RDMA_READ;
      memset(ep.send_buf(), 0, len);
      break;
  }
  if (t == T_WRITE || t == T_WRITE_IMM || t == T_READ) {
    wr.wr.rdma.remote_addr = remote.addr;
    wr.wr.rdma.rkey = remote.rkey;
  }
  int r = ibv_post_send(qp, &wr, &bad);
  if (r) return std::string("post failed: ") + strerror(r);
  struct ibv_wc wc;
  if (!poll_one(ep.cq, &wc, kTimeoutMs)) return "no send completion";
  if (wc.status != IBV_WC_SUCCESS)
    return std::string("completion error: ") + ibv_wc_status_str(wc.status);
  if (t == T_READ && !check_pattern(ep.send_buf(), len, 99))
    return "read data mismatch";
  return "ok";
}

// Server side of one test: verify what arrived.
std::string server_check(endpoint_t& ep, int t)
{
  const int len = (t == T_SEND_INLINE) ? 32 : 1024;
  if (t == T_READ) return "ok";  // verified by the client
  if (t == T_WRITE) {
    // No completion on the target; poll memory.
    long deadline = now_ms() + kTimeoutMs;
    while (now_ms() < deadline) {
      if (check_pattern(ep.rdma_buf(), len, t + 1)) return "ok";
    }
    return "data not delivered";
  }
  struct ibv_wc wc;
  if (!poll_one(ep.cq, &wc, kTimeoutMs)) return "no receive completion";
  if (wc.status != IBV_WC_SUCCESS)
    return std::string("completion error: ") + ibv_wc_status_str(wc.status);
  if (t == T_SEND_IMM) {
    if (wc.opcode != IBV_WC_RECV || !(wc.wc_flags & IBV_WC_WITH_IMM) ||
        ntohl(wc.imm_data) != 0x1234)
      return "bad opcode/imm";
    if (!check_pattern(ep.recv_buf(wc.wr_id - 1000), len, t + 1))
      return "data mismatch";
  } else if (t == T_SEND_INLINE) {
    if (wc.opcode != IBV_WC_RECV) return "bad opcode";
    if (!check_pattern(ep.recv_buf(wc.wr_id - 1000), len, t + 1))
      return "data mismatch";
  } else if (t == T_WRITE_IMM) {
    if (wc.opcode != IBV_WC_RECV_RDMA_WITH_IMM || ntohl(wc.imm_data) != 0x5678)
      return "bad opcode/imm";
    if (!check_pattern(ep.rdma_buf(), len, t + 1)) return "data mismatch";
  }
  return "ok";
}

int run_two_node(endpoint_t& ep, const options_t& opt)
{
  int fd = opt.server ? tcp_accept(opt.tcp_port)
                      : tcp_connect(opt.server_host, opt.tcp_port);
  if (fd < 0) {
    printf("TCP connection failed: %s\n", strerror(errno));
    return 1;
  }
  const bool is_rc[2] = {true, false};
  const char* tname[2] = {"RC", "UC"};
  bool usable[2] = {false, false};

  ep.cq = ibv_create_cq(ep.ctx, 64, nullptr, nullptr, 0);
  if (!ep.cq) {
    printf("ibv_create_cq failed\n");
    return 1;
  }
  wire_info_t local;
  memset(&local, 0, sizeof(local));
  for (int k = 0; k < 2; ++k) {
    struct ibv_qp_init_attr ia;
    memset(&ia, 0, sizeof(ia));
    ia.send_cq = ep.cq;
    ia.recv_cq = ep.cq;
    ia.qp_type = is_rc[k] ? IBV_QPT_RC : IBV_QPT_UC;
    ia.cap.max_send_wr = 16;
    ia.cap.max_recv_wr = kNumRecvs;
    ia.cap.max_send_sge = 1;
    ia.cap.max_recv_sge = 1;
    ia.cap.max_inline_data = 64;
    ep.qp[k] = ibv_create_qp(ep.pd, &ia);
    if (ep.qp[k] && modify_init(ep.qp[k], ep.port, is_rc[k])) {
      local.qpn[k] = ep.qp[k]->qp_num;
      usable[k] = true;
    } else {
      printf("%s QP unavailable: %s\n", tname[k], strerror(errno));
    }
  }
  local.rkey = ep.mr->rkey;
  local.addr = (uint64_t)ep.rdma_buf();
  local.lid = ep.pa.lid;
  if (ep.gid_idx >= 0) {
    union ibv_gid gid;
    if (ibv_query_gid(ep.ctx, ep.port, ep.gid_idx, &gid) == 0)
      memcpy(local.gid, gid.raw, 16);
  }
  wire_info_t remote;
  if (!write_all(fd, &local, sizeof(local)) ||
      !read_all(fd, &remote, sizeof(remote))) {
    printf("address exchange failed\n");
    return 1;
  }

  std::vector<std::string> results;  // client only
  for (int k = 0; k < 2; ++k) {
    bool ok = usable[k] && remote.qpn[k] != 0 &&
              modify_rtr_rts(ep, ep.qp[k], is_rc[k], remote, k) &&
              post_recvs(ep, ep.qp[k]);
    // Agree on whether both sides are ready.
    char mine = ok, theirs = 0;
    write_all(fd, &mine, 1);
    read_all(fd, &theirs, 1);
    if (!mine || !theirs) {
      if (!opt.server)
        for (int t = 0; t < T_NUM; ++t)
          results.push_back(std::string(tname[k]) + " " + test_names[t] +
                            ": connect failed");
      continue;
    }
    for (int t = 0; t < T_NUM; ++t) {
      memset(ep.rdma_buf(), 0, kBufSize);
      if (t == T_READ) fill_pattern(ep.rdma_buf(), 1024, 99);
      char go = 1;
      // Handshake so the target buffer is reset before the operation.
      if (opt.server) {
        write_all(fd, &go, 1);
        std::string res = server_check(ep, t);
        char len = (char)res.size();
        write_all(fd, &len, 1);
        write_all(fd, res.data(), res.size());
      } else {
        read_all(fd, &go, 1);
        std::string res;
        if (t == T_READ && !is_rc[k]) {
          res = "skipped (not defined for UC)";
        } else {
          res = client_op(ep, ep.qp[k], t, remote);
        }
        char len = 0;
        read_all(fd, &len, 1);
        std::string sres(len, '\0');
        read_all(fd, &sres[0], len);
        results.push_back(std::string(tname[k]) + " " + test_names[t] +
                          ": sender=" + res + ", target=" + sres);
      }
    }
  }
  if (!opt.server) {
    printf("\n==== Data path results ====\n");
    for (auto& r : results) printf("%s\n", r.c_str());
  }
  for (int k = 0; k < 2; ++k)
    if (ep.qp[k]) ibv_destroy_qp(ep.qp[k]);
  ibv_destroy_cq(ep.cq);
  close(fd);
  return 0;
}

void usage(const char* argv0)
{
  fprintf(stderr,
          "Usage: %s [-d dev] [-i ib_port] [-g gid_idx] [-p tcp_port] "
          "[-s | server_host]\n",
          argv0);
}
}  // namespace

int main(int argc, char** argv)
{
  options_t opt;
  int c;
  while ((c = getopt(argc, argv, "d:i:g:p:sh")) != -1) {
    switch (c) {
      case 'd':
        opt.dev_name = optarg;
        break;
      case 'i':
        opt.ib_port = atoi(optarg);
        break;
      case 'g':
        opt.gid_idx = atoi(optarg);
        break;
      case 'p':
        opt.tcp_port = atoi(optarg);
        break;
      case 's':
        opt.server = true;
        break;
      default:
        usage(argv[0]);
        return 1;
    }
  }
  if (optind < argc) opt.server_host = argv[optind];

  int ndev = 0;
  struct ibv_device** list = ibv_get_device_list(&ndev);
  if (!list || ndev == 0) {
    printf(
        "No RDMA devices found. On macOS, enable RDMA with `rdma_ctl "
        "enable` from Recovery and connect a Thunderbolt 5 cable.\n");
    return 1;
  }
  struct ibv_device* dev = nullptr;
  for (int i = 0; i < ndev; ++i) {
    printf("device[%d] %s\n", i, ibv_get_device_name(list[i]));
    if (!dev && (!opt.dev_name ||
                 strcmp(opt.dev_name, ibv_get_device_name(list[i])) == 0))
      dev = list[i];
  }
  if (!dev) {
    printf("Device %s not found\n", opt.dev_name);
    return 1;
  }
  endpoint_t ep;
  ep.ctx = ibv_open_device(dev);
  if (!ep.ctx) {
    printf("ibv_open_device failed: %s\n", strerror(errno));
    return 1;
  }
  ep.pd = ibv_alloc_pd(ep.ctx);
  if (!ep.pd) {
    printf("ibv_alloc_pd failed: %s\n", strerror(errno));
    return 1;
  }
  ep.port = opt.ib_port;
  printf("\n==== Local report: %s port %d ====\n", ibv_get_device_name(dev),
         ep.port);
  report_local(ep.ctx, ep.pd, ep.port);
  if (ibv_query_port(ep.ctx, ep.port, &ep.pa) != 0) return 1;

  ep.gid_idx = opt.gid_idx;
  if (ep.gid_idx < 0 && ep.pa.link_layer != IBV_LINK_LAYER_INFINIBAND) {
    for (int i = 0; i < ep.pa.gid_tbl_len; ++i) {
      union ibv_gid gid;
      if (ibv_query_gid(ep.ctx, ep.port, i, &gid) == 0 && !gid_is_zero(gid)) {
        ep.gid_idx = i;
        break;
      }
    }
    printf("Using gid index %d (override with -g)\n", ep.gid_idx);
  }

  int ret = 0;
  if (opt.server || opt.server_host) {
    size_t bytes = (size_t)kBufSize * (2 + kNumRecvs);
    if (posix_memalign((void**)&ep.buf, 4096, bytes) != 0) return 1;
    memset(ep.buf, 0, bytes);
    ep.mr = ibv_reg_mr(ep.pd, ep.buf, bytes,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                           IBV_ACCESS_REMOTE_READ);
    if (!ep.mr) {
      printf("ibv_reg_mr with REMOTE_READ failed (%s); retrying without\n",
             strerror(errno));
      ep.mr = ibv_reg_mr(ep.pd, ep.buf, bytes,
                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    }
    if (!ep.mr) {
      printf("ibv_reg_mr failed: %s\n", strerror(errno));
      return 1;
    }
    ret = run_two_node(ep, opt);
    ibv_dereg_mr(ep.mr);
    free(ep.buf);
  }
  ibv_dealloc_pd(ep.pd);
  ibv_close_device(ep.ctx);
  ibv_free_device_list(list);
  return ret;
}
