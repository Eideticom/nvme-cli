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
	union {
		struct idns_v4 {
			__le32	acc_status;
			char	hls[12];
			char	acc_name[48];
			__le32	acc_lock;
			char	reserved[12];
			__le32	acc_ver;
			__le32	acc_cfg[6];
			__le32	acc_priv_len;
			char	acc_priv[3600];
		} v4;
		struct idns_v6 {
			__le32	ns_type;
			char	ns_name[48];
			__le32	ns_ver;
			__le32	ns_features;
			char	reserved[192];
			__le32	ns_num_accels;
			struct eid_idns_accel {
				__le32	acc_status;
				__le32	acc_job_state;
				__le32	acc_handshake;
				char	reserved[8];
				__le32	acc_spec_len;
				char	acc_spec[104];
			} ns_accels[27];
		} v6;
	};
};

struct eid_idctrl_noload {
	__le32	hw_build_date;
	__le32  fw_build_date;
    __le32  hw_system_ver;
	__le32	work_item;
	__le32  fw_commit_sha[5];
	__le32	hw_commit_sha[5];
	__le32  compiled_fw_commit_sha[5];
	__le32  job_id_count;
};

enum {
	NOLOAD_VER_4 = 1,
	NOLOAD_VER_6 = 2,
	NUM_VERSIONS
};

static unsigned int eid_check_item(struct list_item *item) {
	if (strstr(item->ctrl.mn, "Eideticom") == NULL)
		return 0;

	return 1;
}

static unsigned int get_eid_ver(struct list_item *item) {
	unsigned int ver;
	sscanf(item->ctrl.fr, "%d", &ver);
	return ver;
}

static void eid_print_list_v4(struct list_item *list_items, unsigned int len) {
	unsigned int i;

	printf("NoLoad Version 4 Cards:\n");
	printf("%-16s %-64s %-10s %-10s\n",
	       "Node", "Accelerator Name", "Version", "Status");
	printf("%-16s %-64s %-10s %-10s\n",
	       "----------------",
	       "----------------------------------------------------------------",
	       "----------", "----------");

	for (i = 0 ; i < len ; i++) {
		struct eid_idns_noload *eid_idns =
		(struct eid_idns_noload *) &list_items[i].ns.vs;
		if (eid_check_item(&list_items[i]) && get_eid_ver(&list_items[i]) == NOLOAD_VER_4) {
			printf("%-16s %-64.64s 0x%-8.8x 0x%-8.8x\n", list_items[i].node,
				eid_idns->v4.acc_name, (unsigned int) eid_idns->v4.acc_ver,
				(unsigned int) eid_idns->v4.acc_status);
		}
	}
}

static void eid_print_list_v6(struct list_item *list_items, unsigned int len) {
	unsigned int i;

	printf("NoLoad Version 6 Cards:\n");
	printf("%-16s %-64s %-10s %-10s\n",
	       "Node", "Accelerator Name", "Version", "Number of Accelerators");
	printf("%-16s %-64s %-10s %-10s\n",
	       "----------------",
	       "----------------------------------------------------------------",
	       "----------", "----------");

	for (i = 0 ; i < len ; i++) {
		struct eid_idns_noload *eid_idns =
		(struct eid_idns_noload *) &list_items[i].ns.vs;
		if (eid_check_item(&list_items[i]) && get_eid_ver(&list_items[i]) == NOLOAD_VER_6) {
			printf("%-16s %-64.64s 0x%-8.8x %d\n", list_items[i].node,
				eid_idns->v6.ns_name, (unsigned int) eid_idns->v6.ns_ver,
				(unsigned int) eid_idns->v6.ns_num_accels);
		}
	}
}

static void eid_print_list(struct list_item *list_items, unsigned int len)
{
	unsigned int i;
	unsigned int num_noloads[NUM_VERSIONS];
	unsigned int ver;

	for(i = 0; i < NUM_VERSIONS; i++)
		num_noloads[i] = 0;

	// First let's traverse the list and find all versions
	for(i = 0; i < len; i++) {
		ver = get_eid_ver(&list_items[i]);
		if (ver < NUM_VERSIONS)
			num_noloads[ver]++;
		else {
			fprintf(stderr, "error: unrecognized NoLoad type %u", ver);
			return;
		}
	}

	if (num_noloads[NOLOAD_VER_4] > 0)
		eid_print_list_v4(list_items, len);
	
	if (num_noloads[NOLOAD_VER_6] > 0) {
		if (num_noloads[NOLOAD_VER_4] > 0)
			printf("\n");
		eid_print_list_v6(list_items, len);
	}
}

