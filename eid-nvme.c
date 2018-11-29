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

struct eid_idns_noload {
	__le32	acc_status;
	char	hls[12];
	char	acc_name[48];
	__le32	acc_lock;
	char	reserved[12];
	__le32	acc_ver;
	__le32	acc_cfg[6];
	__le32	acc_priv_len;
	char	acc_priv[3600];
};

struct eid_idctrl_noload {
	__le32	hw_build_date;
	__le32  fw_build_date;
    __le32  hw_system_ver;
};

static unsigned int eid_check_item(struct list_item *item)
{
	if (strstr(item->ctrl.mn, "Eideticom") == NULL)
		return 0;

	return 1;
}

static void eid_print_list_item(struct list_item list_item)
{
	struct eid_idns_noload *eid_idns =
		(struct eid_idns_noload *) &list_item.ns.vs;

	printf("%-16s %-64.64s 0x%-8.8x 0x%-8.8x\n", list_item.node,
	       eid_idns->acc_name, (unsigned int) eid_idns->acc_ver,
	       (unsigned int) eid_idns->acc_status);
}

static void eid_print_list_items(struct list_item *list_items, unsigned int len)
{
	unsigned int i;

	printf("%-16s %-64s %-10s %-10s\n",
	       "Node", "Accelerator Name", "Version", "Status");
	printf("%-16s %-64s %-10s %-10s\n",
	       "----------------",
	       "----------------------------------------------------------------",
	       "----------", "----------");
	for (i = 0 ; i < len ; i++)
		eid_print_list_item(list_items[i]);
}

