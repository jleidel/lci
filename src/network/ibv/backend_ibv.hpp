// Copyright (c) 2025 The LCI Project Authors
// SPDX-License-Identifier: NCSA

#ifndef LCI_BACKEND_IBV_BACKEND_IBV_HPP
#define LCI_BACKEND_IBV_BACKEND_IBV_HPP

#include "infiniband/verbs.h"
#include <atomic>
#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

#define IBV_SAFECALL(x)                                                        \
  do {                                                                         \
    int err = (x);                                                             \
    if (err) {                                                                 \
      LCI_Assert(false, "err %d : %s (%s:%d)\n", err, strerror(err), __FILE__, \
                 __LINE__);                                                    \
    }                                                                          \
  } while (0)

#define IBV_SAFECALL_RET(x)                                                    \
  do {                                                                         \
    int err = (x);                                                             \
    if (err) {                                                                 \
      LCI_Assert(false, "err %d : %s (%s:%d)\n", err, strerror(err), __FILE__, \
                 __LINE__);                                                    \
    }                                                                          \
    return errorcode_t::fatal;                                                 \
  } while (0)

namespace lci
{
namespace ibv_detail
{
// wr_id used for internal credit-update RDMA writes. Never a valid pointer.
constexpr uint64_t credit_wr_id = UINT64_MAX;
}  // namespace ibv_detail

class ibv_net_context_impl_t : public lci::net_context_impl_t
{
 public:
  static bool check_availability();

  ibv_net_context_impl_t(runtime_t runtime_, attr_t attr_);
  ~ibv_net_context_impl_t() override;
  device_t alloc_device(device_t::attr_t attr) override;

  struct ibv_device** ib_dev_list;
  struct ibv_device* ib_dev;
  struct ibv_context* ib_context;
  struct ibv_pd* ib_pd;
  struct ibv_device_attr ib_dev_attr;
  struct ibv_device_attr_ex ib_dev_attrx;
  struct ibv_port_attr ib_port_attr;
  uint8_t ib_dev_port;
  struct ibv_mr* ib_odp_mr;
  size_t max_inline;
  union ibv_gid ib_gid;
  // The resolved QP transport (IBV_QPT_RC or IBV_QPT_UC).
  enum ibv_qp_type qp_type;
  // Whether receives are posted to a shared receive queue. When false, each
  // QP has its own receive queue fed from a per-device software stash.
  bool use_srq;
  // Whether senders must hold receiver-advertised credits before posting
  // recv-consuming operations (required for UC, which has no RNR retry).
  bool use_credits;
};

struct LCISI_ibv_qp_extra_t {
  struct ibv_td* ib_td;
  struct ibv_pd* ib_pd;
  LCIU_CACHE_PADDING(sizeof(struct ibv_td*) + sizeof(struct ibv_pd*));
  spinlock_t lock;
  LCIU_CACHE_PADDING(sizeof(spinlock_t));
};

class ibv_mr_impl_t : public lci::mr_impl_t
{
 public:
  struct ibv_mr* ibv_mr;
};

class ibv_endpoint_impl_t;

// A receive buffer handed to the backend by the device layer.
struct ibv_recv_item_t {
  uint64_t addr;
  uint32_t length;
  uint32_t lkey;
  uint64_t wr_id;
};

// Per-QP receive state, used when shared receive queues are not available.
// All non-atomic fields are protected by the owning device's srq_lock.
struct ibv_qp_recv_state_t {
  struct ibv_qp* qp = nullptr;
  ibv_endpoint_impl_t* endpoint = nullptr;
  int rank = -1;
  // Receives currently posted on this QP, in posting order (receive queues
  // complete in FIFO order).
  std::deque<ibv_recv_item_t> posted;
  // Whether this QP is queued in the device's hungry list.
  bool in_hungry = false;
  // Whether the QP is connected (RTS) and can carry credit updates.
  bool connected = false;
  // Credit flow control: cumulative number of receives ever posted on this QP
  // and the last value advertised to the peer.
  uint64_t total_posted = 0;
  uint64_t advertised = 0;
  bool credit_pending = false;
};
class qp2rank_map_t
{
 public:
  struct entry_t {
    int rank = -1;
    padded_atomic_t<int>* slots = nullptr;
    ibv_qp_recv_state_t* recv_state = nullptr;
  };

  struct snapshot_t {
    int mod = 1;
    std::vector<entry_t> table;

    entry_t get_entry(uint32_t qp_num);
  };

  ~qp2rank_map_t()
  {
    if (current_snapshot) {
      delete current_snapshot;
    }
  }

  void add_qps(const std::vector<struct ibv_qp*>& qps,
               std::vector<padded_atomic_t<int>>* qp_slots,
               std::vector<ibv_qp_recv_state_t>* recv_states = nullptr);
  void remove_qps(const std::vector<struct ibv_qp*>& qps);
  snapshot_t* get_snapshot()
  {
    return current_snapshot.load(std::memory_order_relaxed);
  }

 private:
  void rebuild_locked();

