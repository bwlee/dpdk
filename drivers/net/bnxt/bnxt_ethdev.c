/*-
 *   BSD LICENSE
 *
 *   Copyright(c) Broadcom Limited.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Broadcom Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <inttypes.h>
#include <stdbool.h>

#include <rte_dev.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_cycles.h>

#include "bnxt.h"
#include "bnxt_hwrm.h"
#include "bnxt_rxq.h"
#include "bnxt_txq.h"

#define DRV_MODULE_NAME		"bnxt"
static const char bnxt_version[] =
	"Broadcom Cumulus driver " DRV_MODULE_NAME "\n";

static struct rte_pci_id bnxt_pci_id_map[] = {
#define RTE_PCI_DEV_ID_DECL_BNXT(vend, dev) {RTE_PCI_DEVICE(vend, dev)},
#include "rte_pci_dev_ids.h"
	{.device_id = 0},
};

static void bnxt_dev_close_op(struct rte_eth_dev *eth_dev)
{
	struct bnxt *bp = (struct bnxt *)eth_dev->data->dev_private;

	rte_free(eth_dev->data->mac_addrs);
	bnxt_free_hwrm_resources(bp);
}

/*
 * Device configuration and status function
 */

static void bnxt_dev_info_get_op(struct rte_eth_dev *eth_dev,
				  struct rte_eth_dev_info *dev_info)
{
	struct bnxt *bp = (struct bnxt *)eth_dev->data->dev_private;
	uint16_t max_vnics, i, j, vpool, vrxq;

	/* MAC Specifics */
	dev_info->max_mac_addrs = MAX_NUM_MAC_ADDR;
	dev_info->max_hash_mac_addrs = 0;

	/* PF/VF specifics */
	if (BNXT_PF(bp)) {
		dev_info->max_rx_queues = bp->pf.max_rx_rings;
		dev_info->max_tx_queues = bp->pf.max_tx_rings;
		dev_info->max_vfs = bp->pf.active_vfs;
		dev_info->reta_size = bp->pf.max_rsscos_ctx;
		max_vnics = bp->pf.max_vnics;
	} else {
		dev_info->max_rx_queues = bp->vf.max_rx_rings;
		dev_info->max_tx_queues = bp->vf.max_tx_rings;
		dev_info->reta_size = bp->vf.max_rsscos_ctx;
		max_vnics = bp->vf.max_vnics;
	}

	/* Fast path specifics */
	dev_info->min_rx_bufsize = 1;
	dev_info->max_rx_pktlen = BNXT_MAX_MTU + ETHER_HDR_LEN + ETHER_CRC_LEN
				  + VLAN_TAG_SIZE;
	dev_info->rx_offload_capa = 0;
	dev_info->tx_offload_capa = DEV_TX_OFFLOAD_IPV4_CKSUM |
					DEV_TX_OFFLOAD_TCP_CKSUM |
					DEV_TX_OFFLOAD_UDP_CKSUM |
					DEV_TX_OFFLOAD_TCP_TSO;

	/* *INDENT-OFF* */
	dev_info->default_rxconf = (struct rte_eth_rxconf) {
		.rx_thresh = {
			.pthresh = 8,
			.hthresh = 8,
			.wthresh = 0,
		},
		.rx_free_thresh = 32,
		.rx_drop_en = 0,
	};

	dev_info->default_txconf = (struct rte_eth_txconf) {
		.tx_thresh = {
			.pthresh = 32,
			.hthresh = 0,
			.wthresh = 0,
		},
		.tx_free_thresh = 32,
		.tx_rs_thresh = 32,
		.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS |
			     ETH_TXQ_FLAGS_NOOFFLOADS,
	};
	/* *INDENT-ON* */

	/*
	 * TODO: default_rxconf, default_txconf, rx_desc_lim, and tx_desc_lim
	 *       need further investigation.
	 */

