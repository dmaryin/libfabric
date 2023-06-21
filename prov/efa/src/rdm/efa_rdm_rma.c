/*
 * Copyright (c) 2019-2020 Amazon.com, Inc. or its affiliates.
 * All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <ofi_mem.h>
#include <ofi_iov.h>
#include "efa.h"

#include "efa_rdm_msg.h"
#include "efa_rdm_rma.h"
#include "rxr_pkt_cmd.h"
#include "efa_cntr.h"

int efa_rdm_rma_verified_copy_iov(struct efa_rdm_ep *ep, struct efa_rma_iov *rma,
			      size_t count, uint32_t flags,
			      struct iovec *iov, void **desc)
{
	void *context;
	struct efa_mr *efa_mr;
	int i, ret;

	for (i = 0; i < count; i++) {
		ofi_genlock_lock(&efa_rdm_ep_domain(ep)->util_domain.lock);
		ret = ofi_mr_map_verify(&efa_rdm_ep_domain(ep)->util_domain.mr_map,
					(uintptr_t *)(&rma[i].addr),
					rma[i].len, rma[i].key, flags,
					&context);
		efa_mr = context;
		desc[i] = fi_mr_desc(&efa_mr->mr_fid);
		ofi_genlock_unlock(&efa_rdm_ep_domain(ep)->util_domain.lock);
		if (ret) {
			EFA_WARN(FI_LOG_EP_CTRL,
				"MR verification failed (%s), addr: %lx key: %ld\n",
				fi_strerror(-ret), rma[i].addr, rma[i].key);
			return -FI_EACCES;
		}

		iov[i].iov_base = (void *)rma[i].addr;
		iov[i].iov_len = rma[i].len;
	}
	return 0;
}


struct efa_rdm_ope *
efa_rdm_rma_alloc_txe(struct efa_rdm_ep *efa_rdm_ep,
		       const struct fi_msg_rma *msg_rma,
		       uint32_t op,
		       uint64_t flags)
{
	struct efa_rdm_ope *txe;
	struct fi_msg msg;

	txe = ofi_buf_alloc(efa_rdm_ep->ope_pool);
	if (OFI_UNLIKELY(!txe)) {
		EFA_DBG(FI_LOG_EP_CTRL, "TX entries exhausted.\n");
		return NULL;
	}

	msg.addr = msg_rma->addr;
	msg.msg_iov = msg_rma->msg_iov;
	msg.context = msg_rma->context;
	msg.iov_count = msg_rma->iov_count;
	msg.data = msg_rma->data;
	msg.desc = msg_rma->desc;
	efa_rdm_txe_construct(txe, efa_rdm_ep, &msg, op, flags);

	assert(msg_rma->rma_iov_count > 0);
	assert(msg_rma->rma_iov);
	txe->rma_iov_count = msg_rma->rma_iov_count;
	memcpy(txe->rma_iov, msg_rma->rma_iov,
	       sizeof(struct fi_rma_iov) * msg_rma->rma_iov_count);

	dlist_insert_tail(&txe->ep_entry, &efa_rdm_ep->txe_list);
	return txe;
}

/* rma_read functions */
ssize_t efa_rdm_rma_post_efa_emulated_read(struct efa_rdm_ep *ep, struct efa_rdm_ope *txe)
{
	int err;

#if ENABLE_DEBUG
	dlist_insert_tail(&txe->pending_recv_entry,
			  &ep->ope_recv_list);
	ep->pending_recv_counter++;
#endif

	if (txe->total_len < ep->mtu_size - sizeof(struct rxr_readrsp_hdr)) {
		err = rxr_pkt_post(ep, txe, RXR_SHORT_RTR_PKT);
	} else {
		assert(efa_env.tx_min_credits > 0);
		txe->window = MIN(txe->total_len,
				       efa_env.tx_min_credits * ep->max_data_payload_size);
		err = rxr_pkt_post(ep, txe, RXR_LONGCTS_RTR_PKT);
	}

	if (OFI_UNLIKELY(err)) {
#if ENABLE_DEBUG
		dlist_remove(&txe->pending_recv_entry);
		ep->pending_recv_counter--;
#endif
	}

	return err;
}

