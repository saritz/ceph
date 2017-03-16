// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 XSKY <haomai@xsky.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "Infiniband.h"
#include "RDMAStack.h"
#include "Device.h"
#include "common/errno.h"
#include "common/debug.h"

#include <poll.h>

#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix *_dout << "IBDevice "

static const uint32_t MAX_SHARED_RX_SGE_COUNT = 1;
static const uint32_t CQ_DEPTH = 30000;

Port::Port(CephContext *cct, struct ibv_context* ictxt, uint8_t ipn): ctxt(ictxt), port_num(ipn), port_attr(new ibv_port_attr)
{
#ifdef HAVE_IBV_EXP
  union ibv_gid cgid;
  struct ibv_exp_gid_attr gid_attr;
  bool malformed = false;

  ldout(cct,1) << __func__ << " using experimental verbs for gid" << dendl;
  int r = ibv_query_port(ctxt, port_num, port_attr);
  if (r == -1) {
    lderr(cct) << __func__  << " query port failed  " << cpp_strerror(errno) << dendl;
    ceph_abort();
  }

  lid = port_attr->lid;

  // search for requested GID in GIDs table
  ldout(cct, 1) << __func__ << " looking for local GID " << (cct->_conf->ms_async_rdma_local_gid)
    << " of type " << (cct->_conf->ms_async_rdma_roce_ver) << dendl;
  r = sscanf(cct->_conf->ms_async_rdma_local_gid.c_str(),
	     "%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx"
	     ":%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx:%02hhx%02hhx",
	     &cgid.raw[ 0], &cgid.raw[ 1],
	     &cgid.raw[ 2], &cgid.raw[ 3],
	     &cgid.raw[ 4], &cgid.raw[ 5],
	     &cgid.raw[ 6], &cgid.raw[ 7],
	     &cgid.raw[ 8], &cgid.raw[ 9],
	     &cgid.raw[10], &cgid.raw[11],
	     &cgid.raw[12], &cgid.raw[13],
	     &cgid.raw[14], &cgid.raw[15]);

  if (r != 16) {
    ldout(cct, 1) << __func__ << " malformed or no GID supplied, using GID index 0" << dendl;
    malformed = true;
  }

  gid_attr.comp_mask = IBV_EXP_QUERY_GID_ATTR_TYPE;

  for (gid_idx = 0; gid_idx < port_attr->gid_tbl_len; gid_idx++) {
    r = ibv_query_gid(ctxt, port_num, gid_idx, &gid);
    if (r) {
      lderr(cct) << __func__  << " query gid of port " << port_num << " index " << gid_idx << " failed  " << cpp_strerror(errno) << dendl;
      ceph_abort();
    }
    r = ibv_exp_query_gid_attr(ctxt, port_num, gid_idx, &gid_attr);
    if (r) {
      lderr(cct) << __func__  << " query gid attributes of port " << port_num << " index " << gid_idx << " failed  " << cpp_strerror(errno) << dendl;
      ceph_abort();
    }

    if (malformed) break; // stay with gid_idx=0
    if ( (gid_attr.type == cct->_conf->ms_async_rdma_roce_ver) &&
	 (memcmp(&gid, &cgid, 16) == 0) ) {
      ldout(cct, 1) << __func__ << " found at index " << gid_idx << dendl;
      break;
    }
  }

  if (gid_idx == port_attr->gid_tbl_len) {
    lderr(cct) << __func__ << " Requested local GID was not found in GID table" << dendl;
    ceph_abort();
  }
#else
  int r = ibv_query_port(ctxt, port_num, port_attr);
  if (r == -1) {
    lderr(cct) << __func__  << " query port failed  " << cpp_strerror(errno) << dendl;
    ceph_abort();
  }

  lid = port_attr->lid;
  r = ibv_query_gid(ctxt, port_num, 0, &gid);
  if (r) {
    lderr(cct) << __func__  << " query gid failed  " << cpp_strerror(errno) << dendl;
    ceph_abort();
  }
#endif
}


Device::Device(CephContext *cct, ibv_device* d)
  : cct(cct), device(d), lock("ibdev_lock"),
    device_attr(new ibv_device_attr), active_port(nullptr)
{
  if (device == NULL) {
    lderr(cct) << __func__ << " device == NULL" << cpp_strerror(errno) << dendl;
    ceph_abort();
  }
  name = ibv_get_device_name(device);
  ctxt = ibv_open_device(device);
  if (ctxt == NULL) {
    lderr(cct) << __func__ << " open rdma device failed. " << cpp_strerror(errno) << dendl;
    ceph_abort();
  }
  int r = ibv_query_device(ctxt, device_attr);
  if (r == -1) {
    lderr(cct) << __func__ << " failed to query rdma device. " << cpp_strerror(errno) << dendl;
    ceph_abort();
  }

  tx_cc = create_comp_channel(cct);
  assert(tx_cc);

  rx_cc = create_comp_channel(cct);
  assert(rx_cc);
}