	/* VMDq resources */
	vpool = 64; /* ETH_64_POOLS */
	vrxq = 128; /* ETH_VMDQ_DCB_NUM_QUEUES */
	for (i = 0; i < 4; vpool >>= 1, i++) {
		if (max_vnics > vpool) {
			for (j = 0; j < 5; vrxq >>= 1, j++) {
				if (dev_info->max_rx_queues > vrxq) {
					if (vpool > vrxq)
						vpool = vrxq;
					goto found;
				}
			}
			/* Not enough resources to support VMDq */
			break;
		}
	}
	/* Not enough resources to support VMDq */
	vpool = 0;
	vrxq = 0;
found:
	dev_info->max_vmdq_pools = vpool;
	dev_info->vmdq_queue_num = vrxq;

	dev_info->vmdq_pool_base = 0;
	dev_info->vmdq_queue_base = 0;
}

/* Configure the device based on the configuration provided */
static int bnxt_dev_configure_op(struct rte_eth_dev *eth_dev)
{
	struct bnxt *bp = (struct bnxt *)eth_dev->data->dev_private;
	int rc;

	bp->rx_queues = (void *)eth_dev->data->rx_queues;
	bp->tx_queues = (void *)eth_dev->data->tx_queues;

	/* Inherit new configurations */
	bp->rx_nr_rings = eth_dev->data->nb_rx_queues;
	bp->tx_nr_rings = eth_dev->data->nb_tx_queues;
	bp->rx_cp_nr_rings = bp->rx_nr_rings;
	bp->tx_cp_nr_rings = bp->tx_nr_rings;

	if (eth_dev->data->dev_conf.rxmode.jumbo_frame)
		eth_dev->data->mtu =
				eth_dev->data->dev_conf.rxmode.max_rx_pkt_len -
				ETHER_HDR_LEN - ETHER_CRC_LEN - VLAN_TAG_SIZE;
	rc = bnxt_set_hwrm_link_config(bp, true);
	return rc;
}

/*
 * Initialization
 */

static struct eth_dev_ops bnxt_dev_ops = {
	.dev_infos_get = bnxt_dev_info_get_op,
	.dev_close = bnxt_dev_close_op,
	.dev_configure = bnxt_dev_configure_op,
	.rx_queue_setup = bnxt_rx_queue_setup_op,
	.rx_queue_release = bnxt_rx_queue_release_op,
	.tx_queue_setup = bnxt_tx_queue_setup_op,
	.tx_queue_release = bnxt_tx_queue_release_op,
};

static bool bnxt_vf_pciid(uint16_t id)
{
	if (id == BROADCOM_DEV_ID_57304_VF ||
	    id == BROADCOM_DEV_ID_57406_VF)
		return true;
	return false;
}

static int bnxt_init_board(struct rte_eth_dev *eth_dev)
{
	int rc;
	struct bnxt *bp = eth_dev->data->dev_private;

	/* enable device (incl. PCI PM wakeup), and bus-mastering */
	if (!eth_dev->pci_dev->mem_resource[0].addr) {
		RTE_LOG(ERR, PMD,
			"Cannot find PCI device base address, aborting\n");
		rc = -ENODEV;
		goto init_err_disable;
	}

	bp->eth_dev = eth_dev;
	bp->pdev = eth_dev->pci_dev;

	bp->bar0 = (void *)eth_dev->pci_dev->mem_resource[0].addr;
	if (!bp->bar0) {
		RTE_LOG(ERR, PMD, "Cannot map device registers, aborting\n");
		rc = -ENOMEM;
		goto init_err_release;
	}
	return 0;

init_err_release:
	if (bp->bar0)
		bp->bar0 = NULL;

init_err_disable:

	return rc;
}