ssize_t efa_rdm_rma_readmsg(struct fid_ep *ep, const struct fi_msg_rma *msg, uint64_t flags)
{
	ssize_t err;
	struct efa_rdm_ep *efa_rdm_ep;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ope *txe = NULL;
	fi_addr_t tmp_addr;
	struct fi_msg_rma *msg_clone;
	bool use_lower_ep_read;
	void *shm_desc[RXR_IOV_LIMIT];
	void **tmp_desc;
	struct util_srx_ctx *srx_ctx;

	EFA_DBG(FI_LOG_EP_DATA,
	       "read iov_len: %lu flags: %lx\n",
	       ofi_total_iov_len(msg->msg_iov, msg->iov_count),
	       flags);

	efa_rdm_ep = container_of(ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
	srx_ctx = efa_rdm_ep_get_peer_srx_ctx(efa_rdm_ep);
	err = efa_rdm_ep_cap_check_rma(efa_rdm_ep);
	if (err)
		return err;

	assert(msg->iov_count <= efa_rdm_ep->tx_iov_limit);

	efa_perfset_start(efa_rdm_ep, perf_efa_tx);
	ofi_genlock_lock(srx_ctx->lock);

	peer = efa_rdm_ep_get_peer(efa_rdm_ep, msg->addr);
	assert(peer);

	if (peer->flags & EFA_RDM_PEER_IN_BACKOFF) {
		err = -FI_EAGAIN;
		goto out;
	}

	if (peer->is_local && efa_rdm_ep->use_shm_for_tx) {
		tmp_addr = msg->addr;
		tmp_desc = msg->desc;
		msg_clone = (struct fi_msg_rma *)msg;
		msg_clone->addr = peer->shm_fiaddr;
		if (msg->desc) {
			efa_rdm_get_desc_for_shm(msg->iov_count, msg->desc, shm_desc);
			msg_clone->desc = shm_desc;
		} else {
			msg_clone->desc = NULL;
		}
		err = fi_readmsg(efa_rdm_ep->shm_ep, msg_clone, flags);
		msg_clone->addr = tmp_addr;
		msg_clone->desc = tmp_desc;
		goto out;
	}

	txe = efa_rdm_rma_alloc_txe(efa_rdm_ep, msg, ofi_op_read_req, flags);
	if (OFI_UNLIKELY(!txe)) {
		efa_rdm_ep_progress_internal(efa_rdm_ep);
		err = -FI_EAGAIN;
		goto out;
	}

	use_lower_ep_read = false;
	if (efa_both_support_rdma_read(efa_rdm_ep, peer)) {
		/* efa_both_support_rdma_read also check domain.use_device_rdma,
		 * so we do not check it here
		 */
		use_lower_ep_read = true;
	} else if (efa_mr_is_neuron(txe->desc[0])) {
		err = efa_rdm_ep_determine_rdma_read_support(efa_rdm_ep, txe->addr, peer);

		if (err < 0)
			goto out;

		if (err != 1) {
			err = -FI_EOPNOTSUPP;
			goto out;
		}

		use_lower_ep_read = true;
	}

	/*
	 * Not going to check efa_ep->hmem_p2p_opt here, if the remote side
	 * gave us a valid MR we should just honor the request even if p2p is
	 * disabled.
	 */
	if (use_lower_ep_read) {
		err = efa_rdm_ope_prepare_to_post_read(txe);
		if (err)
			goto out;
			
		err = efa_rdm_ope_post_read(txe);
		if (OFI_UNLIKELY(err)) {
			if (err == -FI_ENOBUFS)
				err = -FI_EAGAIN;
			efa_rdm_ep_progress_internal(efa_rdm_ep);
			goto out;
		}
	} else {
		err = efa_rdm_rma_post_efa_emulated_read(efa_rdm_ep, txe);
		if (OFI_UNLIKELY(err))
			efa_rdm_ep_progress_internal(efa_rdm_ep);
	}

out:
	if (OFI_UNLIKELY(err && txe))
		efa_rdm_txe_release(txe);

	ofi_genlock_unlock(srx_ctx->lock);
	efa_perfset_end(efa_rdm_ep, perf_efa_tx);
	return err;
}

ssize_t efa_rdm_rma_readv(struct fid_ep *ep, const struct iovec *iov, void **desc,
		      size_t iov_count, fi_addr_t src_addr, uint64_t addr,
		      uint64_t key, void *context)
{
	struct fi_rma_iov rma_iov;
	struct fi_msg_rma msg;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ep *efa_rdm_ep;
	void *shm_desc[RXR_IOV_LIMIT] = {NULL};
	int err;

	efa_rdm_ep = container_of(ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
	err = efa_rdm_ep_cap_check_rma(efa_rdm_ep);
	if (err)
		return err;

	peer = efa_rdm_ep_get_peer(efa_rdm_ep, src_addr);
	assert(peer);
	if (peer->is_local && efa_rdm_ep->use_shm_for_tx) {
		if (desc)
			efa_rdm_get_desc_for_shm(iov_count, desc, shm_desc);

		return fi_readv(efa_rdm_ep->shm_ep, iov, desc? shm_desc : NULL, iov_count, peer->shm_fiaddr, addr, key, context);
	}

	rma_iov.addr = addr;
	rma_iov.len = ofi_total_iov_len(iov, iov_count);
	rma_iov.key = key;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.desc = desc;
	msg.iov_count = iov_count;
	msg.addr = src_addr;
	msg.context = context;
	msg.rma_iov = &rma_iov;
	msg.rma_iov_count = 1;

	return efa_rdm_rma_readmsg(ep, &msg, 0);
}

ssize_t efa_rdm_rma_read(struct fid_ep *ep, void *buf, size_t len, void *desc,
		     fi_addr_t src_addr, uint64_t addr, uint64_t key,
		     void *context)
{
	struct iovec iov;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ep *efa_rdm_ep;
	void *shm_desc[RXR_IOV_LIMIT] = {NULL};
	int err;

	efa_rdm_ep = container_of(ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
	err = efa_rdm_ep_cap_check_rma(efa_rdm_ep);
	if (err)
		return err;

	peer = efa_rdm_ep_get_peer(efa_rdm_ep, src_addr);
	assert(peer);
	if (peer->is_local && efa_rdm_ep->use_shm_for_tx) {
		if (desc)
			efa_rdm_get_desc_for_shm(1, &desc, shm_desc);

		return fi_read(efa_rdm_ep->shm_ep, buf, len, desc? shm_desc[0] : NULL, peer->shm_fiaddr, addr, key, context);
	}

	iov.iov_base = (void *)buf;
	iov.iov_len = len;
	return efa_rdm_rma_readv(ep, &iov, &desc, 1, src_addr, addr, key, context);
}

/**
 * @brief Decide if we should issue this WRITE using rdma-core, or via emulation.
 *
 * This function could force a handshake with peer, otherwise ep and peer will be
 * read-only.
 *
 * @param ep[in,out]		The endpoint.
 * @param txe[in]		The ope context for this write.
 * @param peer[in,out]		The peer we will be writing to.
 * @return true			When WRITE can be done using RDMA_WRITE
 * @return false		When WRITE should be emulated with SEND's
 */
static inline
bool efa_rdm_rma_should_write_using_rdma(struct efa_rdm_ep *ep, struct efa_rdm_ope *txe, struct efa_rdm_peer *peer)
{
	/*
	 * RDMA_WRITE does not support FI_INJECT, because device may
	 * need to re-send data and FI_INJECT allows user to re-use
	 * these buffers immediately.
	 */
	if (txe->fi_flags & FI_INJECT)
		return false;

	/*
	 * Because EFA is unordered and EFA iov descriptions can be more
	 * expressive than the IBV sge's, we only implement
	 * FI_REMOTE_CQ_DATA using RDMA_WRITE_WITH_IMM when a single iov
	 * is given, otherwise we use sends to emulate it.
	 */
	if ((txe->fi_flags & FI_REMOTE_CQ_DATA) &&
	    (txe->iov_count > 1 || txe->rma_iov_count > 1))
		return false;

	/* Check for hardware support of RDMA write.
	   This will incur a handshake for new peers. */
	return efa_rdm_ep_determine_rdma_write_support(ep, txe->addr, peer);
}

/**
 * @brief Post a WRITE described the txe
 *
 * @param ep		The endpoint.
 * @param txe	The ope context for this write.
 * @return On success return 0, otherwise return a negative libfabric error code.
 */
ssize_t efa_rdm_rma_post_write(struct efa_rdm_ep *ep, struct efa_rdm_ope *txe)
{
	ssize_t err;
	struct efa_rdm_peer *peer;
	bool delivery_complete_requested;
	int ctrl_type, iface;
	size_t max_eager_rtw_data_size;

	peer = efa_rdm_ep_get_peer(ep, txe->addr);
	assert(peer);

	if (efa_rdm_rma_should_write_using_rdma(ep, txe, peer)) {
		efa_rdm_ope_prepare_to_post_write(txe);
		return efa_rdm_ope_post_remote_write(txe);
	}

	delivery_complete_requested = txe->fi_flags & FI_DELIVERY_COMPLETE;
	if (delivery_complete_requested) {
		/*
		 * Because delivery complete is defined as an extra
		 * feature, the receiver might not support it.
		 *
		 * The sender cannot send with FI_DELIVERY_COMPLETE
		 * if the peer is not able to handle it.
		 *
		 * If the sender does not know whether the peer
		 * can handle it, it needs to trigger
		 * a handshake packet from the peer.
		 *
		 * The handshake packet contains
		 * the information whether the peer
		 * support it or not.
		 */
		err = rxr_pkt_trigger_handshake(ep, txe->addr, peer);
		if (OFI_UNLIKELY(err))
			return err;

		if (!(peer->flags & EFA_RDM_PEER_HANDSHAKE_RECEIVED))
			return -FI_EAGAIN;
		else if (!efa_rdm_peer_support_delivery_complete(peer))
			return -FI_EOPNOTSUPP;

		max_eager_rtw_data_size = efa_rdm_txe_max_req_data_capacity(ep, txe, RXR_DC_EAGER_RTW_PKT);
	} else {
		max_eager_rtw_data_size = efa_rdm_txe_max_req_data_capacity(ep, txe, RXR_EAGER_RTW_PKT);
	}

	/* Inter instance */

	iface = txe->desc[0] ? ((struct efa_mr*) txe->desc[0])->peer.iface : FI_HMEM_SYSTEM;

	if (txe->total_len >= efa_rdm_ep_domain(ep)->hmem_info[iface].min_read_write_size &&
		efa_rdm_ep_determine_rdma_read_support(ep, txe->addr, peer) &&
		(txe->desc[0] || efa_is_cache_available(efa_rdm_ep_domain(ep)))) {
		err = rxr_pkt_post(ep, txe, RXR_LONGREAD_RTW_PKT);
		if (err != -FI_ENOMEM)
			return err;
		/*
		 * If read write protocol failed due to memory registration, fall back to use long
		 * message protocol
		 */
	}

	if (txe->total_len <= max_eager_rtw_data_size) {
		ctrl_type = delivery_complete_requested ? RXR_DC_EAGER_RTW_PKT : RXR_EAGER_RTW_PKT;
		return rxr_pkt_post(ep, txe, ctrl_type);
	}

	ctrl_type = delivery_complete_requested ? RXR_DC_LONGCTS_RTW_PKT : RXR_LONGCTS_RTW_PKT;
	return rxr_pkt_post(ep, txe, ctrl_type);
}

ssize_t efa_rdm_rma_writemsg(struct fid_ep *ep,
			 const struct fi_msg_rma *msg,
			 uint64_t flags)
{
	ssize_t err;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ep *efa_rdm_ep;
	struct efa_rdm_ope *txe;
	fi_addr_t tmp_addr;
	struct fi_msg_rma *msg_clone;
	void *shm_desc[RXR_IOV_LIMIT];
	void **tmp_desc;
	struct util_srx_ctx *srx_ctx;

	EFA_DBG(FI_LOG_EP_DATA,
	       "write iov_len %lu flags: %lx\n",
	       ofi_total_iov_len(msg->msg_iov, msg->iov_count),
	       flags);

	efa_rdm_ep = container_of(ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
	srx_ctx = efa_rdm_ep_get_peer_srx_ctx(efa_rdm_ep);
	err = efa_rdm_ep_cap_check_rma(efa_rdm_ep);
	if (err)
		return err;

	assert(msg->iov_count <= efa_rdm_ep->tx_iov_limit);

	efa_perfset_start(efa_rdm_ep, perf_efa_tx);
	ofi_genlock_lock(srx_ctx->lock);

	peer = efa_rdm_ep_get_peer(efa_rdm_ep, msg->addr);
	assert(peer);

	if (peer->flags & EFA_RDM_PEER_IN_BACKOFF) {
		err = -FI_EAGAIN;
		goto out;
	}

	if (peer->is_local && efa_rdm_ep->use_shm_for_tx) {
		tmp_addr = msg->addr;
		tmp_desc = msg->desc;
		msg_clone = (struct fi_msg_rma *)msg;
		msg_clone->addr = peer->shm_fiaddr;
		if (msg->desc) {
			efa_rdm_get_desc_for_shm(msg->iov_count, msg->desc, shm_desc);
			msg_clone->desc = shm_desc;
		} else {
			msg_clone->desc = NULL;
		}
		err = fi_writemsg(efa_rdm_ep->shm_ep, msg, flags);
		msg_clone->addr = tmp_addr;
		msg_clone->desc = tmp_desc;
		goto out;
	}

	txe = efa_rdm_rma_alloc_txe(efa_rdm_ep, msg, ofi_op_write, flags);
	if (OFI_UNLIKELY(!txe)) {
		efa_rdm_ep_progress_internal(efa_rdm_ep);
		err = -FI_EAGAIN;
		goto out;
	}

	err = efa_rdm_rma_post_write(efa_rdm_ep, txe);
	if (OFI_UNLIKELY(err)) {
		efa_rdm_ep_progress_internal(efa_rdm_ep);
		efa_rdm_txe_release(txe);
	}
out:
	ofi_genlock_unlock(srx_ctx->lock);
	efa_perfset_end(efa_rdm_ep, perf_efa_tx);
	return err;
}

ssize_t efa_rdm_rma_writev(struct fid_ep *ep, const struct iovec *iov, void **desc,
		       size_t iov_count, fi_addr_t dest_addr, uint64_t addr,
		       uint64_t key, void *context)
{
	struct fi_rma_iov rma_iov;
	struct fi_msg_rma msg;

	struct efa_rdm_peer *peer;
	struct efa_rdm_ep *efa_rdm_ep;
	void *shm_desc[RXR_IOV_LIMIT] = {NULL};
	int err;

	efa_rdm_ep = container_of(ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
	err = efa_rdm_ep_cap_check_rma(efa_rdm_ep);
	if (err)
		return err;

	peer = efa_rdm_ep_get_peer(efa_rdm_ep, dest_addr);
	assert(peer);
	if (peer->is_local && efa_rdm_ep->use_shm_for_tx) {
		if (desc)
			efa_rdm_get_desc_for_shm(iov_count, desc, shm_desc);
		return fi_writev(efa_rdm_ep->shm_ep, iov, desc? shm_desc : NULL, iov_count, peer->shm_fiaddr, addr, key, context);
	}

	rma_iov.addr = addr;
	rma_iov.len = ofi_total_iov_len(iov, iov_count);
	rma_iov.key = key;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.desc = desc;
	msg.iov_count = iov_count;
	msg.addr = dest_addr;
	msg.context = context;
	msg.rma_iov = &rma_iov;
	msg.rma_iov_count = 1;

	return efa_rdm_rma_writemsg(ep, &msg, 0);
}

ssize_t efa_rdm_rma_write(struct fid_ep *ep, const void *buf, size_t len, void *desc,
		      fi_addr_t dest_addr, uint64_t addr, uint64_t key,
		      void *context)
{
	struct iovec iov;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ep *efa_rdm_ep;
	void *shm_desc[RXR_IOV_LIMIT] = {NULL};
	int err;

	efa_rdm_ep = container_of(ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
	err = efa_rdm_ep_cap_check_rma(efa_rdm_ep);
	if (err)
		return err;

	peer = efa_rdm_ep_get_peer(efa_rdm_ep, dest_addr);
	assert(peer);
	if (peer->is_local && efa_rdm_ep->use_shm_for_tx) {
		if (desc)
			efa_rdm_get_desc_for_shm(1, &desc, shm_desc);
		return fi_write(efa_rdm_ep->shm_ep, buf, len, desc? shm_desc[0] : NULL, peer->shm_fiaddr, addr, key, context);
	}

	iov.iov_base = (void *)buf;
	iov.iov_len = len;
	return efa_rdm_rma_writev(ep, &iov, &desc, 1, dest_addr, addr, key, context);
}

ssize_t efa_rdm_rma_writedata(struct fid_ep *ep, const void *buf, size_t len,
			  void *desc, uint64_t data, fi_addr_t dest_addr,
			  uint64_t addr, uint64_t key, void *context)
{
	struct iovec iov;
	struct fi_rma_iov rma_iov;
	struct fi_msg_rma msg;
	struct efa_rdm_peer *peer;
	struct efa_rdm_ep *efa_rdm_ep;
	void *shm_desc[RXR_IOV_LIMIT] = {NULL};
	int err;

	efa_rdm_ep = container_of(ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
	err = efa_rdm_ep_cap_check_rma(efa_rdm_ep);
	if (err)
		return err;

	peer = efa_rdm_ep_get_peer(efa_rdm_ep, dest_addr);
	assert(peer);
	if (peer->is_local && efa_rdm_ep->use_shm_for_tx) {
		if (desc)
			efa_rdm_get_desc_for_shm(1, &desc, shm_desc);
		return fi_writedata(efa_rdm_ep->shm_ep, buf, len, desc? shm_desc[0] : NULL, data, peer->shm_fiaddr, addr, key, context);
	}

	iov.iov_base = (void *)buf;
	iov.iov_len = len;
	rma_iov.addr = addr;
	rma_iov.len  = len;
	rma_iov.key = key;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.desc = &desc;
	msg.iov_count = 1;
	msg.addr = dest_addr;
	msg.context = context;
	msg.rma_iov = &rma_iov;
	msg.rma_iov_count = 1;
	msg.data = data;

	return efa_rdm_rma_writemsg(ep, &msg, FI_REMOTE_CQ_DATA);
}

ssize_t efa_rdm_rma_inject_write(struct fid_ep *ep, const void *buf, size_t len,
			     fi_addr_t dest_addr, uint64_t addr, uint64_t key)
{
	struct fi_msg_rma msg;
	struct iovec iov;
	struct fi_rma_iov rma_iov;
	struct efa_rdm_ep *efa_rdm_ep;
	struct efa_rdm_peer *peer;
	int err;

	efa_rdm_ep = container_of(ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
	err = efa_rdm_ep_cap_check_rma(efa_rdm_ep);
	if (err)
		return err;

	peer = efa_rdm_ep_get_peer(efa_rdm_ep, dest_addr);
	if (peer->is_local)
		return fi_inject_write(efa_rdm_ep->shm_ep, buf, len, peer->shm_fiaddr, addr, key);

	iov.iov_base = (void *)buf;
	iov.iov_len = len;
	rma_iov.addr = addr;
	rma_iov.len  = len;
	rma_iov.key = key;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.iov_count = 1;
	msg.rma_iov = &rma_iov;
	msg.rma_iov_count = 1;
	msg.addr = dest_addr;

	return efa_rdm_rma_writemsg(ep, &msg, FI_INJECT | EFA_RDM_TXE_NO_COMPLETION);
}

ssize_t efa_rdm_rma_inject_writedata(struct fid_ep *ep, const void *buf, size_t len,
				 uint64_t data, fi_addr_t dest_addr, uint64_t addr,
				 uint64_t key)
{
	struct fi_msg_rma msg;
	struct iovec iov;
	struct fi_rma_iov rma_iov;
	struct efa_rdm_ep *efa_rdm_ep;
	struct efa_rdm_peer *peer;
	int err;

	efa_rdm_ep = container_of(ep, struct efa_rdm_ep, base_ep.util_ep.ep_fid.fid);
	err = efa_rdm_ep_cap_check_rma(efa_rdm_ep);
	if (err)
		return err;

	peer = efa_rdm_ep_get_peer(efa_rdm_ep, dest_addr);
	if (peer->is_local)
		return fi_inject_writedata(efa_rdm_ep->shm_ep, buf, len, data, peer->shm_fiaddr, addr, key);

	iov.iov_base = (void *)buf;
	iov.iov_len = len;
	rma_iov.addr = addr;
	rma_iov.len  = len;
	rma_iov.key = key;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.iov_count = 1;
	msg.rma_iov = &rma_iov;
	msg.rma_iov_count = 1;
	msg.addr = dest_addr;
	msg.data = data;

	return efa_rdm_rma_writemsg(ep, &msg, FI_INJECT | EFA_RDM_TXE_NO_COMPLETION |
				FI_REMOTE_CQ_DATA);
}

struct fi_ops_rma efa_rdm_rma_ops = {
	.size = sizeof(struct fi_ops_rma),
	.read = efa_rdm_rma_read,
	.readv = efa_rdm_rma_readv,
	.readmsg = efa_rdm_rma_readmsg,
	.write = efa_rdm_rma_write,
	.writev = efa_rdm_rma_writev,
	.writemsg = efa_rdm_rma_writemsg,
	.inject = efa_rdm_rma_inject_write,
	.writedata = efa_rdm_rma_writedata,
	.injectdata = efa_rdm_rma_inject_writedata,
};
