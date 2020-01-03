/*
 * Copyright (c) 2015-2017 Intel Corporation. All rights reserved.
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

#include <rdma/fi_errno.h>

#include <ofi_prov.h>
#include "smr.h"
#include "smr_signal.h"


static void smr_resolve_addr(const char *node, const char *service,
			     char **addr, size_t *addrlen)
{
	char temp_name[NAME_MAX];

	if (service) {
		if (node)
			snprintf(temp_name, NAME_MAX - 1, "%s%s:%s",
				 SMR_PREFIX_NS, node, service);
		else
			snprintf(temp_name, NAME_MAX - 1, "%s%s",
				 SMR_PREFIX_NS, service);
	} else {
		if (node)
			snprintf(temp_name, NAME_MAX - 1, "%s%s",
				 SMR_PREFIX, node);
		else
			snprintf(temp_name, NAME_MAX - 1, "%s%d",
				 SMR_PREFIX, getpid());
	}

	*addr = strdup(temp_name);
	*addrlen = strlen(*addr) + 1;
	(*addr)[*addrlen - 1]  = '\0';
}

static int smr_get_ptrace_scope(void)
{
	FILE *file;
	int scope, ret;

	scope = 0;
	file = fopen("/proc/sys/kernel/yama/ptrace_scope", "r");
	if (file) {
		ret = fscanf(file, "%d", &scope);
		if (ret != 1) {
			FI_WARN(&smr_prov, FI_LOG_CORE,
				"Error getting value from ptrace_scope\n");
			return -FI_EINVAL;
		}
		ret = fclose(file);
		if (ret) {
			FI_WARN(&smr_prov, FI_LOG_CORE,
				"Error closing ptrace_scope file\n");
			return -FI_EINVAL;
		}
	}
	return scope;
}

static int smr_getinfo(uint32_t version, const char *node, const char *service,
		       uint64_t flags, const struct fi_info *hints,
		       struct fi_info **info)
{
	struct fi_info *cur;
	uint64_t mr_mode, msg_order;
	int fast_rma;
	int ptrace_scope, ret;

	mr_mode = hints && hints->domain_attr ? hints->domain_attr->mr_mode :
						FI_MR_VIRT_ADDR;
	msg_order = hints && hints->tx_attr ? hints->tx_attr->msg_order : 0;
	fast_rma = smr_fast_rma_enabled(mr_mode, msg_order);

	ret = util_getinfo(&smr_util_prov, version, node, service, flags,
			   hints, info);
	if (ret)
		return ret;

	ptrace_scope = smr_get_ptrace_scope();

	for (cur = *info; cur; cur = cur->next) {
		if (!(flags & FI_SOURCE) && !cur->dest_addr)
			smr_resolve_addr(node, service, (char **) &cur->dest_addr,
					 &cur->dest_addrlen);

		if (!cur->src_addr) {
			if (flags & FI_SOURCE)
				smr_resolve_addr(node, service, (char **) &cur->src_addr,
						 &cur->src_addrlen);
			else
				smr_resolve_addr(NULL, NULL, (char **) &cur->src_addr,
						 &cur->src_addrlen);
		}
		if (fast_rma) {
			cur->domain_attr->mr_mode = FI_MR_VIRT_ADDR;
			cur->tx_attr->msg_order = FI_ORDER_SAS;
			cur->ep_attr->max_order_raw_size = 0;
			cur->ep_attr->max_order_waw_size = 0;
			cur->ep_attr->max_order_war_size = 0;
		}
		if (ptrace_scope != 0)
			cur->ep_attr->max_msg_size = SMR_INJECT_SIZE;
	}
	return 0;
}

static void smr_fini(void)
{
	struct smr_ep_name *ep_name;
	struct dlist_entry *tmp;

	dlist_foreach_container_safe(&ep_name_list, struct smr_ep_name,
				     ep_name, entry, tmp) {
		free(ep_name);
	}
}

struct fi_provider smr_prov = {
	.name = "shm",
	.version = FI_VERSION(SMR_MAJOR_VERSION, SMR_MINOR_VERSION),
	.fi_version = OFI_VERSION_LATEST,
	.getinfo = smr_getinfo,
	.fabric = smr_fabric,
	.cleanup = smr_fini
};

struct util_prov smr_util_prov = {
	.prov = &smr_prov,
	.info = &smr_info,
	.flags = 0
};

SHM_INI
{
	dlist_init(&ep_name_list);

	/* Signal handlers to cleanup tmpfs files on an unclean shutdown */
	smr_reg_sig_hander(SIGBUS);
	smr_reg_sig_hander(SIGSEGV);
	smr_reg_sig_hander(SIGTERM);
	smr_reg_sig_hander(SIGINT);

	return &smr_prov;
}