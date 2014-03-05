/**************************************************************************
 * Copyright 2013, Freescale Semiconductor, Inc. All rights reserved.
 ***************************************************************************/
/*
 * File:	ceetm_qman.c
 *
 * Description: CEETM Low level configuration inteface
 *
 * Authors:	Sachin Saxena <sachin.saxena@freescale.com>
 *
 * History
 *  Version     Date		Author			Change Description *
 *  1.0		15-10-2013	Sachin Saxena		Initial Changes
 */
 /****************************************************************************/

#include <dpa/dpaa_eth.h>
#include <dpa/dpaa_eth_common.h>
#include <dpa/mac.h>
#include <lnxwrp_fm.h>
#include "include/ceetm.h"

/* ----------------------------- */
/* CEETM HW COnfiguration APIs   */
/* ----------------------------- */

#define MAX_CEETM	2

static u8 ceetm_1g_lni_index[MAX_CEETM] = {2, 2};
static u8 ceetm_10g_lni_index[MAX_CEETM];

/******** Utility Fucntions *********/
static int get_fm_dcp_id(char *c, uint32_t *res)
{
	int ret = -EINVAL;
	uint32_t val;
	char buf[2];

	/* Only fm#-mac# type name expected */
	if (c[0] == 'f' && c[1] == 'm') {
		buf[0] = c[2];
		buf[1] = '\0';
		ret = kstrtouint(buf, 10, &val);
		if (ret == 0)
			*res = (val - 1);
	}
	return ret;
}

static int get_tx_port_type(struct mac_device *mac_dev)
{
	t_LnxWrpFmPortDev   *p_LnxWrpFmPortDev =
		(t_LnxWrpFmPortDev *)mac_dev->port_dev[TX];


	ceetm_dbg("FM sub-portal IDX is %d\n",  p_LnxWrpFmPortDev->id);
	return p_LnxWrpFmPortDev->settings.param.portType;
}
void ceetm_inc_drop_cnt(void *handle)
{
	struct ceetm_fq *ceetm_fq = (struct ceetm_fq *)handle;

	spin_lock(&(ceetm_fq->lock));
	ceetm_fq->ulDroppedPkts++;
	spin_unlock(&(ceetm_fq->lock));
}

void ceetm_inc_enqueue_cnt(void *handle)
{
	struct ceetm_fq *ceetm_fq = (struct ceetm_fq *)handle;

	spin_lock(&(ceetm_fq->lock));
	ceetm_fq->ulEnqueuePkts++;
	spin_unlock(&(ceetm_fq->lock));
}

void ceetm_get_fq_stats(void *handle, uint64_t *enq, uint64_t *drp)
{
	struct ceetm_fq *ceetm_fq = (struct ceetm_fq *)handle;

	*enq = ceetm_fq->ulEnqueuePkts;
	*drp = ceetm_fq->ulDroppedPkts;
	ceetm_fq->ulEnqueuePkts = 0;
	ceetm_fq->ulDroppedPkts = 0;
}

static void egress_ern(struct qman_portal	*portal,
		       struct qman_fq		*fq,
		       const struct qm_mr_entry	*msg)
{
	struct qm_fd fd = msg->ern.fd;
	dma_addr_t addr = qm_fd_addr(&fd);
	struct sk_buff *skb = NULL;
	struct sk_buff **skbh;
	struct ceetm_fq *ceetm_fq;
	const struct dpa_priv_s	*priv;
	struct dpa_bp *bp;

	ceetm_fq = (struct ceetm_fq *)fq;
	/* Updates STATS */
	ceetm_inc_drop_cnt((void *)ceetm_fq);

	if (fd.cmd & FM_FD_CMD_FCO) {
		ceetm_dbg("Releasing FD.!\n");
		dpa_fd_release(ceetm_fq->net_dev, &fd);
		return;
	}

	priv = netdev_priv(ceetm_fq->net_dev);
	bp = priv->dpa_bp;

	if (unlikely(!addr))
		return;

	skbh = (struct sk_buff **)phys_to_virt(addr);

	if (fd.format == qm_fd_contig) {
		/* Retrieve the skb backpointer */
		skb = *skbh;
		dma_unmap_single(bp->dev, addr, bp->size,
					 DMA_TO_DEVICE);
	} else {
		ceetm_err("SG Buffer..?\n");
		/* Retrieve the skb backpointer */
		skb = *skbh;
		/* Free first buffer (which was allocated on Tx) */
		kfree((void *) skbh);

	}
	dev_kfree_skb_any(skb);
}