static void eid_show_id_ns_vs_status(__le32 status)
{
	int as_en = status & 0x1;
	int as_rdrdy = (status & 0x2) >> 1;
	int as_wrrdy = (status & 0x4) >> 2;
	__u8 lower_rsvd = (status & 0x000000F8) >> 3;
	__u8 as_sc = (status & 0xFF00) >> 8;
	int as_sen = (status & 0x10000) >> 16;
	int as_be = (status & 0x20000) >> 17;
	int mid_rsvd = (status & 0x40000) >> 18;
	__u8 as_inver = (status & 0x780000) >> 19;
	int as_sjob =   (status & 0x00800000) >> 23;
	int as_aow = 	(status & 0x01000000) >> 24;
	__u8 upper_rsvd = (status & 0x0E000000) >> 25;
	int as_rdone = (status & 0x10000000) >> 28;
	int as_srdy =  (status & 0x20000000) >> 29;
	int as_rdack = (status & 0x40000000) >> 30;
	int as_wrack = (status & 0x80000000) >> 31;

	printf("\t\t[31:31]\t: %d\tWrite acknowledge clear\n", as_wrack);
	printf("\t\t[30:30]\t: %d\tRead acknowledge clear\n", as_rdack);
	printf("\t\t[29:29]\t: %d\tStatus Ready\n", as_srdy);
	printf("\t\t[28:28]\t: %d\tRead Done\n", as_rdone);
	if (upper_rsvd)
		printf("\t\t[27:24]\t: 0x%x\tReserved\n", upper_rsvd);

	printf("\t\t[24:24]\t: %d\tAllow Overprovisioned Writes\n", as_aow);
	printf("\t\t[23:23]\t: %d\tSingle Job Enable\n", as_sjob);
	printf("\t\t[22:19]\t: %d\tAccelerator Interface Version\n", as_inver);
	if (mid_rsvd)
		printf("\t\t[18:18]\t: 0x%x\tReserved\n", mid_rsvd);

	printf("\t\t[17:17]\t: %d\tBlocking functionality is ", as_be);
	if (as_be)
		printf("enabled ");
	else
		printf("NOT enabled ");
	printf("(AS.BE)\n");

	printf("\t\t[16:16]\t: %d\tStatus code field is ", as_sen);
	if (as_sen)
		printf("enabled ");
	else
		printf("disabled ");
	printf("(AS.SEN)\n");

	if (as_sen) {
		if (as_sc)
			printf("\t\t[15:8]\t: %d\tStatus code 0x%x occurred "
				"(AS.SC)\n", as_sc, as_sc);
		else
			printf("\t\t[15:8]\t: %d\tNo status code has been "
			"reported (AS.SC)\n", as_sc);
	}

	if (lower_rsvd)
		printf("\t\t[7:3]\t: 0x%x\tReserved\n", lower_rsvd);

	if (as_wrrdy) {
		printf("\t\t[2:2]\t: %d\tAccelerator is ready for "
			"the next write command ", as_wrrdy);
	} else {
		printf("\t\t[2:2]\t: %d\tAccelerator is NOT ready for "
			"the next write command ", as_wrrdy);
	}
	printf("(AS.WRRDY)\n");

	if (as_rdrdy)
		printf("\t\t[1:1]\t: %d\tAccelerator is ready for "
			"the next read command ", as_rdrdy);
	else
		printf("\t\t[1:1]\t: %d\tAccelerator is NOT ready for "
			"the next read command ", as_rdrdy);
	printf("(AS.RDRDY)\n");

	if (as_en)
		printf("\t\t[0:0]\t: %d\tAccelerator is enabled ", as_en);
	else
		printf("\t\t[0:0]\t: %d\tAccelerator is NOT enabled ", as_en);
	printf("(AS.EN)\n\n");
}

