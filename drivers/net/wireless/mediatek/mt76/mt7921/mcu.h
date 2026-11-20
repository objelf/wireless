/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2020 MediaTek Inc. */

#ifndef __MT7921_MCU_H
#define __MT7921_MCU_H

#include "../mt76_connac_mcu.h"

struct mt7921_mcu_tx_done_event {
	u8 pid;
	u8 status;
	__le16 seq;

	u8 wlan_idx;
	u8 tx_cnt;
	__le16 tx_rate;

	u8 flag;
	u8 tid;
	u8 rsp_rate;
	u8 mcs;

	u8 bw;
	u8 tx_pwr;
	u8 reason;
	u8 rsv0[1];

	__le32 delay;
	__le32 timestamp;
	__le32 applied_flag;
	u8 txs[28];

	u8 rsv1[32];
} __packed;

/* ext event table */
enum {
	MCU_EXT_EVENT_RATE_REPORT = 0x87,
};

struct mt7921_mcu_eeprom_info {
	__le32 addr;
	__le32 valid;
	u8 data[MT7921_EEPROM_BLOCK_SIZE];
} __packed;

#define MT_RA_RATE_NSS			GENMASK(8, 6)
#define MT_RA_RATE_MCS			GENMASK(3, 0)
#define MT_RA_RATE_TX_MODE		GENMASK(12, 9)
#define MT_RA_RATE_DCM_EN		BIT(4)
#define MT_RA_RATE_BW			GENMASK(14, 13)

enum {
	MT_EBF = BIT(0),	/* explicit beamforming */
	MT_IBF = BIT(1)		/* implicit beamforming */
};

struct mt7921_mcu_ant_id_config {
	u8 ant_id[4];
} __packed;

struct mt7921_txpwr_req {
	u8 ver;
	u8 action;
	__le16 len;
	u8 dbdc_idx;
	u8 rsv[3];
} __packed;

struct mt7921_txpwr_event {
	u8 ver;
	u8 action;
	__le16 len;
	struct mt7921_txpwr txpwr;
} __packed;

struct mt7921_wf_rf_pin_ctrl_event {
	u8 result;
	u8 value;
} __packed;

enum {
	TM_SWITCH_MODE,
	TM_SET_AT_CMD,
	TM_QUERY_AT_CMD,
};

enum {
	MT7921_TM_NORMAL,
	MT7921_TM_TESTMODE,
	MT7921_TM_ICAP,
	MT7921_TM_ICAP_OVERLAP,
	MT7921_TM_WIFISPECTRUM,
};

struct mt7921_rftest_cmd {
	u8 action;
	u8 rsv[3];
	__le32 param0;
	__le32 param1;
} __packed;

struct mt7921_rftest_evt {
	__le32 param0;
	__le32 param1;
} __packed;

struct mt7921_clc_info_tlv {
	__le16 tag;
	__le16 len;

	u8 chan_conf; /* BIT(0) : Enable UNII-4
		       * BIT(1) : Enable UNII-5
		       * BIT(2) : Enable UNII-6
		       * BIT(3) : Enable UNII-7
		       * BIT(4) : Enable UNII-8
		       */
	u8 rsv[63];
} __packed;

/* Command set of the MT7921 legacy firmware. Values and layouts follow what
 * that firmware expects, every field is little endian.
 */
enum {
	MCU_CE_CMD_BASIC_CONFIG = 0x02,
	MCU_CE_CMD_BSS_ACTIVATE = 0x11,
	MCU_CE_CMD_SET_BSS_INFO = 0x12,
	MCU_CE_CMD_UPDATE_STA_REC = 0x13,
	MCU_CE_CMD_REMOVE_STA_REC = 0x14,
	MCU_CE_CMD_SET_BSS_CREATED = 0x15,
	MCU_CE_CMD_SET_BSS_RLM = 0x19,
};

#define MT7921_CE_NETWORK_AIS		0
#define MT7921_CE_NETWORK_P2P		1

#define MT7921_CE_OP_MODE_INFRA		0
#define MT7921_CE_OP_MODE_IBSS		1
#define MT7921_CE_OP_MODE_AP		2
#define MT7921_CE_OP_MODE_P2P_DEV	3