int ceetm_enqueue_pkt(void *handle, struct sk_buff *skb)
{

	struct ceetm_fq *fq = (struct ceetm_fq *)handle;
	struct qman_fq		*tx_fq = &(fq->egress_fq);
	struct qm_fd		*tx_fd, fd;
	struct dpa_priv_s	*priv;
	struct dpa_bp		*dpa_bp;
	struct dpa_percpu_priv_s *percpu_priv;
	struct sk_buff		**skbh;
	dma_addr_t		addr;
	enum dma_data_direction dma_dir = DMA_TO_DEVICE;
	bool	can_recycle = false;
	int	offset, extra_offset;
	int	err, i;
	int *countptr;
	struct net_device *dev = skb->dev;

	/* We will not enqueue if Queue is already congested.
	   This will save lot of ERN interrupts & their handling
	   which may cause even CPU hang/ and DPA RX buffer
	   allocation failure */
	if (fq->congested)
		return -ENOSPC;

	priv = netdev_priv(dev);
	percpu_priv = per_cpu_ptr(priv->percpu_priv, smp_processor_id());
	countptr = __this_cpu_ptr(priv->dpa_bp->percpu_count);
	dpa_bp = priv->dpa_bp;

	/* In case, packet is recieved from Linux,
	   Head room may not be sufficient */
	if (skb_headroom(skb) < priv->tx_headroom) {
		struct sk_buff *skb_new;

		skb_new = skb_realloc_headroom(skb, priv->tx_headroom);
		if (unlikely(!skb_new)) {
			/* Increment Error Stat */
			ceetm_err("Headroom Allocation error.\n");
			return -ENOMEM;
		}
		dev_kfree_skb(skb);
		skb = skb_new;
	}

	/* TODO, if SKB is cloned & require SG support*/
	skb = skb_unshare(skb, GFP_ATOMIC);
	if (!skb)
		return CEETM_SUCCESS;

	tx_fd = &fd;
	/* Clear Required field in FD */
	tx_fd->opaque_addr = 0;
	tx_fd->opaque = 0;
	tx_fd->cmd = 0;

#ifdef CONFIG_FSL_DPAA_1588
	if (priv->tsu && priv->tsu->valid && priv->tsu->hwts_tx_en_ioctl)
		fd.cmd |= FM_FD_CMD_UPD;
#endif
#ifdef CONFIG_FSL_DPAA_TS
	if (unlikely(priv->ts_tx_en &&
			skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP))
		fd.cmd |= FM_FD_CMD_UPD;
#endif /* CONFIG_FSL_DPAA_TS */

#define DPA_RECYCLE_MAX_SIZE	16384
/* Maximum offset value for a contig or sg FD (represented on 9bits) */
#define MAX_FD_OFFSET	((1 << 9) - 1)
	/* Now Convert SKB to Tx_FD */
	if (likely(skb_is_recycleable(skb, dpa_bp->size)
		&& (skb_end_pointer(skb) - skb->head <= DPA_RECYCLE_MAX_SIZE)
		&& (*countptr < dpa_bp->target_count))) {
		/* Compute the minimum necessary fd offset */
		offset = dpa_bp->size - skb->len - skb_tailroom(skb);

		/*
		 * And make sure the offset is no lower than DPA_BP_HEAD,
		 * as required by FMan
		 */
		offset = max(offset, (int)priv->tx_headroom);

		/*
		 * We also need to align the buffer address to 16, such that
		 * Fman will be able to reuse it on Rx.
		 */
		extra_offset = (unsigned long)(skb->data - offset) & 0xF;
		if (likely((offset + extra_offset) <= skb_headroom(skb) &&
			   (offset + extra_offset) <= MAX_FD_OFFSET)) {
			/* We're good to go for recycling*/
			offset += extra_offset;
			/* can_recycle = true; */
		}
	}
#ifdef CONFIG_FSL_DPAA_TS
	if (unlikely(priv->ts_tx_en &&
			skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP)) {
		/* we need the fd back to get the timestamp */
		can_recycle = false;
		skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
	}
#endif /* CONFIG_FSL_DPAA_TS */
	if (can_recycle) {
		/* Buffer will get recycled, setup fd accordingly */
		tx_fd->cmd = FM_FD_CMD_FCO;
		tx_fd->bpid = dpa_bp->bpid;
		dma_dir = DMA_BIDIRECTIONAL;
	} else {
		offset = priv->tx_headroom;
	}

	skbh = (struct sk_buff **)(skb->data - offset);
	*skbh = skb;

	tx_fd->format = qm_fd_contig;
	tx_fd->length20 = skb->len;
	tx_fd->offset = offset;

	if (!priv->mac_dev || skb->ip_summed != CHECKSUM_PARTIAL) {
		/* Reaching Here means HW Checksum offloading not required.
		   We are not implementing this as linux packet
		   don't use this feature but still putting this
		   check to catch if any packet comes */
	} else
		ceetm_err("HW CHECKSUM Offload handling required..!\n");

	addr = dma_map_single(dpa_bp->dev, skbh, dpa_bp->size, dma_dir);
	if (unlikely(addr == 0)) {
		ceetm_err("xmit dma_map Error\n");
		return -EINVAL;
	}

	tx_fd->addr_hi = upper_32_bits(addr);
	tx_fd->addr_lo = lower_32_bits(addr);

	if (can_recycle) {
		/* Recycle SKB */
		(*countptr)++;
		skb_recycle(skb);
		skb = NULL;
		percpu_priv->tx_returned++;
	}

	for (i = 0; i < 100000; i++) {
		err = qman_enqueue(tx_fq, tx_fd, 0);
		if (err != -EBUSY)
			break;
	}
	if (unlikely(err < 0)) {
		if (tx_fd->cmd & FM_FD_CMD_FCO) {
			(*countptr)--;
			percpu_priv->tx_returned--;
		}
		ceetm_err("QMAN Enqueue Error.\n");
		return -EINVAL;
	}
	dev->trans_start = jiffies;
	return CEETM_SUCCESS;
}