static void json_eid_show_id_ns_vs_v4(struct eid_idns_noload *eid)
{
	unsigned int i;
	struct json_object *root;
	struct json_array *acc_cfg;

	root = json_create_object();

	json_object_add_value_string(root, "acc_name", eid->v4.acc_name);
	json_object_add_value_uint(root, "acc_status", eid->v4.acc_status);
	json_object_add_value_uint(root, "acc_lock", eid->v4.acc_lock);
	json_object_add_value_uint(root, "acc_version", eid->v4.acc_ver);

	acc_cfg = json_create_array();

	for (i = 0; i < 24/4; ++i)
		json_array_add_value_uint(acc_cfg, eid->v4.acc_cfg[i]);

	json_object_add_value_array(root, "acc_cfg", acc_cfg);
	json_object_add_value_uint(root, "acc_spec_bytes", eid->v4.acc_priv_len);

	// TBD: Add something here for acc_user_space?

	json_print_object(root, NULL);
	printf("\n");
	json_free_object(root);
}

static void json_eid_show_id_ns_vs_v6(struct eid_idns_noload *eid)
{
	unsigned int i;
	struct json_object *root;
	struct json_object *accel_info;
	struct json_array *accels_list;
	struct eid_idns_accel *accel;


	root = json_create_object();

	json_object_add_value_uint(root, "ns_type", eid->v6.ns_type);
	json_object_add_value_string(root, "ns_name", eid->v6.ns_name);
	json_object_add_value_uint(root, "ns_ver", eid->v6.ns_ver);
	json_object_add_value_uint(root, "ns_num_accels", eid->v6.ns_num_accels);
	json_object_add_value_uint(root, "ns_features", eid->v6.ns_features);

	if (eid->v6.ns_num_accels > 27 || eid->v6.ns_num_accels < 1) {
		perror("ns_num_accels not valid (must be between 1 and 27)");
		goto free_json;
	}

	accels_list = json_create_array();
	for (i=0; i < eid->v6.ns_num_accels; ++i) {
		accel_info = json_create_object();
		accel = &eid->v6.ns_accels[0];
		json_object_add_value_uint(accel_info, "accel_status", accel->acc_status);
		json_object_add_value_uint(accel_info, "accel_job_state", accel->acc_job_state);
		json_object_add_value_uint(accel_info, "accel_handshake", accel->acc_handshake);
		json_object_add_value_uint(accel_info, "accel_spec_len", accel->acc_spec_len);
		json_array_add_value_object(accels_list, accel_info);
	}
	json_object_add_value_array(root, "accel_info", accels_list);
	
	json_print_object(root, NULL);
	printf("\n");
free_json:
	json_free_object(root);
}

static void eid_show_id_ns_vs_v6_accel(struct eid_idns_accel *accel, int human)
{
	printf("\taccel_status\t: 0x%-8.8x\n", accel->acc_status);
	if (human)
		eid_show_id_ns_vs_status(accel->acc_status);
	printf("\taccel_job_state\t: 0x%-8.8x\n", accel->acc_job_state);
	printf("\taccel_handshake\t: 0x%-8.8x\n", accel->acc_handshake);
	printf("\taccel_spec_len\t: %d\n", accel->acc_spec_len);
	if (accel->acc_spec_len)
		d((unsigned char *)accel->acc_spec, accel->acc_spec_len, 16, 1);
}

static void eid_show_id_ns_vs_v4(struct eid_idns_noload *eid, int human) {
	unsigned int i;

	printf("acc_name\t: %s\n", eid->v4.acc_name);
	printf("acc_status\t: 0x%-8.8x\n", eid->v4.acc_status);
	if (human)
		eid_show_id_ns_vs_status(eid->v4.acc_status);
	printf("acc_lock\t: 0x%-8.8x", eid->v4.acc_lock);
	if (human && eid->v4.acc_lock)
		printf("\tAccelerator is locked with lock 0x%x\n", eid->v4.acc_lock);
	else if (human && !eid->v4.acc_lock)
		printf("\tAccelerator is NOT locked\n");
	else
		printf("\n");
	printf("acc_version\t: 0x%-8.8x\n", eid->v4.acc_ver);
	for (i = 0; i < 24/4; ++i)
		printf("acc_cfg[%d]\t: 0x%-8.8x\n", i, eid->v4.acc_cfg[i]);
	printf("acc_spec_bytes\t: %d\n", eid->v4.acc_priv_len);
	if (eid->v4.acc_priv_len) {
		printf("acc_user_space\t:\n");
		d((unsigned char *)eid->v4.acc_priv, eid->v4.acc_priv_len, 16, 1);
	}
}