#define MT7921_CE_MEDIA_CONNECTED	0
#define MT7921_CE_MEDIA_DISCONNECTED	1

/* station type: who the peer is, and what it is to us */
#define MT7921_CE_STA_LEGACY		BIT(0)
#define MT7921_CE_STA_P2P		BIT(1)
#define MT7921_CE_STA_ROLE_ADHOC	BIT(4)
#define MT7921_CE_STA_ROLE_CLIENT	BIT(5)
#define MT7921_CE_STA_ROLE_AP		BIT(6)
#define MT7921_CE_STA_ROLE_DLS		BIT(7)

#define MT7921_CE_STA_IDX_NONE		0xfe
#define MT7921_CE_WLAN_IDX_NONE		0xff

#define MT7921_CE_PHY_HR_DSSS		BIT(0)
#define MT7921_CE_PHY_ERP		BIT(1)
#define MT7921_CE_PHY_ERP_P2P		BIT(2)
#define MT7921_CE_PHY_OFDM		BIT(3)
#define MT7921_CE_PHY_HT		BIT(4)
#define MT7921_CE_PHY_VHT		BIT(5)
#define MT7921_CE_PHY_HE		BIT(6)

/* index form of the above, for the non-HT basic PHY type */
#define MT7921_CE_PHY_IDX_HR_DSSS	0
#define MT7921_CE_PHY_IDX_ERP		1
#define MT7921_CE_PHY_IDX_ERP_P2P	2
#define MT7921_CE_PHY_IDX_OFDM		3

#define MT7921_CE_RATE_CCK_1M		0x0000
#define MT7921_CE_RATE_OFDM_6M		0x004b

#define MT7921_CE_BAND_2G		1
#define MT7921_CE_BAND_5G		2
#define MT7921_CE_BAND_6G		3

#define MT7921_CE_SCO_NONE		0
#define MT7921_CE_SCO_ABOVE		1
#define MT7921_CE_SCO_BELOW		3

#define MT7921_CE_CW_20_40MHZ		0
#define MT7921_CE_CW_80MHZ		1
#define MT7921_CE_CW_160MHZ		2
#define MT7921_CE_CW_80P80MHZ		3

#define MT7921_CE_DBDC_BAND_AUTO	4

#define MT7921_CE_AUTH_OPEN		0
#define MT7921_CE_AUTH_WPA_PSK		4
#define MT7921_CE_AUTH_WPA2_PSK		7

#define MT7921_CE_ENC_WEP		0
#define MT7921_CE_ENC_NONE		1
#define MT7921_CE_ENC_TKIP		4
#define MT7921_CE_ENC_AES		6

#define MT7921_CE_RTS_POLICY_LEGACY	3

#define MT7921_CE_BA_SIZE		64
#define MT7921_CE_BA_SIZE_HE		256
#define MT7921_CE_MAX_AMSDU_LEN		11454

struct mt7921_ce_basic_config {
	u8 native_80211;
	u8 assert_path;
	u8 debug_level;
	u8 rsv[1];
	__le16 rx_csum; /* bit0: IP, bit1: UDP, bit2: TCP */
	__le16 tx_csum;
	u8 segment;
	u8 rsv2[3];
} __packed;

struct mt7921_ce_bss_activate {
	u8 bss_idx;
	u8 active;
	u8 network_type;
	u8 omac_idx;
	u8 addr[ETH_ALEN];
	u8 bmc_wlan_idx;
	u8 link_idx;
} __packed;

struct mt7921_ce_bss_rlm {
	u8 bss_idx;
	u8 band;
	u8 primary_chan;
	u8 sco;
	u8 erp_protect;
	u8 ht_protect;
	u8 gf_mode;
	u8 rifs_mode;
	__le16 ht_op_info3;
	__le16 ht_op_info2;
	u8 ht_op_info1;
	u8 short_preamble;
	u8 short_slot;
	u8 vht_width;
	u8 vht_center_chan;
	u8 vht_center_chan2;
	__le16 vht_basic_mcs;
	u8 tx_nss;
	u8 rx_nss;
} __packed;

