/*
 * Copyright (c) 2017 Eideticom Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *   Author: Stephen Bates <sbates@raithlin.com>
 *           Andrew Maier <andrew.maier@eideticom.com>
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "linux/nvme_ioctl.h"

#include "nvme.h"
#include "nvme-print.h"
#include "nvme-ioctl.h"
#include "plugin.h"
#include "json.h"

#include "argconfig.h"
#include "suffix.h"
#include <sys/ioctl.h>
#define CREATE_CMD
#include "eid-nvme.h"

static const char *dev = "/dev/";

struct eid_noload {
	__le32           acc_status;
	char             hls[12];
	char             acc_name[64];
	__le32           acc_ver;
	__le32           acc_cfg[6];
	__le32           acc_priv_len;
	char             acc_priv[3600];
};

static unsigned int eid_check_item(struct list_item *item)
{
	if (strstr(item->ctrl.mn, "Eideticom") == NULL)
		return 0;

	return 1;
}

static void eid_print_list_item(struct list_item list_item)
{
	struct eid_noload *eid =
		(struct eid_noload *) &list_item.ns.vs;

	printf("%-16s %-64.64s %-8.8u 0x%-8.8x\n", list_item.node,
	       eid->acc_name, (unsigned int) eid->acc_ver,
	       (unsigned int) eid->acc_status);
}

static void eid_print_list_items(struct list_item *list_items, unsigned int len)
{
	unsigned int i;

	printf("%-16s %-64s %-8s %-10s\n",
	       "Node", "Accelerator Name", "Version", "Status");
	printf("%-16s %-64s %-8s %-10s\n",
	       "----------------",
	       "----------------------------------------------------------------",
	       "--------", "----------");
	for (i = 0 ; i < len ; i++)
		eid_print_list_item(list_items[i]);
}

static void eid_show_nvme_id_ns_status(__le32 status)
{
	__u16 upper_rsvd = (status & 0xFFFE0000) >> 17;
	__u16 lower_rsvd = (status & 0x000000F8) >> 3;
	int as_en = status & 0x1;
	int as_rdrdy = (status & 0x2) >> 1;
	int as_wrrdy = (status & 0x4) >> 2;
	__u8 as_err = (status & 0xFF00) >> 8;
	int as_async = (status & 0x10000) >> 16;

	if (upper_rsvd)
		printf("\t[31:17]\t: 0x%x\tReserved\n", upper_rsvd);

	printf("\t[16:16]\t: %#x\tAsync Event Request ", as_async);
	if (as_async)
		printf("enabled ");
	else
		printf("disabled ");
	printf("(AS.ASYNC)\n");

	if (as_err)
		printf("\t[15:8]\t: %d\tError 0x%x occurred ", as_err, as_err);
	else
		printf("\t[15:8]\t: %d\tNo errors have been reported ", as_err);
	printf("(AS.ERR)\n");

	if (lower_rsvd)
		printf("\t[7:3]\t: 0x%x\tReserved\n", lower_rsvd);

	if (as_wrrdy) {
		printf("\t[2:2]\t: %d\tAccelerator is ready for "
		       "the next write command ", as_wrrdy);
	} else {
		printf("\t[2:2]\t: %d\tAccelerator is NOT ready for "
		       "the next write command ", as_wrrdy);
	}
	printf("(AS.WRRDY)\n");

	if (as_rdrdy)
		printf("\t[1:1]\t: %d\tAccelerator is ready for "
		       "the next read command ", as_rdrdy);
	else
		printf("\t[1:1]\t: %d\tAccelerator is NOT ready for "
		       "the next read command ", as_rdrdy);
	printf("(AS.RDRDY)\n");

	if (as_en)
		printf("\t[0:0]\t: %d\tAccelerator is enabled ", as_en);
	else
		printf("\t[0:0]\t: %d\tAccelerator is NOT enabled ", as_en);
	printf("(AS.EN)\n\n");
}

static void eid_show_nvme_id_ns(struct nvme_id_ns *ns, unsigned int mode)
{
	unsigned int i;
	int human = mode & HUMAN;
	struct eid_noload *eid =
		(struct eid_noload *) &ns->vs;

	printf("acc_name\t: %s\n", eid->acc_name);
	printf("acc_status\t: 0x%-8.8x\n", eid->acc_status);
	if (human)
		eid_show_nvme_id_ns_status(eid->acc_status);
	printf("acc_version\t: %-8.8d\n", eid->acc_ver);
	for (i = 0; i < 24/4; ++i)
		printf("acc_cfg[%d]\t: 0x%-8.8x\n", i, eid->acc_cfg[i]);
	printf("acc_spec_bytes\t: %d\n", eid->acc_priv_len);
	if (eid->acc_priv_len) {
		printf("acc_user_space\t:\n");
		d((unsigned char *)eid->acc_priv, eid->acc_priv_len, 16, 1);
	}
}

/*
 * List all the Eideticom namespaces in the system and identify the
 * accerlation functio provided by that namespace. We base this off
 * the Huawei code. Ideally we'd refactor this a bit. That is a TBD.
 */