static void eid_show_id_ns_vs_v6(struct eid_idns_noload *eid, int human) {
	unsigned int i;
	
	printf("ns_type\t\t: 0x%-8.8x\n", eid->v6.ns_type);
	printf("ns_name\t\t: %s\n", eid->v6.ns_name);
	printf("ns_ver\t\t: 0x%-8.8x\n", eid->v6.ns_ver);
	printf("ns_features\t: 0x%-8.8x\n", eid->v6.ns_features);
	printf("ns_num_accels\t: %d\n", eid->v6.ns_num_accels);
	if (eid->v6.ns_num_accels > 8 || eid->v6.ns_num_accels < 1) {
		perror("ns_num_accels not valid (must be between 1 and 8)");
		return;
	}
	for (i=0; i < eid->v6.ns_num_accels; ++i) {
		printf("Accelerator %d:\n", i);
		eid_show_id_ns_vs_v6_accel(&eid->v6.ns_accels[i], human);
	}
}

static void eid_id_ns_vs(struct eid_idns_noload *eid, __u32 nsid, unsigned int noload_ver, unsigned int mode, int fmt)
{
	int human = mode & HUMAN;

	if (fmt == JSON)
		if (noload_ver == NOLOAD_VER_4)
			json_eid_show_id_ns_vs_v4(eid);
		else if (noload_ver == NOLOAD_VER_6)
			json_eid_show_id_ns_vs_v6(eid);
		else
			fprintf(stderr, "error: unrecognized NoLoad version %u", noload_ver); 
	else {
		printf("NVME Identify Namespace %d:\n", nsid);
		if (noload_ver == NOLOAD_VER_4)
			eid_show_id_ns_vs_v4(eid, human);
		else if (noload_ver == NOLOAD_VER_6)
			eid_show_id_ns_vs_v6(eid, human);
		else
			fprintf(stderr, "error: unrecognized NoLoad version %u", noload_ver); 
	}
}

static void json_eid_show_id_ctrl_vs(struct eid_idctrl_noload *eid_idctrl, char *hw_build_str, 
									 char *fw_build_str, char *hw_ver_str, char *fw_commit_str, 
									 char *hw_commit_str, char *compiled_fw_commit_str, char *sys_ver, 
									 int human)
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

	json_object_add_value_uint(root, "work_item", le32_to_cpu(eid_idctrl->work_item));
	json_object_add_value_string(root, "system_version", sys_ver);
	json_object_add_value_string(root, "fw_commit_sha", fw_commit_str);
	json_object_add_value_string(root, "hw_commit_sha", hw_commit_str);
	json_object_add_value_string(root, "compiled_fw_commit_sha", compiled_fw_commit_str);
	json_object_add_value_uint(root, "job_id_count", le32_to_cpu(eid_idctrl->job_id_count));
	json_print_object(root, NULL);
	printf("\n");
	json_free_object(root);
}

static void eid_show_id_ctrl_vs(struct eid_idctrl_noload *eid_idctrl, char *hw_build_str, 
								char *fw_build_str, char *hw_ver_str, char *fw_commit_str, 
								char *hw_commit_str, char *compiled_fw_commit_str, char *sys_ver,
								int human)
{
	if (human) {
		printf("hw_build_date\t\t\t: %s\n", hw_build_str);
		printf("fw_build_date\t\t\t: %s\n", fw_build_str);
		printf("hw_system_version\t\t: %s\n", hw_ver_str);
	} else {
		printf("hw_build_date\t\t\t: 0x%-8.8x\n", eid_idctrl->hw_build_date);
		printf("fw_build_date\t\t\t: 0x%-8.8x\n", eid_idctrl->fw_build_date);
		printf("hw_system_version\t\t: 0x%-8.8x\n", eid_idctrl->hw_system_ver);
	}

	printf("system_version\t\t\t: %s\n", sys_ver);
	printf("work_item\t\t\t: %d\n", eid_idctrl->work_item);
	printf("fw_commit_sha\t\t\t: %s\n", fw_commit_str);
	printf("hw_commit_sha\t\t\t: %s\n", hw_commit_str);
	printf("compiled_fw_commit_sha\t\t: %s\n", compiled_fw_commit_str);
	printf("job_id_count\t\t\t: %d\n", eid_idctrl->job_id_count);
}

