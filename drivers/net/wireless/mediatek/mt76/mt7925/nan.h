/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Copyright (C) 2025-2026 MediaTek Inc. */

#ifndef __MT7925_NAN_H
#define __MT7925_NAN_H

#include <linux/if_ether.h>
#include <linux/types.h>

#include "../mt76_connac_mcu.h"

#define NAN_MAX_SOCIAL_CHANNELS		3
#define NAN_ANCHOR_MASTER_RANK_NUM	8
#define NAN_5G_LOW_DISC_CHANNEL		44
#define NAN_5G_HIGH_DISC_CHANNEL	149
#define NAN_MAX_MASTER_PREFERENCE	255
#define NAN_DEFAULT_DW_INTERVAL		1
#define NAN_DEFAULT_DISC_BCN_INTERVAL	100

#define MT7925_NAN_CONF_MAX_SIZE					\
	(sizeof(struct mt7925_nan_common_hdr) +				\
	 sizeof(struct mt7925_nan_master_preference_tlv) +		\
	 sizeof(struct mt7925_nan_dw_interval_tlv) +			\
	 sizeof(struct mt7925_nan_cluster_id_tlv) +			\
	 sizeof(struct mt7925_nan_sync_rssi_tlv))

enum nan_uni_cmd_tag {
	NAN_UNI_CMD_SET_MASTER_PREFERENCE	= 0,
	NAN_UNI_CMD_ENABLE_REQUEST		= 7,
	NAN_UNI_CMD_DISABLE_REQUEST		= 8,
	NAN_UNI_CMD_SET_DW_INTERVAL		= 26,
	NAN_UNI_CMD_SET_SYNC_RSSI		= 39,
	NAN_UNI_CMD_SET_CLUSTER_ID		= 40,
};

enum nan_uni_event_tag {
	NAN_UNI_EVENT_ID_DE_EVENT_IND		= 19,
};

enum nan_disc_event_type {
	NAN_EVENT_ID_DISC_MAC_ADDR		= 0,
	NAN_EVENT_ID_JOINED_CLUSTER		= 2,
};

struct mt7925_nan_social_ch_scan_params {
	u8 dwell_time[NAN_MAX_SOCIAL_CHANNELS];
	__le16 scan_period[NAN_MAX_SOCIAL_CHANNELS];
} __packed;

struct mt7925_nan_conf_dw {
	u8 config_2dot4g_dw_band;
	__le32 dw_2dot4g_interval_val;

	u8 config_5g_dw_band;
	__le32 dw_5g_interval_val;
} __packed;

struct mt7925_nan_enable_req_tlv {
	__le16 tag;
	__le16 len;

	u8 master_pref;
	__le16 cluster_low;
	__le16 cluster_high;

	u8 config_support_5g;
	u8 support_5g_val;

	u8 config_sid_beacon;
	u8 sid_beacon_val;

	u8 config_2dot4g_rssi_close;
	u8 rssi_close_2dot4g_val;
	u8 config_2dot4g_rssi_middle;
	u8 rssi_middle_2dot4g_val;

	u8 config_2dot4g_rssi_proximity;
	u8 rssi_proximity_2dot4g_val;
	u8 config_hop_count_limit;
	u8 hop_count_limit_val;

	u8 config_2dot4g_support;
	u8 support_2dot4g_val;

	u8 config_2dot4g_beacons;
	u8 beacon_2dot4g_val;

	u8 config_2dot4g_sdf;
	u8 sdf_2dot4g_val;

	u8 config_5g_beacons;
	u8 beacon_5g_val;

	u8 config_5g_sdf;
	u8 sdf_5g_val;

	u8 config_5g_rssi_close;
	u8 rssi_close_5g_val;

	u8 config_5g_rssi_middle;
	u8 rssi_middle_5g_val;

	u8 config_5g_rssi_close_proximity;
	u8 rssi_close_proximity_5g_val;

	u8 config_rssi_window_size;
	u8 rssi_window_size_val;

	u8 config_oui;
	__le32 oui_val;

	u8 config_intf_addr;
	u8 intf_addr_val[ETH_ALEN];

	u8 config_cluster_attribute_val;

	u8 config_scan_params;
	struct mt7925_nan_social_ch_scan_params scan_params_val;

	u8 config_random_factor_force;
	u8 random_factor_force_val;

	u8 config_hop_count_force;
	u8 hop_count_force_val;

	u8 config_24g_channel;
	__le32 channel_24g_val;

	u8 config_5g_channel;
	__le32 channel_5g_val;

	struct mt7925_nan_conf_dw config_dw;

	u8 config_disc_mac_addr_randomization;
	__le32 disc_mac_addr_rand_interval_sec;

	u8 discovery_indication_cfg;

	u8 config_subscribe_sid_beacon;
	__le32 subscribe_sid_beacon_val;

	u8 enable_log_slot_statistics;
} __packed __aligned(4);

struct mt7925_nan_common_hdr {
	u8 reserved[4];
};

struct mt7925_nan_master_preference_tlv {
	__le16 tag;
	__le16 len;
	u8 master_preference;
	u8 reserved[3];
} __packed __aligned(4);

struct mt7925_nan_dw_interval_tlv {
	__le16 tag;
	__le16 len;
	u8 dw_interval;
	u8 vendor_ioctl;
	__le16 disc_bcn_interval;
} __packed __aligned(4);

struct mt7925_nan_cluster_id_tlv {
	__le16 tag;
	__le16 len;
	u8 cluster_id[ETH_ALEN];
	u8 reserved[2];
} __packed __aligned(4);

struct mt7925_nan_sync_rssi_tlv {
	__le16 tag;
	__le16 len;
	s8 rssi_close_2g;
	s8 rssi_middle_2g;
	s8 rssi_close_5g;
	s8 rssi_middle_5g;
} __packed __aligned(4);

struct mt7925_nan_de_event {
	u8 event_type;
	u8 cluster_id[ETH_ALEN];
	u8 anchor_master_rank[NAN_ANCHOR_MASTER_RANK_NUM];
	u8 own_nmi[ETH_ALEN];
	u8 master_nmi[ETH_ALEN];
};

int mt7925_nan_enable(struct ieee80211_vif *vif,
		      struct mt792x_dev *dev,
		      struct cfg80211_nan_conf *conf);

int mt7925_nan_disable(struct ieee80211_vif *vif,
		       struct mt792x_dev *dev);

int mt7925_nan_change_configure(struct ieee80211_vif *vif,
				struct mt792x_dev *dev,
				struct cfg80211_nan_conf *conf);

void mt7925_nan_mcu_event(struct mt792x_dev *dev, struct sk_buff *skb);
#endif