struct mt7921_ce_bss_info {
	u8 bss_idx;
	u8 conn_state;
	u8 op_mode;
	u8 ssid_len;
	u8 ssid[IEEE80211_MAX_SSID_LEN];
	u8 bssid[ETH_ALEN];
	u8 qbss;
	u8 version;
	__le16 oper_rates;
	__le16 basic_rates;
	u8 ap_sta_idx;
	u8 rsv[1];
	__le16 fixed_rate;
	u8 basic_phy_type;
	u8 auth_mode;
	u8 enc_status;
	u8 phy_type;
	u8 wapi;
	u8 ap_mode;
	u8 bmc_wlan_idx;
	u8 hidden_ssid;
	u8 disconnect_thresh;
	u8 iot_ap_act;
	u8 rsv2[2];
	__le32 private_data;
	struct mt7921_ce_bss_rlm rlm;
	u8 dbdc_band;
	u8 wmm_idx;
	u8 dbdc_action;
	u8 nss;
	u8 rsv3[2];
	u8 he_op_params[3];
	u8 bss_color;
	__le16 he_basic_mcs;
	u8 max_bssid_indicator;
	u8 mbssid_idx;
	__le64 iot_ap_act_ext;
	u8 rsv4[8];
} __packed;

struct mt7921_ce_sta_rec {
	u8 sta_idx;
	u8 sta_type;
	u8 addr[ETH_ALEN];
	__le16 aid;
	__le16 listen_interval;
	u8 bss_idx;
	u8 phy_type;
	__le16 rates;
	__le16 basic_rates;
	u8 qos;
	u8 uapsd;
	u8 state;
	u8 mcs_set;
	u8 mcs32;
	u8 version;
	u8 rx_mcs_bitmask[10];
	__le16 rx_highest;
	__le32 tx_rate_info;
	__le16 ht_cap;
	__le16 ht_ext_cap;
	__le32 txbf_cap;
	u8 ampdu_param;
	u8 asel_cap;
	u8 rcpi;
	u8 need_resp;
	u8 uapsd_ac;
	u8 uapsd_sp;
	u8 wlan_idx;
	u8 bmc_wlan_idx;
	__le32 vht_cap;
	__le16 vht_rx_mcs_map;
	__le16 vht_rx_highest;
	__le16 vht_tx_mcs_map;
	__le16 vht_tx_highest;
	u8 rts_policy;
	u8 vht_op_mode;
	u8 traffic_type;
	u8 tx_gf;
	u8 tx_sgi;
	u8 tx_stbc;
	__le16 fixed_rate;
	u8 tx_ampdu;
	u8 rx_ampdu;
	u8 rsv[2];
	__le32 fixed_phy_rate;
	__le16 max_link_speed;
	__le16 min_link_speed;
	__le32 flags;
	union {
		struct {
			u8 tx;
			u8 rx;
			u8 rsv[2];
		} ht;
		struct {
			__le16 tx;
			__le16 rx;
		} he;
	} ba_size;
	u8 txbf[30];
	u8 mlr_mode;
	u8 mlr_state;
	u8 tx_amsdu_in_ampdu;
	u8 rx_amsdu_in_ampdu;
	u8 rsv2[2];
	__le32 max_amsdu_len;
	/* valid with version 1 */
	u8 he_mac_cap[6];
	u8 he_phy_cap[11];
	u8 rsv3[3];
	__le16 he_rx_mcs_80;
	__le16 he_tx_mcs_80;
	__le16 he_rx_mcs_160;
	__le16 he_tx_mcs_160;
	__le16 he_rx_mcs_80p80;
	__le16 he_tx_mcs_80p80;
	__le16 he_6g_cap;
	u8 rsv4[30];
} __packed;

struct mt7921_ce_sta_remove {
	u8 action;
	u8 sta_idx;
	u8 bss_idx;
	u8 rsv;
} __packed;

struct mt7921_ce_bss_created {
	u8 bss_idx;
	u8 dtim_period;
	__le16 bcn_interval;
	__le16 atim_window;
	u8 rsv[2];
} __packed;
#endif