/**
 * ceetm_cfg_lni - Claims & configure a sub-portal and LNI object
 * from QMAN CEETM, provided it is available to us and configured
 * for traffic-management.
 * @dev: Network device pointer.
 * @q: CEETM Qdisc private data structre
*/
void ceetm_cfg_lni(struct net_device *dev,
			struct ceetm_sched *q)
{
	struct dpa_priv_s *priv;
	struct mac_device *mac_dev;
	struct qm_ceetm_sp *sp = NULL;
	struct qm_ceetm_lni *lni = NULL;
	uint32_t dcp_id, sp_idx, lni_idx;
	struct ceetm_class *shaped;
	struct qm_ceetm_rate token_rate, token_ceil;
	uint16_t token_limit;

	priv = netdev_priv(dev);
	mac_dev = priv->mac_dev;

	if (!mac_dev) {
		ceetm_err("MAC dev not exist for (%s)\n", dev->name);
		return;
	}
	if (get_fm_dcp_id(dev->name, &dcp_id)) {
		ceetm_err("Invalid  device (%s)\n", dev->name);
		return;
	}
	if (e_FM_PORT_TYPE_TX_10G == get_tx_port_type(mac_dev)) {
		ceetm_dbg("Port_type is 10G\n");
		/* Using LNI 0 & 1 for 10G ports only */
		lni_idx = ceetm_10g_lni_index[dcp_id]++;
		sp_idx = mac_dev->cell_index;
	} else {
		ceetm_dbg("Port_type is 1G\n");
		lni_idx = ceetm_1g_lni_index[dcp_id]++;
		sp_idx = mac_dev->cell_index + 2;
	}
	/* claim a subportal */
	if (qman_ceetm_sp_claim(&sp, dcp_id, sp_idx))
		return;
	/* claim a LNI */
	if (qman_ceetm_lni_claim(&lni, dcp_id, lni_idx))
		goto error;
	lni->dcp_idx = dcp_id;
	/* Set SP to LNI Mapping */
	if (qman_ceetm_sp_set_lni(sp, lni))
		goto error;
	sp->dcp_idx = dcp_id;
	ceetm_dbg("Claimed LNI_idx %d & SP_idx %d\n", lni_idx, sp_idx);
	/* Store the SP pointer in LNI.... Missing in CEETM QMAN APIs */
	lni->sp = sp;