static int eid_list(int argc, char **argv, struct command *command,
				struct plugin *plugin)
{
	char path[264];
	struct dirent **devices;
	struct list_item *list_items;
	unsigned int i, n, fd, ret, eid_num;
	int fmt;

	const char *desc = "Retrieve basic information for any Eideticom " \
		"namespaces in the systrem";
	struct config {
		char *output_format;
	};
	struct config cfg = {
		.output_format = "normal",
	};

	const struct argconfig_commandline_options opts[] = {
		{"output-format", 'o', "FMT", CFG_STRING, &cfg.output_format,
		 required_argument, "Output Format: normal|json"},
		{NULL}
	};

	argconfig_parse(argc, argv, desc, opts, &cfg, sizeof(cfg));
	fmt = validate_output_format(cfg.output_format);

	if (fmt == JSON) {
		fprintf(stderr, "json not yet supported for eid_list\n");
		return -EINVAL;
	}

	if (fmt != JSON && fmt != NORMAL)
		return -EINVAL;

	n = scandir(dev, &devices, scan_dev_filter, alphasort);
	if (n <= 0)
		return n;

	list_items = calloc(n, sizeof(*list_items));
	if (!list_items) {
		fprintf(stderr, "can not allocate controller list payload\n");
		return -ENOMEM;
	}

	eid_num = 0;
	for (i = 0; i < n; i++) {
		snprintf(path, sizeof(path), "%s%s", dev, devices[i]->d_name);
		fd = open(path, O_RDONLY);
		ret = get_nvme_info(fd, &list_items[eid_num], path);
		if (ret)
			return ret;
		if (eid_check_item(&list_items[eid_num]))
			eid_num++;
	}

	if (eid_num > 0)
		eid_print_list_items(list_items, eid_num);

	for (i = 0; i < n; i++)
		free(devices[i]);
	free(devices);
	free(list_items);

	return 0;
}

static int eid_id_ns(int argc, char **argv, struct command *command,
				 struct plugin *plugin)
{
	const char *desc = "Send an Identify Namespace command to the "\
		"given device, returns properties of the specified namespace. "	\
		"Fails on non-Eideticom namespaces.";
	const char *namespace_id = "identifier of desired namespace";
	const char *human_readable = "show infos in readable format";
	struct nvme_id_ns ns;
	struct stat nvme_stat;
	unsigned int flags = 0;

	int err, fd;

	struct config {
		__u32 namespace_id;
		int human_readable;
	};

	struct config cfg = {
		.namespace_id    = 0,
	};

	const struct argconfig_commandline_options command_line_options[] = {
		{"namespace-id", 'n', "NUM", CFG_POSITIVE, &cfg.namespace_id,
		 required_argument, namespace_id},
		{"human-readable", 'H', "", CFG_NONE, &cfg.human_readable,
		 no_argument, human_readable},
		{NULL}
	};

	fd = parse_and_open(argc, argv, desc, command_line_options,
			    &cfg, sizeof(cfg));
	if (fd < 0)
		return fd;

	err = fstat(fd, &nvme_stat);
	if (err < 0)
		return err;

	if (!cfg.namespace_id && S_ISBLK(nvme_stat.st_mode))
		cfg.namespace_id = nvme_get_nsid(fd);
	else if (!cfg.namespace_id)
		fprintf(stderr,
			"Error: requesting namespace-id from non-block device\n");

	if (cfg.human_readable)
		flags |= HUMAN;

	err = nvme_identify_ns(fd, cfg.namespace_id, 0, &ns);
	if (!err) {
		printf("NVME Identify Namespace %d:\n", cfg.namespace_id);
		eid_show_nvme_id_ns(&ns, flags);
	} else if (err > 0) {
		fprintf(stderr, "NVMe Status:%s(%x) NSID:%d\n",
			nvme_status_to_string(err), err, cfg.namespace_id);
	} else {
		perror("identify namespace");
	}
	return err;
}