void Device::init()
{
  Mutex::Locker l(lock);

  if (initialized)
    return;

  int r = ibv_query_device(ctxt, device_attr);
  if (r == -1) {
    lderr(cct) << __func__ << " failed to query rdma device. " << cpp_strerror(errno) << dendl;
    ceph_abort();
  }

  pd = new ProtectionDomain(cct, this);

  assert(NetHandler(cct).set_nonblock(ctxt->async_fd) == 0);

  max_recv_wr = std::min(device_attr->max_srq_wr, (int)cct->_conf->ms_async_rdma_receive_buffers);
  ldout(cct, 1) << __func__ << " assigning: " << max_recv_wr << " receive buffers" << dendl;

  max_send_wr = std::min(device_attr->max_qp_wr, (int)cct->_conf->ms_async_rdma_send_buffers);
  ldout(cct, 1) << __func__ << " assigning: " << max_send_wr << " send buffers"  << dendl;

  ldout(cct, 1) << __func__ << " device allow " << device_attr->max_cqe
                << " completion entries" << dendl;

  memory_manager = new MemoryManager(this, pd,
                                     cct->_conf->ms_async_rdma_enable_hugepage);
  memory_manager->register_rx_tx(
      cct->_conf->ms_async_rdma_buffer_size, max_recv_wr, max_send_wr);

  srq = create_shared_receive_queue(max_recv_wr, MAX_SHARED_RX_SGE_COUNT);
  post_channel_cluster();

  tx_cq = create_comp_queue(cct, tx_cc);
  assert(tx_cq);

  rx_cq = create_comp_queue(cct, rx_cc);
  assert(rx_cq);

  initialized = true;
  ldout(cct, 5) << __func__ << ":" << __LINE__ << " device " << *this << " is initialized" << dendl;
}

void Device::uninit()
{
  if (!initialized)
    return;

  tx_cc->ack_events();
  rx_cc->ack_events();

  initialized = false;

  delete rx_cq;
  delete tx_cq;
  delete rx_cc;
  delete tx_cc;

  delete srq;
  delete memory_manager;
  delete pd;
}

Device::~Device()
{
  assert(ibv_destroy_srq(srq) == 0);
  delete memory_manager;
  delete pd;

  if (active_port) {
    delete active_port;
    assert(ibv_close_device(ctxt) == 0);
  }
}

void Device::binding_port(CephContext *cct, int port_num) {
  port_cnt = device_attr->phys_port_cnt;
  for (uint8_t i = 0; i < port_cnt; ++i) {
    Port *port = new Port(cct, ctxt, i+1);
    if (i + 1 == port_num && port->get_port_attr()->state == IBV_PORT_ACTIVE) {
      active_port = port;
      ldout(cct, 1) << __func__ << " found active port " << i+1 << dendl;
      break;
    } else {
      ldout(cct, 10) << __func__ << " port " << i+1 << " is not what we want. state: " << port->get_port_attr()->state << ")"<< dendl;
    }
    delete port;
  }
  if (nullptr == active_port) {
    lderr(cct) << __func__ << "  port not found" << dendl;
    assert(active_port);
  }
}

/**
 * Create a new QueuePair. This factory should be used in preference to
 * the QueuePair constructor directly, since this lets derivatives of
 * Infiniband, e.g. MockInfiniband (if it existed),
 * return mocked out QueuePair derivatives.
 *
 * \return
 *      QueuePair on success or NULL if init fails
 * See QueuePair::QueuePair for parameter documentation.
 */
Infiniband::QueuePair* Device::create_queue_pair(CephContext *cct,
						 ibv_qp_type type)
{
  Infiniband::QueuePair *qp = new QueuePair(
      cct, *this, type, active_port->get_port_num(), srq, tx_cq, rx_cq, max_send_wr, max_recv_wr);
  if (qp->init()) {
    delete qp;
    return NULL;
  }
  return qp;
}

/**
 * Create a shared receive queue. This basically wraps the verbs call.
 *
 * \param[in] max_wr
 *      The max number of outstanding work requests in the SRQ.
 * \param[in] max_sge
 *      The max number of scatter elements per WR.
 * \return
 *      A valid ibv_srq pointer, or NULL on error.
 */
ibv_srq* Device::create_shared_receive_queue(uint32_t max_wr, uint32_t max_sge)
{
  ibv_srq_init_attr sia;
  memset(&sia, 0, sizeof(sia));
  sia.srq_context = ctxt;
  sia.attr.max_wr = max_wr;
  sia.attr.max_sge = max_sge;
  return ibv_create_srq(pd->pd, &sia);
}

Infiniband::CompletionChannel* Device::create_comp_channel(CephContext *c)
{
  Infiniband::CompletionChannel *cc = new Infiniband::CompletionChannel(c, *this);
  if (cc->init()) {
    delete cc;
    return NULL;
  }
  return cc;
}

Infiniband::CompletionQueue* Device::create_comp_queue(
    CephContext *cct, CompletionChannel *cc)
{
  Infiniband::CompletionQueue *cq = new Infiniband::CompletionQueue(
      cct, *this, CQ_DEPTH, cc);
  if (cq->init()) {
    delete cq;
    return NULL;
  }
  return cq;
}