	/* Enable Shaper, if Configured */
	shaped = &(q->un.root.shaped);
	if (!shaped->cfg.root.shaping_en) {
		ceetm_dbg("Unshaped Class: Skipping Shaper configuration.\n");
		goto exit;
	}
	/* Else configure the LNI Shaper */
	if (qman_ceetm_lni_enable_shaper(lni, 1, shaped->cfg.root.overhead))
		goto error;

	if (qman_ceetm_bps2tokenrate(shaped->cfg.root.rate * 8, &token_rate, 0))
		goto error;
	ceetm_dbg("CR Rate %d token.whole %d  token.fraction %d\n",
				shaped->cfg.root.rate,
				token_rate.whole,
				token_rate.fraction);
	if (qman_ceetm_bps2tokenrate(shaped->cfg.root.ceil * 8,
						&token_ceil, 0))
		goto error;
	ceetm_dbg("ER Rate %d token.whole %d  token.fraction %d\n",
				shaped->cfg.root.ceil,
				token_ceil.whole,
				token_ceil.fraction);
	/* Set Committed Rate */
	token_limit = dev->mtu + token_rate.fraction;
	if (qman_ceetm_lni_set_commit_rate(lni, &token_rate,
						token_limit))
		goto error;
	/* Set Exccess Rate */
	token_limit = dev->mtu + token_ceil.fraction;
	if (qman_ceetm_lni_set_excess_rate(lni, &token_ceil,
						token_limit))
		goto error;
exit:
	q->hw_handle = (void *)lni;
	return;
error:
	if (lni)
		qman_ceetm_lni_release(lni);
	if (sp)
		qman_ceetm_sp_release(sp);
}

/**
  * Congestion State Callback function.
**/
void ceetm_cscn(struct qm_ceetm_ccg *p, void *cb_ctx, int congested)
{

	struct ceetm_fq *ceetm_fq = (struct ceetm_fq *)cb_ctx;
	/* Update the congestion state */
	ceetm_fq->congested = congested;
	return;
}

/**
 * ceetm_cfg_channel - Claims & configure a PRIO Channel Scheduler for given LNI
 * from QMAN CEETM, provided it is available to us and configured
 * for traffic-management.
 * @parent: Parent Class pointer which contains Rate/Wieght related information.
 * @q: CEETM Qdisc private data structre
*/
void ceetm_cfg_channel(void *handle,
			uint32_t mtu,
			struct ceetm_sched *q)
{
	struct qm_ceetm_lni *lni = (struct qm_ceetm_lni *)handle;
	struct qm_ceetm_channel *channel = NULL;
	struct qm_ceetm_rate token_rate, token_ceil;
	struct ceetm_class *parent;
	struct qm_ceetm_ccg *ccg = NULL, *tmp;
	struct qm_ceetm_ccg_params params;
	uint16_t token_limit, mask;
	int i;