  std::vector<std::pair<uint32_t, entry_t>> qp_entries;
  std::atomic<snapshot_t*> current_snapshot = nullptr;
  mutable std::shared_mutex mutex;
};

class ibv_device_impl_t : public lci::device_impl_t
{
 public:
  ibv_device_impl_t(net_context_t context_, device_t::attr_t attr_);
  ~ibv_device_impl_t() override;
  endpoint_t alloc_endpoint_impl(endpoint_t::attr_t attr) override;
  mr_t register_memory_impl(void* buffer, size_t size) override;
  void deregister_memory_impl(mr_impl_t*) override;
  uint64_t get_rkey(mr_impl_t* mr) override;
  size_t poll_comp_impl(net_status_t* p_statuses, size_t max_polls) override;
  error_t post_recv_impl(void* buffer, size_t size, mr_t mr,
                         void* user_context) override;
  size_t post_recvs_impl(void* buffers[], size_t size, size_t count, mr_t mrm,
                         void* usesr_contexts[]) override;

  // Per-QP receive queue management (when !use_srq). Must be called with
  // srq_lock held.
  void add_recv_qps_locked(std::vector<ibv_qp_recv_state_t>& states);
  void remove_recv_qps_locked(std::vector<ibv_qp_recv_state_t>& states);
  void distribute_recvs_locked();
  void mark_hungry_locked(ibv_qp_recv_state_t* state);
  // Try to advertise receive credits to the peer of a QP (when use_credits).
  // Must be called with srq_lock held.
  void advertise_credits_locked(ibv_qp_recv_state_t* state, bool force);
  void flush_pending_credits_locked();

  // Connections O(N)
  struct ibv_td* ib_td;
  struct ibv_pd* ib_pd;
  struct ibv_cq* ib_cq;
  struct ibv_srq* ib_srq;
  qp2rank_map_t qp2rank_map;
  bool use_srq;
  bool use_credits;
  size_t recv_depth_per_qp;
  size_t credit_batch;
  // Software receive stash and the list of QPs needing receives (!use_srq).
  // Protected by srq_lock.
  std::deque<ibv_recv_item_t> recv_stash;
  std::deque<ibv_qp_recv_state_t*> hungry_qps;
  std::vector<ibv_qp_recv_state_t*> recv_qps;
  std::atomic<bool> credit_pending_any{false};

  ibv_mr_impl_t odp_mr;
  LCIU_CACHE_PADDING(0);
  spinlock_t srq_lock;
  LCIU_CACHE_PADDING(sizeof(spinlock_t));
  spinlock_t cq_lock;
  LCIU_CACHE_PADDING(sizeof(spinlock_t));
};

class ibv_endpoint_impl_t : public lci::endpoint_impl_t
{
 public:
  ibv_endpoint_impl_t(device_t device_, attr_t attr_);
  ~ibv_endpoint_impl_t() override;
  error_t post_sends_impl(int rank, void* buffer, size_t size,
                          net_imm_data_t imm_data, void* user_context,
                          bool high_priority) override;
  error_t post_send_impl(int rank, void* buffer, size_t size, mr_t mr,
                         net_imm_data_t imm_data, void* user_context,
                         bool high_priority) override;
  error_t post_puts_impl(int rank, void* buffer, size_t size, uint64_t offset,
                         rmr_t rmr, void* user_context,
                         bool high_priority) override;
  error_t post_put_impl(int rank, void* buffer, size_t size, mr_t mr,
                        uint64_t offset, rmr_t rmr, void* user_context,
                        bool high_priority) override;
  error_t post_putImms_impl(int rank, void* buffer, size_t size,
                            uint64_t offset, rmr_t rmr, net_imm_data_t imm_data,
                            void* user_context, bool high_priority) override;
  error_t post_putImm_impl(int rank, void* buffer, size_t size, mr_t mr,
                           uint64_t offset, rmr_t rmr, net_imm_data_t imm_data,
                           void* user_context, bool high_priority) override;
  error_t post_get_impl(int rank, void* buffer, size_t size, mr_t mr,
                        uint64_t offset, rmr_t rmr, void* user_context,
                        bool high_priority) override;

  ibv_device_impl_t* p_ibv_device;
  std::vector<struct ibv_qp*> ib_qps;
  std::vector<LCISI_ibv_qp_extra_t> ib_qp_extras;
  std::vector<padded_atomic_t<int>> qp_remaining_slots;
  spinlock_t qps_lock;

  // Per-QP receive state (only when the device does not use an SRQ).
  std::vector<ibv_qp_recv_state_t> recv_states;

  // Credit flow control (only when the device uses credits).
  // credit_limits[r]: cumulative receives rank r has posted on its QP to us;
  // written remotely by rank r through RDMA write.
  uint64_t* credit_limits = nullptr;
  struct ibv_mr* credit_mr = nullptr;
  // credits_consumed[r]: recv-consuming operations we have posted to rank r.
  std::vector<padded_atomic_t<uint64_t>> credits_consumed;
  // Where to write our credit advertisements on each peer.
  std::vector<uint64_t> peer_credit_addr;
  std::vector<uint32_t> peer_credit_rkey;

  // Post an RDMA write advertising `value` receive credits to `rank`.
  // Returns true if posted.
  bool post_credit_update(int rank, uint64_t value);

 private:
  bool try_lock_qp(int rank);
  void unlock_qp(int rank);
  bool try_acquire_slot(int rank, bool high_priority);
  void release_slot(int rank);
  bool try_acquire_credit(int rank);
  void release_credit(int rank);
};

}  // namespace lci

#endif  // LCI_BACKEND_IBV_BACKEND_IBV_HPP