static void eid_nvme_id_ctrl_vs(struct eid_idctrl_noload *eid_idctrl, unsigned int mode, int fmt)
{
	int human = mode & HUMAN;
	char hw_build_str[21];
	char fw_build_str[21];
    char hw_board_str[23];
	char fw_commit_str[41];
	char hw_commit_str[41];
	char sys_ver[6];
	char compiled_fw_commit_str[41];

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

    unsigned int hw_board_version = eid_idctrl->hw_system_ver & 0x00FFu;

	switch (hw_board_version) {
		case 1:
			strncpy(hw_board_str, "Flash GT Plus (250sp)", sizeof(hw_board_str));
			break;
		case 2:
			strncpy(hw_board_str, "AlphaData 9v3", sizeof(hw_board_str));
			break;
		case 3:
			strncpy(hw_board_str, "Bittware U2 Series 1", sizeof(hw_board_str));
			break;
		case 4:
			strncpy(hw_board_str, "Bittware U2 Series 2", sizeof(hw_board_str));
			break;
		case 5:
			strncpy(hw_board_str, "Xilinx VCU1525 v1.1", sizeof(hw_board_str));
			break;
		default:
			strncpy(hw_board_str, "Unknown", sizeof(hw_board_str));
			break;
	}
	unsigned int major_ver = ((eid_idctrl->hw_system_ver & 0xF000u) >> 12u);
	unsigned int minor_ver = ((eid_idctrl->hw_system_ver & 0x0F00u) >> 8u);
	snprintf(sys_ver, sizeof(sys_ver), "%d.%d", major_ver, minor_ver);

	snprintf(fw_commit_str, sizeof(fw_commit_str), "%08x%08x%08x%08x%08x", eid_idctrl->fw_commit_sha[0], 
		eid_idctrl->fw_commit_sha[1], eid_idctrl->fw_commit_sha[2],
		eid_idctrl->fw_commit_sha[3], eid_idctrl->fw_commit_sha[4]);

	snprintf(hw_commit_str, sizeof(hw_commit_str), "%08x%08x%08x%08x%08x", eid_idctrl->hw_commit_sha[0], 
		eid_idctrl->hw_commit_sha[1], eid_idctrl->hw_commit_sha[2],
		eid_idctrl->hw_commit_sha[3], eid_idctrl->hw_commit_sha[4]);

	snprintf(compiled_fw_commit_str, sizeof(compiled_fw_commit_str), "%08x%08x%08x%08x%08x", eid_idctrl->compiled_fw_commit_sha[0], 
		eid_idctrl->compiled_fw_commit_sha[1], eid_idctrl->compiled_fw_commit_sha[2],
		eid_idctrl->compiled_fw_commit_sha[3], eid_idctrl->compiled_fw_commit_sha[4]);

	if (fmt == JSON)
		json_eid_show_id_ctrl_vs(eid_idctrl, hw_build_str, fw_build_str, hw_board_str, fw_commit_str, hw_commit_str, compiled_fw_commit_str, sys_ver, human);
	else {
		printf("Eideticom NVME Identify Controller:\n");
		eid_show_id_ctrl_vs(eid_idctrl, hw_build_str, fw_build_str, hw_board_str, fw_commit_str, hw_commit_str, compiled_fw_commit_str, sys_ver, human);
	}
}

/*
 * List all the Eideticom namespaces in the system and identify the
 * accerlation function provided by that namespace. We base this off
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
		eid_print_list(list_items, eid_num);

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
	struct list_item temp_list_item;
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
		temp_list_item.ctrl = ctrl;
		if (eid_check_item(&temp_list_item)) {
			eid_nvme_id_ctrl_vs((struct eid_idctrl_noload *) &ctrl.vs, flags, fmt);
		}
		else {
			fprintf(stderr, "Not an Eideticom device\n");
		}
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
	struct nvme_id_ctrl ctrl;
	struct list_item temp_list_item;
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
	{
		fprintf(stderr,
			"Error: requesting namespace-id from non-block device\n");
			err = EINVAL;
			goto close_fd;
	}
	
	if (cfg.human_readable)
		flags |= HUMAN;

	err = nvme_identify_ctrl(fd, &ctrl);
	if (!err) {
		temp_list_item.ctrl = ctrl;
		if (!eid_check_item(&temp_list_item)) {
			fprintf(stderr, "Not an Eideticom device\n");
			goto close_fd;
		}
	}

	err = nvme_identify_ns(fd, cfg.namespace_id, 0, &ns);
	if (!err) {
		eid_id_ns_vs((struct eid_idns_noload *) &ns.vs, cfg.namespace_id, get_eid_ver(&temp_list_item), flags, fmt);
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