	/* claim a channel scheduler */
	if (qman_ceetm_channel_claim(&channel, lni)) {
		ceetm_err("Failed to claim Channel Scheduler"
					" for LNI (0x%X)\n", lni->idx);
		return;
	}
	ceetm_dbg("Claimed Channel %d for LNI %d\n", channel->idx, lni->idx);
	/* Enable Shaper, if Configured */
	parent = q->parent;
	if (parent->cfg.inner.rate) {
		/* configure channel shaper */
		if (qman_ceetm_channel_enable_shaper(channel, 1))
			goto error;
		if (qman_ceetm_bps2tokenrate(parent->cfg.inner.rate * 8,
							&token_rate, 0))
			goto error;
		ceetm_dbg("CR Rate %d token.whole %d  token.fraction %d\n",
				parent->cfg.inner.rate, token_rate.whole,
				token_rate.fraction);
		if (qman_ceetm_bps2tokenrate(parent->cfg.inner.ceil * 8,
							&token_ceil, 0))
			goto error;
		ceetm_dbg("ER Rate %d token.whole %d  token.fraction %d\n",
				parent->cfg.inner.ceil, token_ceil.whole,
				token_ceil.fraction);
		/* Set Committed Rate */
		token_limit = mtu + token_rate.fraction;
		if (qman_ceetm_channel_set_commit_rate(channel, &token_rate,
						token_limit))
			goto error;
		/* Set Exccess Rate */
		token_limit = mtu + token_ceil.fraction;
		if (qman_ceetm_channel_set_excess_rate(channel, &token_ceil,
						token_limit))
			goto error;
	/* This may be a unshaped channel */
	} else if (parent->cfg.inner.weight) {
		ceetm_dbg("Configuring unshaped weight %d\n",
					parent->cfg.inner.weight);
		/* Configure weight for unshaped channel fair queuing */
		if (qman_ceetm_channel_set_weight(channel,
				parent->cfg.inner.weight))
			goto error;
	} else {
		ceetm_err("Neither Weight nor Rate is configured.\n");
		return;
	}
	/* Claim Congestion control groups with index 0 - 15.*/
	memset(&params, 0, sizeof(struct qm_ceetm_ccg_params));
	params.mode = 1;
	params.cscn_en = 1;
	params.td_en = 0;
	/* Threshold start limt is 64 & exit limt is 32 . These values
	   are reffered from HW QMAN CEETM test scripts*/
	params.cs_thres_in.TA = 32;
	params.cs_thres_in.Tn = 1;
	params.cs_thres_out.TA = 16;
	params.cs_thres_out.Tn = 1;
	mask = QM_CCGR_WE_TD_EN | QM_CCGR_WE_MODE | QM_CCGR_WE_CSCN_EN |
			QM_CCGR_WE_CS_THRES_IN | QM_CCGR_WE_CS_THRES_OUT;
	for (i = 0; i < 16; i++) {
		/* Callback context of each CQ will set later, when alloacted */
		if (qman_ceetm_ccg_claim(&ccg, channel, i, ceetm_cscn, NULL))
				goto rel_ccg;
		if (qman_ceetm_ccg_set(ccg, mask, &params))
				goto rel_ccg;
		ceetm_dbg("CCG %d claimed for Channel %d\n", i, channel->idx);
	}
	q->hw_handle = (void *)channel;
	return;

rel_ccg:
	list_for_each_entry_safe(ccg, tmp, &channel->ccgs, node)
		qman_ceetm_ccg_release(ccg);
error:
	if (channel)
		qman_ceetm_channel_release(channel);
}

/**
 * ceetm_cfg_prio_leaf_q - Claims & configure CQ, LFQID & finally an
 * associated FQ, from QMAN CEETM, provided it is available to us and configured
 * for traffic-management.
*/
void ceetm_cfg_prio_leaf_q(void *handle,
			uint32_t idx,
			struct ceetm_class *cl,
			struct net_device *dev)
{
	struct qm_ceetm_channel *channel =
		(struct qm_ceetm_channel *)handle;
	struct qm_ceetm_cq *cq;
	struct qm_ceetm_lfq *lfq;
	struct ceetm_fq *fq;
	struct qm_ceetm_ccg *p = NULL;