int Device::post_chunk(Chunk* chunk)
{
  ibv_sge isge;
  isge.addr = reinterpret_cast<uint64_t>(chunk->buffer);
  isge.length = chunk->bytes;
  isge.lkey = chunk->mr->lkey;
  ibv_recv_wr rx_work_request;

  memset(&rx_work_request, 0, sizeof(rx_work_request));
  rx_work_request.wr_id = reinterpret_cast<uint64_t>(chunk);// stash descriptor ptr
  rx_work_request.next = NULL;
  rx_work_request.sg_list = &isge;
  rx_work_request.num_sge = 1;

  ibv_recv_wr *badWorkRequest;
  int ret = ibv_post_srq_recv(srq, &rx_work_request, &badWorkRequest);
  if (ret)
    return -errno;
  return 0;
}

int Device::post_channel_cluster()
{
  vector<Chunk*> free_chunks;
  int r = memory_manager->get_channel_buffers(free_chunks, 0);
  assert(r > 0);
  for (vector<Chunk*>::iterator iter = free_chunks.begin(); iter != free_chunks.end(); ++iter) {
    r = post_chunk(*iter);
    assert(r == 0);
  }
  return 0;
}

int Device::get_tx_buffers(std::vector<Chunk*> &c, size_t bytes)
{
  return memory_manager->get_send_buffers(c, bytes);
}

int Device::poll_tx_cq(int n, ibv_wc *wc)
{
  if (!initialized)
    return 0;

  return tx_cq->poll_cq(n, wc);
}

int Device::poll_rx_cq(int n, ibv_wc *wc)
{
  if (!initialized)
    return 0;

  return rx_cq->poll_cq(n, wc);
}

void Device::rearm_cqs()
{
  int ret;

  if (!initialized)
    return;

  ret = tx_cq->rearm_notify();
  assert(!ret);

  ret = rx_cq->rearm_notify();
  assert(!ret);
}


DeviceList::DeviceList(CephContext *cct)
  : cct(cct), device_list(ibv_get_device_list(&num))
{
  if (device_list == NULL || num == 0) {
    lderr(cct) << __func__ << " failed to get rdma device list.  " << cpp_strerror(errno) << dendl;
    ceph_abort();
  }
  devices = new Device*[num];

  poll_fds = new struct pollfd[2 * num];

  for (int i = 0; i < num; ++i) {
    struct pollfd *pfd = &poll_fds[i * 2];
    struct Device *d;

    d = new Device(cct, device_list[i]);
    devices[i] = d;

    pfd[0].fd = d->tx_cc->get_fd();
    pfd[0].events = POLLIN | POLLERR | POLLNVAL | POLLHUP;
    pfd[0].revents = 0;

    pfd[1].fd = d->rx_cc->get_fd();
    pfd[1].events = POLLIN | POLLERR | POLLNVAL | POLLHUP;
    pfd[1].revents = 0;
  }
}

DeviceList::~DeviceList()
{
  delete poll_fds;

  for (int i=0; i < num; ++i) {
    delete devices[i];
  }
  delete []devices;
  ibv_free_device_list(device_list);
}

Device* DeviceList::get_device(const char* device_name)
{
  assert(devices);
  for (int i = 0; i < num; ++i) {
    if (!strlen(device_name) || !strcmp(device_name, devices[i]->get_name())) {
      return devices[i];
    }
  }
  return NULL;
}

int DeviceList::poll_tx(int num_entries, Device **d, ibv_wc *wc)
{
  int n = 0;

  for (int i = 0; i < num; i++) {
    *d = devices[++last_poll_dev % num];

    n = (*d)->poll_tx_cq(num_entries, wc);
    if (n)
      break;
  }

  return n;
}

int DeviceList::poll_rx(int num_entries, Device **d, ibv_wc *wc)
{
  int n = 0;

  for (int i = 0; i < num; i++) {
    *d = devices[++last_poll_dev % num];

    n = (*d)->poll_rx_cq(num_entries, wc);
    if (n)
      break;
  }

  return n;
}

int DeviceList::poll_blocking(bool &done)
{
  int r = 0;
  while (!done && r == 0) {
    r = poll(poll_fds, num * 2, 1);
    if (r < 0) {
      r = -errno;
      lderr(cct) << __func__ << " poll failed " << r << dendl;
      ceph_abort();
    }
  }
 
  if (r <= 0)
    return r;

  for (int i = 0; i < num ; i++) {
    Device *d = devices[i];

    if (d->tx_cc->get_cq_event())
      ldout(cct, 20) << __func__ << " " << *d << ": got tx cq event" << dendl;

    if (d->rx_cc->get_cq_event())
      ldout(cct, 20) << __func__ << " " << *d << ": got rx cq event" << dendl;
  }

  return r;
}

void DeviceList::rearm_notify()
{
  for (int i = 0; i < num; i++)
    devices[i]->rearm_cqs();
}
