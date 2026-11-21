// Copyright (c) 2025-2026 The LCI Project Authors
// SPDX-License-Identifier: NCSA

#include "lci_internal.hpp"
#include "network/ibv/backend_ibv_detail.hpp"

namespace lci
{
namespace
{
const int max_sge_num = 1;
size_t g_qp_num = 0;
[[maybe_unused]] size_t g_td_num = 0;

const char* mtu_str(enum ibv_mtu mtu)
{
  switch (mtu) {
    case IBV_MTU_256:
      return "256";
    case IBV_MTU_512:
      return "512";
    case IBV_MTU_1024:
      return "1024";
    case IBV_MTU_2048:
      return "2048";
    case IBV_MTU_4096:
      return "4096";
    default:
      return "invalid MTU";
  }
}
}  // namespace

//

bool ibv_net_context_impl_t::check_availability()
{
  int num_devices = 0;
  struct ibv_device** dev_list = ibv_get_device_list(&num_devices);
  if (!dev_list) return false;
  // A device alone is not enough; it needs a usable port (e.g., a RoCE port
  // with a valid GID).
  struct ibv_device* dev;
  uint8_t port;
  bool ret = ibv_detail::select_best_device_port(
      dev_list, num_devices, g_default_attr.device_name.c_str(), &dev, &port);
  ibv_free_device_list(dev_list);
  return ret;
}

ibv_net_context_impl_t::ibv_net_context_impl_t(runtime_t runtime_, attr_t attr_)
    : net_context_impl_t(runtime_, attr_)
{
  int num_devices;
  ib_dev_list = ibv_get_device_list(&num_devices);
  LCI_Assert(num_devices > 0,
             "Unable to find any IB devices. Are you running on an Infiniband "
             "machine? Try the LCI ofi backend by building LCI with "
             "`-DLCI_NETWORK_BACKENDS=ofi`.\n");

  bool ret = ibv_detail::select_best_device_port(ib_dev_list, num_devices,
                                                 attr.device_name.c_str(),
                                                 &ib_dev, &ib_dev_port);
  LCI_Assert(ret, "Cannot find available ibv device/port!\n");

  // ibv_open_device provides the user with a verbs context which is the object
  // that will be used for all other verb operations.
  ib_context = ibv_open_device(ib_dev);
  LCI_Assert(ib_context, "Couldn't get context for %s\n",
             ibv_get_device_name(ib_dev));

  // allocate protection domain
  ib_pd = ibv_alloc_pd(ib_context);
  LCI_Assert(ib_pd, "Couldn't allocate PD\n");

  // query device attribute
  int rc = ibv_query_device(ib_context, &ib_dev_attr);
  LCI_Assert(rc == 0, "Unable to query device\n");

  rc = ibv_query_device_ex(ib_context, nullptr, &ib_dev_attrx);
  LCI_Assert(rc == 0, "Unable to query device for its extended features\n");

  // configure on-demand paging
  ib_odp_mr = nullptr;
  if (attr.ibv_odp_strategy == attr_ibv_odp_strategy_t::implicit_odp) {
#if LCI_IBV_HAVE_ODP_TRANSPORT_CAPS
    const uint32_t rc_caps_mask = IBV_ODP_SUPPORT_SEND | IBV_ODP_SUPPORT_RECV |
                                  IBV_ODP_SUPPORT_WRITE | IBV_ODP_SUPPORT_READ |
                                  IBV_ODP_SUPPORT_SRQ_RECV;
    LCI_Assert((ib_dev_attrx.odp_caps.general_caps & IBV_ODP_SUPPORT) &&
                   ((ib_dev_attrx.odp_caps.per_transport_caps.rc_odp_caps &
                     rc_caps_mask) == rc_caps_mask),
               "The device isn't ODP capable\n");
#else
    LCI_Assert(ib_dev_attrx.odp_caps.general_caps & IBV_ODP_SUPPORT,
               "The device isn't ODP capable\n");
#endif
    LCI_Assert(ib_dev_attrx.odp_caps.general_caps & IBV_ODP_SUPPORT_IMPLICIT,
               "The device doesn't support implicit ODP\n");
    ib_odp_mr = ibv_reg_mr(ib_pd, nullptr, SIZE_MAX,
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                               IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_ON_DEMAND);
    LCI_Assert(ib_odp_mr, "Couldn't register MR for ODP\n");
  }

  // Query the selected active port.
  rc = ibv_query_port(ib_context, ib_dev_port, &ib_port_attr);
  LCI_Assert(rc == 0, "Unable to query port %u on device %s\n", ib_dev_port,
             ibv_get_device_name(ib_dev));
  LCI_Assert(
      ib_port_attr.link_layer != IBV_LINK_LAYER_INFINIBAND || ib_port_attr.lid,
      "Couldn't get local LID\n");

  LCI_Log(LOG_INFO, "ibv", "Maximum MTU: %s; Active MTU: %s\n",
          mtu_str(ib_port_attr.max_mtu), mtu_str(ib_port_attr.active_mtu));

  // Check max_msg_sz
  if (attr.max_msg_size > ib_port_attr.max_msg_sz) {
    attr.max_msg_size = ib_port_attr.max_msg_sz;
    LCI_Log(LOG_INFO, "ibv",
            "Reduce LCI_MAX_SINGLE_MESSAGE_SIZE to %lu "
            "as required by libibverbs max message size\n",
            attr.max_msg_size);
  }

  // query the gid
  if (attr.ibv_gid_idx < 0 &&
      (attr.ibv_force_gid_auto_select ||
       ibv_detail::is_gid_addressed_link_layer(ib_port_attr.link_layer))) {
    // User did not explicitly specify the gid to use and we are using RoCE
    attr.ibv_gid_idx = ibv_detail::select_best_gid_for_roce(
        ib_dev, ib_context, ib_pd, ib_port_attr, ib_dev_port);
    LCI_Assert(attr.ibv_gid_idx >= 0, "No valid GID on %s:%u\n",
               ibv_get_device_name(ib_dev), ib_dev_port);
  }
  if (attr.ibv_gid_idx >= 0) {
    LCI_Log(LOG_INFO, "ibv", "Use GID index: %d\n", attr.ibv_gid_idx);
    rc = ibv_query_gid(ib_context, ib_dev_port, attr.ibv_gid_idx, &ib_gid);
    LCI_Assert(rc == 0, "can't read gid of index %d\n", attr.ibv_gid_idx);
  } else
    memset(&ib_gid, 0, sizeof(ib_gid));

  attr.support_putimm = true;

  // Resolve the QP transport.
  switch (attr.ibv_qp_type) {
    case attr_ibv_qp_type_t::rc:
      qp_type = IBV_QPT_RC;
      break;
    case attr_ibv_qp_type_t::uc:
      qp_type = IBV_QPT_UC;
      break;
    default:
      // RC needs a shared receive queue or RNR-based flow control; without
      // an SRQ API (e.g., Apple's libibverbs for RDMA over Thunderbolt), use
      // UC, which is what the vendor's own tooling exercises.
      qp_type = LCI_IBV_HAVE_SRQ ? IBV_QPT_RC : IBV_QPT_UC;
      break;
  }
  use_srq = LCI_IBV_HAVE_SRQ && qp_type == IBV_QPT_RC;
  // UC silently drops messages that arrive without a posted receive.
  use_credits = qp_type == IBV_QPT_UC;
  // UC does not support RDMA read.
  attr.support_get = qp_type == IBV_QPT_RC;
  LCI_Log(LOG_INFO, "ibv", "QP transport: %s; receive queue: %s\n",
          qp_type == IBV_QPT_RC ? "RC" : "UC", use_srq ? "shared" : "per-QP");
  if (attr.ibv_odp_strategy == attr_ibv_odp_strategy_t::implicit_odp &&
      !use_srq) {
    LCI_Warn("Implicit ODP is untested without a shared receive queue\n");
  }
}

ibv_net_context_impl_t::~ibv_net_context_impl_t()
{
  ibv_free_device_list(ib_dev_list);
  if (ib_odp_mr != nullptr) IBV_SAFECALL(ibv_dereg_mr(ib_odp_mr));
  IBV_SAFECALL(ibv_dealloc_pd(ib_pd));
  IBV_SAFECALL(ibv_close_device(ib_context));
}

device_t ibv_net_context_impl_t::alloc_device(device_t::attr_t attr)
{
  device_t ret;
  ret.p_impl = new ibv_device_impl_t(net_context, attr);
  return ret;
}

ibv_device_impl_t::ibv_device_impl_t(net_context_t net_context_,
                                     device_t::attr_t attr_)
    : device_impl_t(net_context_, attr_)
{
  ibv_net_context_impl_t* p_net_context =
      static_cast<ibv_net_context_impl_t*>(net_context.p_impl);

  odp_mr.ibv_mr = p_net_context->ib_odp_mr;

  // adjust max_send, max_recv, max_cqe
  // Note: the max value reported by device attributes might still not be the
  // strict upper bound.
  if (attr.net_max_sends >
      static_cast<size_t>(p_net_context->ib_dev_attr.max_qp_wr)) {
    LCI_Log(LOG_INFO, "ibv",
            "The configured net_max_sends (%d) is adjusted as it is "
            "larger than the maximum allowable value by the device (%d).\n",
            attr.net_max_sends, p_net_context->ib_dev_attr.max_qp_wr);
    attr.net_max_sends = p_net_context->ib_dev_attr.max_qp_wr;
  }
  use_srq = p_net_context->use_srq;
  use_credits = p_net_context->use_credits;
  if (use_srq &&
      attr.net_max_recvs >
          static_cast<size_t>(p_net_context->ib_dev_attr.max_srq_wr)) {
    LCI_Log(LOG_INFO, "ibv",
            "The configured LCI_SERVER_MAX_RECVS (%d) is adjusted as it is "
            "larger than the maximum allowable value by the device (%d).\n",
            attr.net_max_recvs, p_net_context->ib_dev_attr.max_srq_wr);
    attr.net_max_recvs = p_net_context->ib_dev_attr.max_srq_wr;
  }
  if (attr.net_max_cqes >
      static_cast<size_t>(p_net_context->ib_dev_attr.max_cqe)) {
    LCI_Log(LOG_INFO, "ibv",
            "The configured LCI_SERVER_MAX_CQES (%d) is adjusted as it is "
            "larger than the maximum allowable value by the device (%d).\n",
            attr.net_max_cqes, p_net_context->ib_dev_attr.max_cqe);
    attr.net_max_cqes = p_net_context->ib_dev_attr.max_cqe;
  }

  ib_srq = nullptr;
  recv_depth_per_qp = net_context_attr.ibv_recv_depth_per_qp;
  if (recv_depth_per_qp >
      static_cast<size_t>(p_net_context->ib_dev_attr.max_qp_wr)) {
    LCI_Log(LOG_INFO, "ibv",
            "The configured ibv_recv_depth_per_qp (%lu) is adjusted to the "
            "device maximum (%d).\n",
            recv_depth_per_qp, p_net_context->ib_dev_attr.max_qp_wr);
    recv_depth_per_qp = p_net_context->ib_dev_attr.max_qp_wr;
  }
  LCI_Assert(use_srq || recv_depth_per_qp > 0,
             "ibv_recv_depth_per_qp must be positive\n");
  // Advertise credits after this many new receives (or when a QP refills).
  credit_batch = std::max<size_t>(1, recv_depth_per_qp / 4);
  if (use_srq) {
#if LCI_IBV_HAVE_SRQ
    // Create shared-receive queue, **number here affect performance**.
    struct ibv_srq_init_attr srq_attr;
    memset(&srq_attr, 0, sizeof(srq_attr));
    srq_attr.srq_context = nullptr;
    srq_attr.attr.max_wr = attr.net_max_recvs;
    srq_attr.attr.max_sge = max_sge_num;
    srq_attr.attr.srq_limit = 0;
    ib_srq = ibv_create_srq(p_net_context->ib_pd, &srq_attr);
    LCI_Assert(ib_srq, "Couldn't create SRQ\n");
#else
    LCI_Assert(false, "This libibverbs does not support SRQ\n");
#endif
  }

  // Create completion queues.
  ib_cq = ibv_create_cq(p_net_context->ib_context, attr.net_max_cqes, nullptr,
                        nullptr, 0);
  LCI_Assert(ib_cq, "Couldn't create CQ\n");

  ib_pd = nullptr;
  ib_td = nullptr;
#if !LCI_IBV_HAVE_TD
  if (attr.ibv_td_strategy != attr_ibv_td_strategy_t::none) {
    LCI_Log(LOG_INFO, "ibv",
            "Thread domains are not supported by this libibverbs; "
            "ignoring ibv_td_strategy\n");
    attr.ibv_td_strategy = attr_ibv_td_strategy_t::none;
  }
#else
  if (attr.ibv_td_strategy == attr_ibv_td_strategy_t::all_qp) {
    // allocate one thread domain for all queue pairs
    struct ibv_td_init_attr td_attr;
    td_attr.comp_mask = 0;
    ib_td = ibv_alloc_td(p_net_context->ib_context, &td_attr);
    ++g_td_num;
    if (ib_td != nullptr) {
      struct ibv_parent_domain_init_attr attr;
      attr.td = ib_td;
      attr.pd = p_net_context->ib_pd;
      attr.comp_mask = 0;
      ib_pd = ibv_alloc_parent_domain(p_net_context->ib_context, &attr);
      if (ib_pd == nullptr) {
        LCI_Warn(
            "ibv_alloc_parent_domain() failed (No. %lu; %s); decalloc the "
            "thread "
            "domain\n",
            g_td_num, strerror(errno));
        IBV_SAFECALL(ibv_dealloc_td(ib_td));
        --g_td_num;
      }
    } else {
      LCI_Warn(
          "ibv_alloc_td() failed (%s). Use `export "
          "LCI_ATTR_IBV_TD_STRATEGY=none` to suppress this warning\n",
          strerror(errno));
    }
  }
#endif  // LCI_IBV_HAVE_TD
  if (ib_pd == nullptr) {
    ib_td = nullptr;
    ib_pd = p_net_context->ib_pd;
  }
}

ibv_device_impl_t::~ibv_device_impl_t()
{
#if LCI_IBV_HAVE_TD
  if (ib_td) {
    IBV_SAFECALL(ibv_dealloc_pd(ib_pd));
    IBV_SAFECALL(ibv_dealloc_td(ib_td));
    --g_td_num;
  }
#endif
  IBV_SAFECALL(ibv_destroy_cq(ib_cq));
#if LCI_IBV_HAVE_SRQ
  if (ib_srq) IBV_SAFECALL(ibv_destroy_srq(ib_srq));
#endif
  LCI_Assert(recv_qps.empty(), "Device destroyed with live per-QP receives\n");
  // Buffers left in the stash are still counted as posted by the device
  // layer and reported through unbind_packet_pool().
}

endpoint_t ibv_device_impl_t::alloc_endpoint_impl(endpoint_t::attr_t attr)
{
  endpoint_t ret;
  ret.p_impl = new ibv_endpoint_impl_t(device, attr);
  return ret;
}

mr_t ibv_device_impl_t::register_memory_impl(void* buffer, size_t size)
{
  ibv_mr_impl_t* mr;

  if (net_context_attr.ibv_odp_strategy ==
      attr_ibv_odp_strategy_t::implicit_odp) {
    // For implicit ODP, create a lightweight wrapper MR that points to the
    // shared ODP ibv_mr, so each registration has its own address/size
    // metadata while sharing the same lkey/rkey.
    mr = new ibv_mr_impl_t();
    mr->ibv_mr = odp_mr.ibv_mr;
  } else {
    mr = new ibv_mr_impl_t();
    int mr_flags;
    if (net_context_attr.ibv_odp_strategy ==
        attr_ibv_odp_strategy_t::explicit_odp) {
      mr_flags = IBV_ACCESS_ON_DEMAND | IBV_ACCESS_LOCAL_WRITE |
                 IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    } else {
      mr_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                 IBV_ACCESS_REMOTE_WRITE;
    }
    if (!net_context_attr.support_get) {
      // No RDMA read on this transport (UC); don't ask for it.
      mr_flags &= ~IBV_ACCESS_REMOTE_READ;
    }
#if (defined(LCI_USE_CUDA) || defined(LCI_USE_HIP)) && LCI_IBV_HAVE_DMABUF_MR
    mr->acc_attr = accelerator::get_buffer_attr(buffer);
    mr->dmabuf_fd = -1;
    mr->ibv_mr = nullptr;
    if (net_context.get_attr_use_dmabuf()) {
      size_t page_size = sysconf(_SC_PAGESIZE);
      size_t size_aligned;
      void* p;
      p = (void*)((uintptr_t)buffer & ~(page_size - 1));
      size_aligned =
          ((size + (uintptr_t)buffer - (uintptr_t)p + page_size - 1) /
           page_size) *
          page_size;
      if (mr->acc_attr.type == accelerator::buffer_type_t::DEVICE) {
        uint64_t dmabuf_offset = 0;
        mr->dmabuf_fd =
            accelerator::get_dmabuf_fd(p, size_aligned, &dmabuf_offset);
        if (mr->dmabuf_fd == -1) {
          LCI_Warn("Failed to get dmabuf fd\n");
        } else {
          mr->ibv_mr = ibv_reg_dmabuf_mr(ib_pd, dmabuf_offset, size_aligned,
                                         (uint64_t)p, mr->dmabuf_fd, mr_flags);
          if (mr->ibv_mr == nullptr) {
            LCI_Warn("Failed to register dmabuf mr. Errno: %d (%s)\n", errno,
                     strerror(errno));
            close(mr->dmabuf_fd);
          }
        }
        if (mr->ibv_mr == nullptr) {
          LCI_Warn("DMA-BUF registration failed. Fall back to ibv_reg_mr\n");
          net_context.get_impl()->attr.use_dmabuf = false;
        } else {
          LCI_Log(
              LOG_INFO, "mr",
              "DMA-BUF registration succeeded fd %d mr %p lkey %u rkey %u\n",
              mr->dmabuf_fd, (void*)mr->ibv_mr, mr->ibv_mr->lkey,
              mr->ibv_mr->rkey);
        }
      }
    }
    if (mr->ibv_mr == nullptr) {
#endif  // (LCI_USE_CUDA || LCI_USE_HIP) && LCI_IBV_HAVE_DMABUF_MR
      mr->ibv_mr = ibv_reg_mr(ib_pd, buffer, size, mr_flags);
#if (defined(LCI_USE_CUDA) || defined(LCI_USE_HIP)) && LCI_IBV_HAVE_DMABUF_MR
    }
#endif  // (LCI_USE_CUDA || LCI_USE_HIP) && LCI_IBV_HAVE_DMABUF_MR
    LCI_Assert(mr->ibv_mr,
               "register_memory_impl(%p, %lu): Failed to register memory.  "
               "Errno: %d (%s)",
               (void*)buffer, size, errno, strerror(errno));
#if LCI_IBV_HAVE_ADVISE_MR
    if (net_context_attr.ibv_prefetch_strategy !=
        attr_ibv_prefetch_strategy_t::none) {
      struct ibv_sge list;
      list.addr = (uintptr_t)buffer;
      list.length = size;
      list.lkey = ibv_detail::get_mr_lkey(mr);
      enum ibv_advise_mr_advice advice = IBV_ADVISE_MR_ADVICE_PREFETCH;
      switch (net_context_attr.ibv_prefetch_strategy) {
        case attr_ibv_prefetch_strategy_t::prefetch:
          advice = IBV_ADVISE_MR_ADVICE_PREFETCH;
          break;
        case attr_ibv_prefetch_strategy_t::prefetch_write:
          advice = IBV_ADVISE_MR_ADVICE_PREFETCH_WRITE;
          break;
        case attr_ibv_prefetch_strategy_t::prefetch_no_fault:
          advice = IBV_ADVISE_MR_ADVICE_PREFETCH_NO_FAULT;
          break;
        default:
          LCI_Assert(false, "Invalid prefetch strategy\n");
      }
      IBV_SAFECALL(ibv_advise_mr(ib_pd, advice, 0, &list, 1));
    }
#else
    if (net_context_attr.ibv_prefetch_strategy !=
        attr_ibv_prefetch_strategy_t::none) {
      static std::once_flag warned;
      std::call_once(warned, [] {
        LCI_Warn(
            "ibv_advise_mr is not supported by this libibverbs; ignoring "
            "ibv_prefetch_strategy\n");
      });
    }
#endif  // LCI_IBV_HAVE_ADVISE_MR
  }
  mr_t ret;
  ret.p_impl = mr;
  return ret;
}

void ibv_device_impl_t::deregister_memory_impl(mr_impl_t* mr_impl)
{
  if (net_context_attr.ibv_odp_strategy ==
      attr_ibv_odp_strategy_t::implicit_odp) {
    // In implicit ODP, the ibv_mr is shared globally; just free the wrapper.
    auto p_ibv_mr = static_cast<ibv_mr_impl_t*>(mr_impl);
    // Safety: do not delete the holder object if ever passed inadvertently.
    if (p_ibv_mr != &odp_mr) {
      delete p_ibv_mr;
    }
  } else {
    auto p_ibv_mr = static_cast<ibv_mr_impl_t*>(mr_impl);
    IBV_SAFECALL(ibv_dereg_mr(p_ibv_mr->ibv_mr));
#if (defined(LCI_USE_CUDA) || defined(LCI_USE_HIP)) && LCI_IBV_HAVE_DMABUF_MR
    if (p_ibv_mr->dmabuf_fd != -1) {
      close(p_ibv_mr->dmabuf_fd);
    }
#endif  // LCI_USE_CUDA || LCI_USE_HIP
    delete p_ibv_mr;
  }
}

/*************************************************************************************
 * Per-QP receive queues (used when shared receive queues are not available)
 * **********************************************************************************/

void ibv_device_impl_t::mark_hungry_locked(ibv_qp_recv_state_t* state)
{
  if (!state->in_hungry && state->posted.size() < recv_depth_per_qp) {
    hungry_qps.push_back(state);
    state->in_hungry = true;
  }
}

void ibv_device_impl_t::add_recv_qps_locked(
    std::vector<ibv_qp_recv_state_t>& states)
{
  for (auto& state : states) {
    recv_qps.push_back(&state);
    mark_hungry_locked(&state);
  }
  distribute_recvs_locked();
}

void ibv_device_impl_t::remove_recv_qps_locked(
    std::vector<ibv_qp_recv_state_t>& states)
{
  for (auto& state : states) {
    recv_qps.erase(std::remove(recv_qps.begin(), recv_qps.end(), &state),
                   recv_qps.end());
    hungry_qps.erase(std::remove(hungry_qps.begin(), hungry_qps.end(), &state),
                     hungry_qps.end());
    state.in_hungry = false;
    state.connected = false;
    state.credit_pending = false;
  }
}

void ibv_device_impl_t::distribute_recvs_locked()
{
  while (!recv_stash.empty() && !hungry_qps.empty()) {
    ibv_qp_recv_state_t* state = hungry_qps.front();
    size_t room = recv_depth_per_qp > state->posted.size()
                      ? recv_depth_per_qp - state->posted.size()
                      : 0;
    size_t n = std::min(room, recv_stash.size());
    bool qp_full = false;
    for (size_t k = 0; k < n; ++k) {
      const ibv_recv_item_t& item = recv_stash.front();
      struct ibv_sge sge;
      sge.addr = item.addr;
      sge.length = item.length;
      sge.lkey = item.lkey;
      struct ibv_recv_wr wr;
      wr.wr_id = item.wr_id;
      wr.next = nullptr;
      wr.sg_list = &sge;
      wr.num_sge = 1;
      struct ibv_recv_wr* bad_wr;
      int ret = ibv_post_recv(state->qp, &wr, &bad_wr);
      if (ret == ENOMEM) {
        // The hardware queue is smaller than requested.
        qp_full = true;
        break;
      }
      IBV_SAFECALL(ret);
      state->posted.push_back(item);
      ++state->total_posted;
      recv_stash.pop_front();
    }
    if (qp_full || state->posted.size() >= recv_depth_per_qp) {
      qp_full = true;
      hungry_qps.pop_front();
      state->in_hungry = false;
    }
    // Advertise eagerly when the QP is full or we have run out of buffers to
    // post; otherwise, batch the credit updates.
    advertise_credits_locked(state, qp_full || recv_stash.empty());
  }
}

void ibv_device_impl_t::advertise_credits_locked(ibv_qp_recv_state_t* state,
                                                 bool force)
{
  if (!use_credits) return;
  uint64_t pending = state->total_posted - state->advertised;
  if (pending == 0) {
    state->credit_pending = false;
    return;
  }
  if (!force && pending < credit_batch) return;
  if (!state->connected ||
      !state->endpoint->post_credit_update(state->rank, state->total_posted)) {
    state->credit_pending = true;
    credit_pending_any.store(true, std::memory_order_relaxed);
    return;
  }
  state->advertised = state->total_posted;
  state->credit_pending = false;
}

void ibv_device_impl_t::flush_pending_credits_locked()
{
  if (!credit_pending_any.load(std::memory_order_relaxed)) return;
  credit_pending_any.store(false, std::memory_order_relaxed);
  for (auto* state : recv_qps) {
    if (state->credit_pending) {
      advertise_credits_locked(state, true);
    }
  }
}

/*************************************************************************************
 * Endpoint
 * **********************************************************************************/

ibv_endpoint_impl_t::ibv_endpoint_impl_t(device_t device_, attr_t attr_)
    : endpoint_impl_t(device_, attr_),
      p_ibv_device(reinterpret_cast<ibv_device_impl_t*>(device.p_impl))
{
  ibv_net_context_impl_t* p_net_context =
      static_cast<ibv_net_context_impl_t*>(p_ibv_device->net_context.p_impl);
  const int nranks = get_rank_n();
  const bool use_srq = p_ibv_device->use_srq;
  const bool use_credits = p_ibv_device->use_credits;
  const bool is_uc = p_net_context->qp_type == IBV_QPT_UC;

  ib_qps.resize(nranks);
  qp_remaining_slots = std::vector<padded_atomic_t<int>>(nranks);
  for (auto& slot : qp_remaining_slots) {
    slot.val.store(static_cast<int>(device_attr.net_max_sends),
                   std::memory_order_relaxed);
  }

#if LCI_IBV_HAVE_TD
  if (device_attr.ibv_td_strategy == attr_ibv_td_strategy_t::per_qp) {
    ib_qp_extras.resize(nranks);
    for (int i = 0; i < nranks; ++i) {
      ib_qp_extras[i].ib_pd = nullptr;
      struct ibv_td_init_attr td_attr;
      td_attr.comp_mask = 0;
      ib_qp_extras[i].ib_td = ibv_alloc_td(p_net_context->ib_context, &td_attr);
      ++g_td_num;
      if (ib_qp_extras[i].ib_td != nullptr) {
        struct ibv_parent_domain_init_attr pd_attr;
        pd_attr.td = ib_qp_extras[i].ib_td;
        pd_attr.pd = p_net_context->ib_pd;
        pd_attr.comp_mask = 0;
        ib_qp_extras[i].ib_pd =
            ibv_alloc_parent_domain(p_net_context->ib_context, &pd_attr);
        if (ib_qp_extras[i].ib_pd == nullptr) {
          LCI_Warn(
              "ibv_alloc_parent_domain() failed (%s); deallocating the thread "
              "domain\n",
              strerror(errno));
          IBV_SAFECALL(ibv_dealloc_td(ib_qp_extras[i].ib_td));
          ib_qp_extras[i].ib_td = nullptr;
          --g_td_num;
        }
      } else {
        LCI_Warn(
            "ibv_alloc_td() failed (No. %lu, %s). Use `export "
            "LCI_ATTR_IBV_TD_STRATEGY=none` to suppress this warning\n",
            g_td_num, strerror(errno));
      }
      if (ib_qp_extras[i].ib_pd == nullptr) {
        ib_qp_extras[i].ib_td = nullptr;
        ib_qp_extras[i].ib_pd = p_net_context->ib_pd;
      }
    }
  }
#endif  // LCI_IBV_HAVE_TD

  // Credit memory: peers RDMA-write their cumulative posted-receive counts
  // into credit_limits[peer_rank].
  if (use_credits) {
    credit_limits = static_cast<uint64_t*>(
        alloc_memalign(sizeof(uint64_t) * nranks, LCI_CACHE_LINE));
    memset(credit_limits, 0, sizeof(uint64_t) * nranks);
    credit_mr = ibv_reg_mr(p_net_context->ib_pd, credit_limits,
                           sizeof(uint64_t) * nranks,
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    LCI_Assert(credit_mr, "Couldn't register the credit array (%s)\n",
               strerror(errno));
    credits_consumed = std::vector<padded_atomic_t<uint64_t>>(nranks);
    for (auto& c : credits_consumed) c.val.store(0, std::memory_order_relaxed);
    peer_credit_addr.resize(nranks, 0);
    peer_credit_rkey.resize(nranks, 0);
  }

  // Create QPs
  for (int i = 0; i < nranks; ++i) {
    struct ibv_qp_init_attr init_attr;
    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.send_cq = p_ibv_device->ib_cq;
    init_attr.recv_cq = p_ibv_device->ib_cq;
    init_attr.srq = use_srq ? p_ibv_device->ib_srq : nullptr;
    init_attr.cap.max_send_wr = device_attr.net_max_sends;
    init_attr.cap.max_recv_wr =
        use_srq ? device_attr.net_max_recvs : p_ibv_device->recv_depth_per_qp;
    init_attr.cap.max_send_sge = max_sge_num;
    init_attr.cap.max_recv_sge = max_sge_num;
    init_attr.cap.max_inline_data = net_context_attr.max_inject_size;
    init_attr.qp_type = p_net_context->qp_type;
    init_attr.sq_sig_all = 0;
    struct ibv_pd* pd = p_ibv_device->ib_pd;
    if (!ib_qp_extras.empty()) {
      pd = ib_qp_extras[i].ib_pd;
    }
    ib_qps[i] = ibv_create_qp(pd, &init_attr);
    ++g_qp_num;
    LCI_Assert(ib_qps[i], "Couldn't create %s QP %lu (%s)\n",
               is_uc ? "UC" : "RC", g_qp_num, strerror(errno));

    struct ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    ibv_query_qp(ib_qps[i], &qp_attr, IBV_QP_CAP, &init_attr);
    LCI_Assert(qp_attr.cap.max_inline_data >= net_context_attr.max_inject_size,
               "Specified inline size %d is too large (maximum %d)",
               net_context_attr.max_inject_size, init_attr.cap.max_inline_data);
    if (qp_attr.cap.max_inline_data > net_context_attr.max_inject_size) {
      LCI_Log(LOG_INFO, "ibv",
              "Maximum inline-size(%d) > requested inline-size(%d)\n",
              qp_attr.cap.max_inline_data, net_context_attr.max_inject_size);
    }
  }
  if (!use_srq) {
    recv_states.resize(nranks);
    for (int i = 0; i < nranks; ++i) {
      recv_states[i].qp = ib_qps[i];
      recv_states[i].endpoint = this;
      recv_states[i].rank = i;
    }
  }
  p_ibv_device->qp2rank_map.add_qps(ib_qps, &qp_remaining_slots,
                                    use_srq ? nullptr : &recv_states);

  // Modify QP states to INIT
  struct bootstrap_data_t {
    int source_rank;
    int target_rank;
    int uid;
    uint32_t qp_num;
    uint16_t lid;
    char wgid[ibv_detail::WIRE_GID_NBYTES + 1];
    // Credit flow control (UC only).
    uint64_t init_credits;
    uint64_t credit_addr;
    uint32_t credit_rkey;
  };
  std::vector<bootstrap_data_t> bootstrap_datav_in(nranks);
  for (int i = 0; i < nranks; ++i) {
    struct ibv_qp_attr mod_attr;
    memset(&mod_attr, 0, sizeof(mod_attr));
    mod_attr.qp_state = IBV_QPS_INIT;
    mod_attr.qp_access_flags =
        is_uc ? IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE
              : IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                    IBV_ACCESS_REMOTE_WRITE;
    mod_attr.pkey_index = 0;
    mod_attr.port_num = p_net_context->ib_dev_port;

    int flags =
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    int rc = ibv_modify_qp(ib_qps[i], &mod_attr, flags);
    LCI_Assert(rc == 0, "Failed to modify QP to INIT (%s)\n", strerror(rc));

    char wgid[ibv_detail::WIRE_GID_NBYTES + 1];
    memset(wgid, 0, sizeof(wgid));
    ibv_detail::gid_to_wire_gid(&p_net_context->ib_gid, wgid);

    bootstrap_data_t data;
    memset(&data, 0, sizeof(data));
    data.source_rank = get_rank_me();
    data.target_rank = i;
    data.uid = this->attr.uid;
    data.qp_num = ib_qps[i]->qp_num;
    data.lid = p_net_context->ib_port_attr.lid;
    memcpy(data.wgid, wgid, sizeof(wgid));
    if (use_credits) {
      data.credit_addr = reinterpret_cast<uint64_t>(credit_limits);
      data.credit_rkey = credit_mr->rkey;
    }
    bootstrap_datav_in[i] = data;
  }

  // Receives may be posted once the QPs are in INIT. Fill the per-QP receive
  // queues from the device stash and record the initial credits.
  if (!use_srq) {
    std::lock_guard<spinlock_t> lock(p_ibv_device->srq_lock);
    p_ibv_device->add_recv_qps_locked(recv_states);
    for (int i = 0; i < nranks; ++i) {
      recv_states[i].advertised = recv_states[i].total_posted;
      recv_states[i].credit_pending = false;
      bootstrap_datav_in[i].init_credits = recv_states[i].total_posted;
    }
  }

  std::vector<bootstrap_data_t> bootstrap_datav_out(nranks);
  bootstrap::alltoall(bootstrap_datav_in.data(), bootstrap_datav_out.data(),
                      sizeof(bootstrap_data_t));
  // Modify QP states to RTR and RTS
  for (int i = 0; i < nranks; ++i) {
    bootstrap_data_t data = bootstrap_datav_out[i];
    LCI_Assert(data.source_rank == i,
               "Unexpected source rank %d, expected %d\n", data.source_rank, i);
    LCI_Assert(data.target_rank == get_rank_me(),
               "Unexpected target rank %d, expected %d\n", data.target_rank,
               get_rank_me());
    LCI_Assert(data.uid == this->attr.uid, "Unexpected uid %d, expected %d\n",
               data.uid, this->attr.uid);
    uint32_t dest_qpn = data.qp_num;
    uint16_t dest_lid = data.lid;
    union ibv_gid gid;
    ibv_detail::wire_gid_to_gid(data.wgid, &gid);

    if (use_credits) {
      // Nobody writes our credit array before the post-connection barrier.
      credit_limits[i] = data.init_credits;
      peer_credit_addr[i] =
          data.credit_addr +
          sizeof(uint64_t) * static_cast<uint64_t>(get_rank_me());
      peer_credit_rkey[i] = data.credit_rkey;
    }

    struct ibv_qp_attr mod_attr;
    memset(&mod_attr, 0, sizeof(mod_attr));
    mod_attr.qp_state = IBV_QPS_RTR;
    mod_attr.path_mtu = p_net_context->ib_port_attr.active_mtu;
    mod_attr.rq_psn = 0;
    mod_attr.dest_qp_num = dest_qpn;
    mod_attr.ah_attr.dlid = dest_lid;
    mod_attr.ah_attr.sl = 0;
    mod_attr.ah_attr.src_path_bits = 0;
    mod_attr.ah_attr.is_global = 0;
    mod_attr.ah_attr.static_rate = 0;
    mod_attr.ah_attr.port_num = p_net_context->ib_dev_port;
    memset(&mod_attr.ah_attr.grh, 0, sizeof mod_attr.ah_attr.grh);
    if (gid.global.interface_id) {
      mod_attr.ah_attr.is_global = 1;
      mod_attr.ah_attr.grh.hop_limit = 1;
      mod_attr.ah_attr.grh.dgid = gid;
      mod_attr.ah_attr.grh.sgid_index = net_context_attr.ibv_gid_idx;
    }

    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                IBV_QP_RQ_PSN;
    if (!is_uc) {
      mod_attr.max_dest_rd_atomic = 1;
      mod_attr.min_rnr_timer = 12;
      flags |= IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    }

    int rc = ibv_modify_qp(ib_qps[i], &mod_attr, flags);
    LCI_Assert(rc == 0, "Failed to modify QP to RTR (%s)\n", strerror(rc));

    memset(&mod_attr, 0, sizeof(mod_attr));
    mod_attr.qp_state = IBV_QPS_RTS;
    mod_attr.sq_psn = 0;
    flags = IBV_QP_STATE | IBV_QP_SQ_PSN;
    if (!is_uc) {
      mod_attr.max_rd_atomic = 1;
      mod_attr.timeout = 18;
      mod_attr.retry_cnt = 7;
      mod_attr.rnr_retry = 7;
      flags |= IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
               IBV_QP_MAX_QP_RD_ATOMIC;
    }
    rc = ibv_modify_qp(ib_qps[i], &mod_attr, flags);
    LCI_Assert(rc == 0, "Failed to modify QP to RTS (%s)\n", strerror(rc));
  }

  if (is_uc) {
    // UC has no retransmission: a message that arrives before the peer's QP
    // reaches RTR is silently dropped. Make sure every peer is connected
    // before anyone sends.
    int dummy = 0;
    std::vector<int> dummies(nranks);
    bootstrap::allgather(&dummy, dummies.data(), sizeof(int));
  }

  if (!use_srq) {
    std::lock_guard<spinlock_t> lock(p_ibv_device->srq_lock);
    for (auto& state : recv_states) {
      state.connected = true;
      p_ibv_device->advertise_credits_locked(&state, true);
    }
  }
}

ibv_endpoint_impl_t::~ibv_endpoint_impl_t()
{
  if (!recv_states.empty()) {
    // Stop posting receives to these QPs.
    std::lock_guard<spinlock_t> lock(p_ibv_device->srq_lock);
    p_ibv_device->remove_recv_qps_locked(recv_states);
  }
  p_ibv_device->qp2rank_map.remove_qps(ib_qps);
  for (auto* qp : ib_qps) {
    if (qp != nullptr) {
      IBV_SAFECALL(ibv_destroy_qp(qp));
      --g_qp_num;
    }
  }
  if (!recv_states.empty()) {
    // The QPs are gone, so their posted receive buffers are free again.
    // Return them to the device stash so other endpoints can use them.
    std::lock_guard<spinlock_t> lock(p_ibv_device->srq_lock);
    for (auto& state : recv_states) {
      for (auto it = state.posted.rbegin(); it != state.posted.rend(); ++it) {
        p_ibv_device->recv_stash.push_front(*it);
      }
      state.posted.clear();
    }
    p_ibv_device->distribute_recvs_locked();
  }
  if (credit_mr) {
    IBV_SAFECALL(ibv_dereg_mr(credit_mr));
    credit_mr = nullptr;
  }
  if (credit_limits) {
    free(credit_limits);
    credit_limits = nullptr;
  }
#if LCI_IBV_HAVE_TD
  if (!ib_qp_extras.empty()) {
    for (auto& extra : ib_qp_extras) {
      if (extra.ib_td) {
        IBV_SAFECALL(ibv_dealloc_pd(extra.ib_pd));
        IBV_SAFECALL(ibv_dealloc_td(extra.ib_td));
        --g_td_num;
      }
    }
  }
#endif
}

bool ibv_endpoint_impl_t::post_credit_update(int rank, uint64_t value)
{
  if (!try_acquire_slot(rank, true)) return false;
  if (!try_lock_qp(rank)) {
    release_slot(rank);
    return false;
  }
  struct ibv_sge sge;
  sge.addr = reinterpret_cast<uint64_t>(&value);
  sge.length = sizeof(value);
  sge.lkey = 0;
  struct ibv_send_wr wr;
  memset(&wr, 0, sizeof(wr));
  wr.wr_id = ibv_detail::credit_wr_id;
  wr.next = nullptr;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_WRITE;
  wr.send_flags = IBV_SEND_INLINE | IBV_SEND_SIGNALED;
  wr.wr.rdma.remote_addr = peer_credit_addr[rank];
  wr.wr.rdma.rkey = peer_credit_rkey[rank];
  struct ibv_send_wr* bad_wr;
  int ret = ibv_post_send(ib_qps[rank], &wr, &bad_wr);
  unlock_qp(rank);
  if (ret == 0) return true;
  release_slot(rank);
  if (ret != ENOMEM) IBV_SAFECALL(ret);
  return false;
}

}  // namespace lci