	/* Find out the congestion group with index '0'*/
	list_for_each_entry(p, &channel->ccgs, node) {
		if (p->idx == idx)
			break;
	}
	if (p == NULL) {
		ceetm_err("CCG not found Class Queue %d"
				" for CH (0x%X)\n", idx, channel->idx);
		return;
	}
	/* claim a class queue */
	if (qman_ceetm_cq_claim(&cq, channel, idx, p)) {
		ceetm_err("Failed to claim Class Queue"
				" for CH (0x%X)\n", channel->idx);
		return;
	}
	/* Claim a LFQ */
	if (qman_ceetm_lfq_claim(&lfq, cq)) {
		ceetm_err("Failed to claim LFQ"
				" for CQ (0x%X)\n", cq->idx);
		return;
	}
	ceetm_dbg("Creating CQ [%d] --> LFQ/FQ [%d]\n", idx, lfq->idx);
	/* Allocate FQ */
	fq = (struct ceetm_fq *) kzalloc(sizeof(struct ceetm_fq),
							GFP_KERNEL);
	if (!fq)
		return;
	lfq->ern = egress_ern;
	fq->net_dev = dev;
	spin_lock_init(&(fq->lock));
	if (qman_ceetm_create_fq(lfq, &fq->egress_fq)) {
		kfree(fq);
		return;
	}
	/* All is well */
	p->cb_ctx = (void *) fq;
	cl->hw_handle = (void *)fq;
	cl->cq = (void *)cq;
	/* Set CR and ER eligibility of PRIO QDisc */
	if (qman_ceetm_channel_set_cq_cr_eligibility(channel,
				idx, cl->cfg.prio.cr_eligible)) {
		ceetm_err("Failed to set cr eligibility of cq %d"
				" for CH (0x%X)\n", idx, channel->idx);
		return;
	}
	if (qman_ceetm_channel_set_cq_er_eligibility(channel, idx,
				cl->cfg.prio.er_eligible)) {
		ceetm_err("Failed to set er eligibility of cq %d"
				" for CH (0x%X)\n", idx, channel->idx);
		return;
	}
	return;
}

/**
 * ceetm_cfg_wbfs_leaf_q - Claims & configure CQ, LFQID & finally
 * an associated FQ, from QMAN CEETM, provided it is available to us
 * and configured for traffic-management.
 * @parent: Parent Class pointer which contains Rate/Wieght related information.
 * @q: CEETM Qdisc private data structre
*/
void ceetm_cfg_wbfs_leaf_q(void *handle,
			int	grp,
			uint32_t idx,
			struct ceetm_class *cl,
			struct net_device *dev)
{
	struct qm_ceetm_channel *channel =
		(struct qm_ceetm_channel *)handle;
	struct qm_ceetm_weight_code weight_code;
	struct qm_ceetm_cq *cq;
	struct qm_ceetm_lfq *lfq;
	struct ceetm_fq *fq;
	struct qm_ceetm_ccg *p = NULL;