static void eid_show_id_ns_vs_status(__le32 status)
{
	int as_en = status & 0x1;
	int as_rdrdy = (status & 0x2) >> 1;
	int as_wrrdy = (status & 0x4) >> 2;
	__u8 lower_rsvd = (status & 0x000000F8) >> 3;
	__u8 as_sc = (status & 0xFF00) >> 8;
	int as_sen = (status & 0x10000) >> 16;
	int as_bre = (status & 0x20000) >> 17;
	int as_bwe = (status & 0x40000) >> 18;
	__u8 as_inver = (status & 0x780000) >> 19;
	int as_ioe =   (status & 0x00800000) >> 23;
	__u8 upper_rsvd = (status & 0x0F000000) >> 24;
	int as_rdone = (status & 0x10000000) >> 28;
	int as_srdy =  (status & 0x20000000) >> 29;
	int as_rdack = (status & 0x40000000) >> 30;
	int as_wrack = (status & 0x80000000) >> 31;

	printf("\t[31:31]\t: %d\tWrite acknowledge clear\n", as_wrack);
	printf("\t[30:30]\t: %d\tRead acknowledge clear\n", as_rdack);
	printf("\t[29:29]\t: %d\tStatus Ready\n", as_srdy);
	printf("\t[28:28]\t: %d\tRead Done\n", as_rdone);
	if (upper_rsvd)
		printf("\t[27:24]\t: 0x%x\tReserved\n", upper_rsvd);

	printf("\t[23:23]\t: %d\tIn-Order Enable\n", as_ioe);
	printf("\t[22:19]\t: %d\tAccelerator Interface Version\n", as_inver);

	printf("\t[18:18]\t: %d\tBlocking write functionality is ", as_bwe);
	if (as_bwe)
		printf("enabled ");
	else
		printf("NOT enabled ");
	printf("(AS.BWE)\n");

	printf("\t[17:17]\t: %d\tBlocking read functionality is ", as_bre);
	if (as_bre)
		printf("enabled ");
	else
		printf("NOT enabled ");
	printf("(AS.BRE)\n");

	printf("\t[16:16]\t: %d\tStatus code field is ", as_sen);
	if (as_sen)
		printf("enabled ");
	else
		printf("disabled ");
	printf("(AS.SEN)\n");

	if (as_sen) {
		if (as_sc)
			printf("\t[15:8]\t: %d\tStatus code 0x%x occurred "
				"(AS.SC)\n", as_sc, as_sc);
		else
			printf("\t[15:8]\t: %d\tNo status code has been "
			"reported (AS.SC)\n", as_sc);
	}

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

static void json_eid_show_id_ns_vs(struct eid_idns_noload *eid)
{
	unsigned int i;
	struct json_object *root;
	struct json_array *acc_cfg;

	root = json_create_object();

	json_object_add_value_string(root, "acc_name", eid->acc_name);
	json_object_add_value_uint(root, "acc_status", eid->acc_status);
	json_object_add_value_uint(root, "acc_lock", eid->acc_lock);
	json_object_add_value_uint(root, "acc_version", eid->acc_ver);

	acc_cfg = json_create_array();

	for (i = 0; i < 24/4; ++i)
		json_array_add_value_uint(acc_cfg, eid->acc_cfg[i]);

	json_object_add_value_array(root, "acc_cfg", acc_cfg);
	json_object_add_value_uint(root, "acc_spec_bytes", eid->acc_priv_len);

	// TBD: Add something here for acc_user_space?

	json_print_object(root, NULL);
	printf("\n");
	json_free_object(root);
}

static void eid_show_id_ns_vs(struct eid_idns_noload *eid, int human)
{
	unsigned int i;
	printf("acc_name\t: %s\n", eid->acc_name);
	printf("acc_status\t: 0x%-8.8x\n", eid->acc_status);
	if (human)
		eid_show_id_ns_vs_status(eid->acc_status);
	printf("acc_lock\t: 0x%-8.8x", eid->acc_lock);
	if (human && eid->acc_lock)
		printf("\tAccelerator is locked with lock 0x%x\n", eid->acc_lock);
	else if (human && !eid->acc_lock)
		printf("\tAccelerator is NOT locked\n");
	else
		printf("\n");
	printf("acc_version\t: 0x%-8.8x\n", eid->acc_ver);
	for (i = 0; i < 24/4; ++i)
		printf("acc_cfg[%d]\t: 0x%-8.8x\n", i, eid->acc_cfg[i]);
	printf("acc_spec_bytes\t: %d\n", eid->acc_priv_len);
	if (eid->acc_priv_len) {
		printf("acc_user_space\t:\n");
		d((unsigned char *)eid->acc_priv, eid->acc_priv_len, 16, 1);
	}
}

static void eid_id_ns_vs(struct eid_idns_noload *eid, __u32 nsid, unsigned int mode, int fmt)
{
	int human = mode & HUMAN;

	if (fmt == JSON)
		json_eid_show_id_ns_vs(eid);
	else {
		printf("NVME Identify Namespace %d:\n", nsid);
		eid_show_id_ns_vs(eid, human);
	}
}

static void json_eid_show_id_ctrl_vs(struct eid_idctrl_noload *eid_idctrl, char *hw_build_str, 
									 char *fw_build_str, char *hw_arch_str, char *hw_ver_str, int human)
{
	struct json_object *root;
	root = json_create_object();

	if (human) {
		json_object_add_value_string(root, "hw_build_date", hw_build_str);
		json_object_add_value_string(root, "fw_build_date", fw_build_str);
		json_object_add_value_string(root, "hw_system_version", hw_ver_str);
	}
	else {
		json_object_add_value_uint(root, "hw_build_date", le32_to_cpu(eid_idctrl->hw_build_date));
		json_object_add_value_uint(root, "fw_build_date", le32_to_cpu(eid_idctrl->fw_build_date));
		json_object_add_value_uint(root, "hw_system_version", le32_to_cpu(eid_idctrl->hw_system_ver));
	}

	json_print_object(root, NULL);
	printf("\n");
	json_free_object(root);
}

static void eid_show_id_ctrl_vs(struct eid_idctrl_noload *eid_idctrl, char *hw_build_str, 
								char *fw_build_str, char *hw_arch_str, char *hw_ver_str, int human)
{
	if (human) {
		printf("hw_build_date\t\t: %s\n", hw_build_str);
		printf("fw_build_date\t\t: %s\n", fw_build_str);
		printf("hw_system_version\t: %s\n", hw_ver_str);
	} else {
		printf("hw_build_date\t\t: 0x%-8.8x\n", eid_idctrl->hw_build_date);
		printf("fw_build_date\t\t: 0x%-8.8x\n", eid_idctrl->fw_build_date);
		printf("hw_system_version\t: 0x%-8.8x\n", eid_idctrl->hw_system_ver);
	}
}

static void eid_nvme_id_ctrl_vs(struct eid_idctrl_noload *eid_idctrl, unsigned int mode, int fmt)
{
	int human = mode & HUMAN;
	char hw_build_str[21];
	char fw_build_str[21];
    char hw_arch_str[18];
    char hw_ver_str[23];

	// Pull out the data
	unsigned int hw_day    =  ((eid_idctrl->hw_build_date >> 27) & 0x1Fu);
	unsigned int hw_month  =  ((eid_idctrl->hw_build_date >> 23) & 0x0Fu);
	unsigned int hw_year   =  ((eid_idctrl->hw_build_date >> 17) & 0x3Fu);
	unsigned int hw_hour   =  ((eid_idctrl->hw_build_date >> 12) & 0x1Fu);
	unsigned int hw_minute =  ((eid_idctrl->hw_build_date >> 6) & 0x3Fu);
	unsigned int hw_second =  ((eid_idctrl->hw_build_date >> 0) & 0x3Fu);

	unsigned int fw_day    =  ((eid_idctrl->fw_build_date >> 27) & 0x1Fu);
	unsigned int fw_month  =  ((eid_idctrl->fw_build_date >> 23) & 0x0Fu);
	unsigned int fw_year   =  ((eid_idctrl->fw_build_date >> 17) & 0x3Fu);
	unsigned int fw_hour   =  ((eid_idctrl->fw_build_date >> 12) & 0x1Fu);
	unsigned int fw_minute =  ((eid_idctrl->fw_build_date >> 6) & 0x3Fu);
	unsigned int fw_second =  ((eid_idctrl->fw_build_date >> 0) & 0x3Fu);

	sprintf(hw_build_str, "%04d-%02d-%02d %02d:%02d:%02d", hw_year+2000, hw_month,
		hw_day, hw_hour, hw_minute, hw_second);

	sprintf(fw_build_str, "%04d-%02d-%02d %02d:%02d:%02d", fw_year+2000, fw_month,
		fw_day, fw_hour, fw_minute, fw_second);

    unsigned int hw_arch_type = ((eid_idctrl->hw_system_ver >> 8) & 0xFF00u);
    unsigned int hw_version   = ((eid_idctrl->hw_system_ver >> 0) & 0x00FFu);

	switch (hw_arch_type) {
		case 1:
			strncpy(hw_arch_str, "NVMe Single Core", sizeof(hw_arch_str));
			break;
		case 2:
			strncpy(hw_arch_str, "NVMe Multi Core", sizeof(hw_arch_str));
			break;
		default:
			strncpy(hw_arch_str, "Unknown", sizeof(hw_arch_str));
			break;
	}

	switch (hw_version) {
		case 1:
			strncpy(hw_ver_str, "Flash GT Plus (250sp)", sizeof(hw_ver_str));
			break;
		case 2:
			strncpy(hw_ver_str, "AlphaData 9v3", sizeof(hw_ver_str));
			break;
		case 3:
			strncpy(hw_ver_str, "Bittware U2 Series 1", sizeof(hw_ver_str));
			break;
		case 4:
			strncpy(hw_ver_str, "Bittware U2 Series 2", sizeof(hw_ver_str));
			break;
		case 5:
			strncpy(hw_ver_str, "Xilinx VCU1525 v1.1", sizeof(hw_ver_str));
			break;
		default:
			strncpy(hw_ver_str, "Unknown", sizeof(hw_ver_str));
			break;
	}

	if (fmt == JSON)
		json_eid_show_id_ctrl_vs(eid_idctrl, hw_build_str, fw_build_str, hw_arch_str, hw_ver_str, human);
	else {
		printf("Eideticom NVME Identify Controller:\n");
		eid_show_id_ctrl_vs(eid_idctrl, hw_build_str, fw_build_str, hw_arch_str, hw_ver_str, human);
	}
}

/*
 * List all the Eideticom namespaces in the system and identify the
 * accerlation function provided by that namespace. We base this off
 * the Huawei code. Ideally we'd refactor this a bit. That is a TBD.
 * TBD: The code in scan_device_filter has been made static in 
 * nvme.c linux-nvme. We'll need to either make our own copy before
 * pull requesting upstream.
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

static int eid_id_ctrl(int argc, char **argv, struct command *command, 
				 struct plugin *plugin)
{
	const char *desc = "Send an Identify Controller command to the "\
		"given device, returns vendor specific properties of the " \
		"specified namespace. Fails on non-Eideticom namespaces.";
	const char *human_readable = "show infos in readable format";
	const char *output_format = "Output format: normal|json";
	struct nvme_id_ctrl ctrl;
	struct stat nvme_stat;
	unsigned int flags = 0;

	int err, fmt, fd;

	struct config {
		int human_readable;
		char *output_format;
	};

	struct config cfg = {
		.output_format = "normal",
	};

	const struct argconfig_commandline_options command_line_options[] = {
		{"human-readable", 'H', "", CFG_NONE, &cfg.human_readable,
		 no_argument, human_readable},
		{"output-format", 'o', "FMT", CFG_STRING, &cfg.output_format,
		 required_argument, output_format},
		{NULL}
	};

	fd = parse_and_open(argc, argv, desc, command_line_options,
			    &cfg, sizeof(cfg));
	if (fd < 0)
		return fd;

	fmt = validate_output_format(cfg.output_format);
	if (fmt < 0) {
		err = fmt;
		goto close_fd;
	}

	err = fstat(fd, &nvme_stat);
	if (err < 0)
		return err;

	if (cfg.human_readable)
		flags |= HUMAN;

	err = nvme_identify_ctrl(fd, &ctrl);
	if (!err) {
		eid_nvme_id_ctrl_vs((struct eid_idctrl_noload *) &ctrl.vs, flags, fmt);
	} else if (err > 0) {
		fprintf(stderr, "NVMe Status:%s(%x)\n",
			nvme_status_to_string(err), err);
	} else {
		perror("identify controller");
	}

close_fd:
	close(fd);

	return err;
	
}

static int eid_id_ns(int argc, char **argv, struct command *command,
				 struct plugin *plugin)
{
	const char *desc = "Send an Identify Namespace command to the "\
		"given device, returns vendor specific properties of the " \
		"specified namespace. Fails on non-Eideticom namespaces.";
	const char *human_readable = "show infos in readable format";
	const char *namespace_id = "identifier of desired controller";
	const char *output_format = "Output format: normal|json";
	struct nvme_id_ns ns;
	struct stat nvme_stat;
	unsigned int flags = 0;

	int err, fmt, fd;

	struct config {
		__u32 namespace_id;
		int human_readable;
		char *output_format;
	};

	struct config cfg = {
		.namespace_id    = 0,
		.output_format   = "normal",
	};

	const struct argconfig_commandline_options command_line_options[] = {
		{"namespace-id", 'n', "NUM", CFG_POSITIVE, &cfg.namespace_id,
		 required_argument, namespace_id},
		{"human-readable", 'H', "", CFG_NONE, &cfg.human_readable,
		 no_argument, human_readable},
		{"output-format", 'o', "FMT", CFG_STRING, &cfg.output_format,
		 required_argument, output_format},
		{NULL}
	};

	fd = parse_and_open(argc, argv, desc, command_line_options,
			    &cfg, sizeof(cfg));
	if (fd < 0)
		return fd;

	fmt = validate_output_format(cfg.output_format);
	if (fmt < 0) {
		err = fmt;
		goto close_fd;
	}

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
		eid_id_ns_vs((struct eid_idns_noload *) &ns.vs, cfg.namespace_id, flags, fmt);
	} else if (err > 0) {
		fprintf(stderr, "NVMe Status:%s(%x) NSID:%d\n",
			nvme_status_to_string(err), err, cfg.namespace_id);
	} else {
		perror("identify namespace");
	}

close_fd:
	close(fd);

	return err;
}