static int
bnxt_dev_init(struct rte_eth_dev *eth_dev)
{
	static int version_printed;
	struct bnxt *bp;
	int rc;

	if (version_printed++ == 0)
		RTE_LOG(INFO, PMD, "%s", bnxt_version);

	if (eth_dev->pci_dev->addr.function >= 2 &&
			eth_dev->pci_dev->addr.function < 4) {
		RTE_LOG(ERR, PMD, "Function not enabled %x:\n",
			eth_dev->pci_dev->addr.function);
		rc = -ENOMEM;
		goto error;
	}

	rte_eth_copy_pci_info(eth_dev, eth_dev->pci_dev);
	bp = eth_dev->data->dev_private;

	if (bnxt_vf_pciid(eth_dev->pci_dev->id.device_id))
		bp->flags |= BNXT_FLAG_VF;

	rc = bnxt_init_board(eth_dev);
	if (rc) {
		RTE_LOG(ERR, PMD,
			"Board initialization failed rc: %x\n", rc);
		goto error;
	}
	eth_dev->dev_ops = &bnxt_dev_ops;
	/* eth_dev->rx_pkt_burst = &bnxt_recv_pkts; */
	/* eth_dev->tx_pkt_burst = &bnxt_xmit_pkts; */

	rc = bnxt_alloc_hwrm_resources(bp);
	if (rc) {
		RTE_LOG(ERR, PMD,
			"hwrm resource allocation failure rc: %x\n", rc);
		goto error_free;
	}
	rc = bnxt_hwrm_ver_get(bp);
	if (rc)
		goto error_free;
	bnxt_hwrm_queue_qportcfg(bp);

	/* Get the MAX capabilities for this function */
	rc = bnxt_hwrm_func_qcaps(bp);
	if (rc) {
		RTE_LOG(ERR, PMD, "hwrm query capability failure rc: %x\n", rc);
		goto error_free;
	}
	eth_dev->data->mac_addrs = rte_zmalloc("bnxt_mac_addr_tbl",
					ETHER_ADDR_LEN * MAX_NUM_MAC_ADDR, 0);
	if (eth_dev->data->mac_addrs == NULL) {
		RTE_LOG(ERR, PMD,
			"Failed to alloc %u bytes needed to store MAC addr tbl",
			ETHER_ADDR_LEN * MAX_NUM_MAC_ADDR);
		rc = -ENOMEM;
		goto error_free;
	}
	/* Copy the permanent MAC from the qcap response address now. */
	if (BNXT_PF(bp))
		memcpy(bp->mac_addr, bp->pf.mac_addr, sizeof(bp->mac_addr));
	else
		memcpy(bp->mac_addr, bp->vf.mac_addr, sizeof(bp->mac_addr));
	memcpy(&eth_dev->data->mac_addrs[0], bp->mac_addr, ETHER_ADDR_LEN);

	rc = bnxt_hwrm_func_driver_register(bp, 0,
					    bp->pf.vf_req_fwd);
	if (rc) {
		RTE_LOG(ERR, PMD,
			"Failed to register driver");
		rc = -EBUSY;
		goto error_free;
	}

	RTE_LOG(INFO, PMD,
		DRV_MODULE_NAME " found at mem %" PRIx64 ", node addr %pM\n",
		eth_dev->pci_dev->mem_resource[0].phys_addr,
		eth_dev->pci_dev->mem_resource[0].addr);

	return 0;

error_free:
	eth_dev->driver->eth_dev_uninit(eth_dev);
error:
	return rc;
}

static int
bnxt_dev_uninit(struct rte_eth_dev *eth_dev) {
	struct bnxt *bp = eth_dev->data->dev_private;
	int rc;

	if (eth_dev->data->mac_addrs)
		rte_free(eth_dev->data->mac_addrs);
	rc = bnxt_hwrm_func_driver_unregister(bp, 0);
	bnxt_free_hwrm_resources(bp);
	return rc;
}

static struct eth_driver bnxt_rte_pmd = {
	.pci_drv = {
		    .name = "rte_" DRV_MODULE_NAME "_pmd",
		    .id_table = bnxt_pci_id_map,
		    .drv_flags = RTE_PCI_DRV_NEED_MAPPING,
		    },
	.eth_dev_init = bnxt_dev_init,
	.eth_dev_uninit = bnxt_dev_uninit,
	.dev_private_size = sizeof(struct bnxt),
};

static int bnxt_rte_pmd_init(const char *name, const char *params __rte_unused)
{
	RTE_LOG(INFO, PMD, "bnxt_rte_pmd_init() called for %s\n", name);
	rte_eth_driver_register(&bnxt_rte_pmd);
	return 0;
}

static struct rte_driver bnxt_pmd_drv = {
	.name = "eth_bnxt",
	.type = PMD_PDEV,
	.init = bnxt_rte_pmd_init,
};

PMD_REGISTER_DRIVER(bnxt_pmd_drv);