	/* claim a class queue */
	if (grp == CEETM_WBFS_GRP_B) {
		idx = idx + CEETM_WBFS_MAX_Q + CEETM_WBFS_MIN_Q;
		/* Find out the congestion group with index '0'*/
		list_for_each_entry(p, &channel->ccgs, node) {
			if (p->idx == idx)
				break;
		}
		if (p == NULL) {
			ceetm_err("CCG not found Class Queue %d"
				" for CH (0x%X)\n", idx, channel->idx);
			return;
		}
		if (qman_ceetm_cq_claim_B(&cq, channel, idx, p)) {
			ceetm_err("Failed to claim Class Queue B"
				" for CH (0x%X)\n", channel->idx);
			return;
		}
	} else {
		idx = idx + CEETM_WBFS_MAX_Q;
		/* Find out the congestion group with index '0'*/
		list_for_each_entry(p, &channel->ccgs, node) {
			if (p->idx == idx)
				break;
		}
		if (p == NULL) {
			ceetm_err("CCG not found Class Queue %d"
				" for CH (0x%X)\n", idx, channel->idx);
			return;
		}
		if (qman_ceetm_cq_claim_A(&cq, channel, idx, p)) {
			ceetm_err("Failed to claim Class Queue A"
				" for CH (0x%X)\n", channel->idx);
			return;
		}
	}
	/* Set the Queue Weight */
	qman_ceetm_ratio2wbfs(cl->cfg.wbfs.weight, 1,
					&weight_code, 1);
	qman_ceetm_set_queue_weight(cq, &weight_code);
	ceetm_dbg(" CQ weight is [%d] -->  y[%d] x[%d]\n",
			cl->cfg.wbfs.weight, weight_code.y, weight_code.x);
	/* Claim a LFQ */
	if (qman_ceetm_lfq_claim(&lfq, cq)) {
		ceetm_err("Failed to claim LFQ"
				" for CQ (0x%X)\n", cq->idx);
		return;
	}
	ceetm_dbg("Creating CQ [%d] --> LFQ/FQ [%d]\n", idx, lfq->idx);
	/* Allocate FQ */
	fq = (struct ceetm_fq *) kzalloc(sizeof(struct ceetm_fq),
							GFP_KERNEL);
	if (!fq)
		return;
	lfq->ern = egress_ern;
	fq->net_dev = dev;
	if (qman_ceetm_create_fq(lfq, &fq->egress_fq))
		return;
	/* All is well */
	p->cb_ctx = (void *) fq;
	cl->hw_handle = (void *)fq;
	cl->cq = (void *)cq;
	return;
}

int qman_ceetm_channel_set_group_cr_er_eligibility(
		struct ceetm_sched *p_q,
		int grp,
		u16 cr_eligibility,
		u16 er_eligibility)
{
	struct qm_ceetm_channel *channel =
		(struct qm_ceetm_channel *)p_q->hw_handle;
	int group_b_flag = 0;

	if (grp == CEETM_WBFS_GRP_B)
		group_b_flag = 1;

	if (qman_ceetm_channel_set_group_cr_eligibility(
				channel, group_b_flag, cr_eligibility)) {
		ceetm_err("Failed to set cr eligibility of group %d"
			" for CH (0x%X)\n", grp, channel->idx);
		return -1;
	}
	if (qman_ceetm_channel_set_group_er_eligibility(
			channel, group_b_flag, er_eligibility)) {
		ceetm_err("Failed to set er eligibility of cq %d"
			" for CH (0x%X)\n", grp, channel->idx);
		return -1;
	}

	return CEETM_SUCCESS;
}

int ceetm_cfg_wbfs_grp(void *handle,
			int	grp,
			uint32_t pri)
{
	struct qm_ceetm_channel *channel =
		(struct qm_ceetm_channel *)handle;

	/* claim a class queue */
	switch (grp) {
	case CEETM_WBFS_GRP_A:
	{
		/* Keeping Priority of group B = group A,
		   till it's not configured in future */
		ceetm_dbg("CEETM_WBFS_GRP_A with priority %d\n", pri);
		if (qman_ceetm_channel_set_group(channel, 1, pri, pri)) {
			ceetm_err("Failed to set Group A"
				" for CH (0x%X)\n", channel->idx);
			return -EINVAL;
		}
	}
	break;
	case CEETM_WBFS_GRP_B:
	{
		uint32_t prio_a, prio_b;
		int group_b;
		if (qman_ceetm_channel_get_group(channel,
					&group_b, &prio_a, &prio_b)) {
			ceetm_err("Failed to Get WBFS Group Settings"
				" for CH (0x%X)\n", channel->idx);
			return -EINVAL;
		}
		ceetm_dbg("CEETM_WBFS_GRP_B with prio_A %d  prio_B %d\n",
							prio_a, pri);
		if (qman_ceetm_channel_set_group(channel, 1, prio_a, pri)) {
			ceetm_err("Failed to Set Group B"
				" for CH (0x%X)\n", channel->idx);
			return -EINVAL;
		}
	}
	break;
	case CEETM_WBFS_GRP_both:
	{
		ceetm_dbg("CEETM_WBFS_GRP_BOTH with priority %d\n", pri);
		if (qman_ceetm_channel_set_group(channel, 0, pri, 0)) {
			ceetm_err("Failed to Set Group A"
				" for CH (0x%X)\n", channel->idx);
			return -EINVAL;
		}
	}
	break;
	}
	return CEETM_SUCCESS;
}

int ceetm_release_lni(void *handle)
{
	struct qm_ceetm_lni *lni =
		(struct qm_ceetm_lni *)handle;
	int ret;

	if (!lni)
		return CEETM_SUCCESS;
	ret = qman_ceetm_lni_release(lni);
	if (ret == 0)
		ret = qman_ceetm_sp_release(lni->sp);
	if (lni->idx < 2)
		ceetm_10g_lni_index[lni->dcp_idx]--;
	else
		ceetm_1g_lni_index[lni->dcp_idx]--;
	ceetm_dbg("Releasing LNI %d ---> SP %d \n",
				lni->idx, lni->sp->idx);
	return ret;
}

int ceetm_release_channel(void *handle)
{
	struct qm_ceetm_channel *channel =
		(struct qm_ceetm_channel *)handle;
	struct qm_ceetm_cq *cq, *tmp1;
	struct qm_ceetm_lfq *lfq, *tmp2;
	struct qm_ceetm_ccg *p , *tmp3;
	int ret;

	if (NULL == handle)
		return CEETM_SUCCESS;

	/* Find out the congestion group with index '0'*/
	list_for_each_entry_safe(p, tmp3, &channel->ccgs, node) {
		qman_ceetm_ccg_release(p);
	}
	/* Release all the CQ & LFQs */
	list_for_each_entry_safe(cq, tmp1, &channel->class_queues, node) {
		list_for_each_entry_safe(lfq, tmp2, &cq->bound_lfqids, node) {
			ceetm_dbg(" cq %p lfq %p Releasing CQ %d ---> LFQ %d\n",
						cq, lfq, cq->idx, lfq->idx);
			qman_ceetm_lfq_release(lfq);
		}
		ret = qman_ceetm_cq_release(cq);
		if (ret)
			return ret;

	}
	ceetm_dbg("Releasing Channel %d\n", channel->idx);
	ret = qman_ceetm_channel_release(channel);
	return ret;
}

int ceetm_release_wbfs_cq(void *handle)
{
	struct qm_ceetm_channel *channel =
		(struct qm_ceetm_channel *)handle;

	struct qm_ceetm_cq *cq, *tmp1;
	struct qm_ceetm_lfq *lfq, *tmp2;
	int ret;

	/* Release all WBFS's group CQs & LFQs */
	list_for_each_entry_safe(cq, tmp1, &channel->class_queues, node) {
		/* don't relaease PRIORITY CQs of the given channel */
		if (cq->idx < CEETM_WBFS_MAX_Q)
			continue;
		list_for_each_entry_safe(lfq, tmp2, &cq->bound_lfqids, node) {
			ceetm_dbg("WBFS Releasing CQ %d ---> LFQ %d\n",
						cq->idx, lfq->idx);
			qman_ceetm_lfq_release(lfq);
		}
		ret = qman_ceetm_cq_release(cq);
		if (ret)
			return ret;
	}
	return CEETM_SUCCESS;
}

void ceetm_release_cq(void *handle)
{
	/* release the (struct ceetm_fq) object memory */
	if (handle)
		kfree(handle);
}
