// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (C) 2025-2026 MediaTek Inc. */

#include <asm/byteorder.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>

#include "mt7925.h"
#include "mcu.h"
#include "nan.h"
#include "regd.h"

static void mt7925_nan_set_5g_channel(struct mt792x_dev *dev,
				      struct mt7925_nan_enable_req_tlv *req,
				      struct cfg80211_nan_conf *conf)
{
	struct ieee80211_channel *chan;
	u32 ch5g = 0;

	chan = conf->band_cfgs[NL80211_BAND_5GHZ].chan;

	if (chan)
		dev_info(dev->mt76.dev, "MP = %u, 5g ch: %u\n",
			 conf->master_pref, chan->hw_value);
	else
		dev_info(dev->mt76.dev, "MP = %u, 5g ch is null\n",
			 conf->master_pref);

	if (!chan)
		return;

	if (!mt7925_regd_is_valid_channel(dev, NL80211_BAND_5GHZ, chan))
		return;

	req->config_5g_channel = 1;

	if (chan->hw_value == NAN_5G_LOW_DISC_CHANNEL)
		ch5g |= BIT(0);
	else if (chan->hw_value == NAN_5G_HIGH_DISC_CHANNEL)
		ch5g |= BIT(1);

	req->channel_5g_val = cpu_to_le32(ch5g);
}

static void mt7925_nan_set_cluster_id(struct mt7925_nan_enable_req_tlv *req,
				      const u8 *cluster_id)
{
	if (!cluster_id)
		return;

	req->cluster_high = cpu_to_le16(*(const u16 *)(cluster_id + 4));
	req->cluster_low = cpu_to_le16((u16)cluster_id[3]);
}

static void mt7925_nan_set_dw_interval(struct mt7925_nan_enable_req_tlv *req,
				       struct cfg80211_nan_conf *conf)
{
	if (conf->band_cfgs[NL80211_BAND_2GHZ].awake_dw_interval > 0) {
		req->config_dw.config_2dot4g_dw_band = 1;
		req->config_dw.dw_2dot4g_interval_val =
			cpu_to_le32(conf->band_cfgs[NL80211_BAND_2GHZ].awake_dw_interval);
	}

	if (conf->band_cfgs[NL80211_BAND_5GHZ].awake_dw_interval > 0) {
		req->config_dw.config_5g_dw_band = 1;
		req->config_dw.dw_5g_interval_val =
			cpu_to_le32(conf->band_cfgs[NL80211_BAND_5GHZ].awake_dw_interval);
	}
}

static void mt7925_nan_set_disc_beacon(struct mt7925_nan_enable_req_tlv *req,
				       struct cfg80211_nan_conf *conf)
{
	if (conf->discovery_beacon_interval > 0) {
		req->config_2dot4g_beacons = true;
		req->beacon_2dot4g_val = conf->discovery_beacon_interval;
	}
}

static void mt7925_nan_set_rssi_thresholds(struct mt7925_nan_enable_req_tlv *req,
					   struct cfg80211_nan_conf *conf)
{
	if (conf->band_cfgs[NL80211_BAND_2GHZ].chan) {
		req->config_2dot4g_rssi_close = 1;
		req->rssi_close_2dot4g_val =
			abs(conf->band_cfgs[NL80211_BAND_2GHZ].rssi_close);
		req->config_2dot4g_rssi_middle = 1;
		req->rssi_middle_2dot4g_val =
			abs(conf->band_cfgs[NL80211_BAND_2GHZ].rssi_middle);
	}

	if (conf->band_cfgs[NL80211_BAND_5GHZ].chan) {
		req->config_5g_rssi_close = 1;
		req->rssi_close_5g_val =
			abs(conf->band_cfgs[NL80211_BAND_5GHZ].rssi_close);
		req->config_5g_rssi_middle = 1;
		req->rssi_middle_5g_val =
			abs(conf->band_cfgs[NL80211_BAND_5GHZ].rssi_middle);
	}
}

static void mt7925_nan_set_scan_params(struct mt7925_nan_enable_req_tlv *req,
				       struct cfg80211_nan_conf *conf)
{
	req->scan_params_val.scan_period[0] =
		conf->scan_period < 255 ? conf->scan_period : 255;
	req->scan_params_val.dwell_time[0] =
		conf->scan_dwell_time < 255 ? conf->scan_dwell_time : 255;
}

int mt7925_nan_enable(struct ieee80211_vif *vif,
		      struct mt792x_dev *dev,
		      struct cfg80211_nan_conf *conf)
{
	struct mt76_dev *mdev = &dev->mt76;
	struct {
		u8 rsv[4];
		struct mt7925_nan_enable_req_tlv nan_req_tlv;
	} nan_cmd = {
		.rsv = { 0 },
		.nan_req_tlv = {
			.tag = cpu_to_le16(NAN_UNI_CMD_ENABLE_REQUEST),
			.len = cpu_to_le16(sizeof(struct mt7925_nan_enable_req_tlv)),
			.config_random_factor_force = 0,
			.random_factor_force_val = 0,
			.config_hop_count_force = 0,
			.hop_count_force_val = 0,
		},
	};
	struct mt7925_nan_enable_req_tlv *p_nan_req_tlv = &nan_cmd.nan_req_tlv;

	if (!dev || !conf)
		return -EINVAL;

	p_nan_req_tlv->master_pref = conf->master_pref;

	mt7925_nan_set_5g_channel(dev, p_nan_req_tlv, conf);
	mt7925_nan_set_cluster_id(p_nan_req_tlv, conf->cluster_id);
	mt7925_nan_set_dw_interval(p_nan_req_tlv, conf);
	mt7925_nan_set_disc_beacon(p_nan_req_tlv, conf);
	mt7925_nan_set_rssi_thresholds(p_nan_req_tlv, conf);
	mt7925_nan_set_scan_params(p_nan_req_tlv, conf);

	return mt76_mcu_send_msg(mdev, MCU_UNI_CMD(NAN), &nan_cmd, sizeof(nan_cmd), true);
}

int mt7925_nan_disable(struct ieee80211_vif *vif, struct mt792x_dev *dev)
{
	struct mt76_dev *mdev = &dev->mt76;
	struct {
		u8 rsv[4];
		struct tlv nan_dis_tlv;
	} nan_cmd = {
		.rsv = { 0 },
		.nan_dis_tlv = {
			.tag = cpu_to_le16(NAN_UNI_CMD_DISABLE_REQUEST),
			.len = cpu_to_le16(sizeof(struct tlv)),
		},
	};

	if (!dev)
		return -EINVAL;

	return mt76_mcu_send_msg(mdev, MCU_UNI_CMD(NAN), &nan_cmd, sizeof(nan_cmd), true);
}

static void
mt7925_nan_mp_tlv(struct sk_buff *skb, u8 master_pref)
{
	struct mt7925_nan_master_preference_tlv *mp_tlv = NULL;
	struct tlv *tlv = NULL;

	if (!skb)
		return;

	tlv = mt76_connac_mcu_add_tlv(skb, NAN_UNI_CMD_SET_MASTER_PREFERENCE,
				      sizeof(struct mt7925_nan_master_preference_tlv));
	if (!tlv)
		return;

	mp_tlv = (struct mt7925_nan_master_preference_tlv *)tlv;

	if (master_pref > NAN_MAX_MASTER_PREFERENCE)
		return;

	mp_tlv->master_preference = master_pref;
}

static void
mt7925_nan_dw_tlv(struct sk_buff *skb, struct cfg80211_nan_conf *conf)
{
	struct mt7925_nan_dw_interval_tlv *dw_tlv = NULL;
	struct tlv *tlv = NULL;
	u16 interval;

	if (!skb || !conf)
		return;

	tlv = mt76_connac_mcu_add_tlv(skb, NAN_UNI_CMD_SET_DW_INTERVAL,
				      sizeof(struct mt7925_nan_dw_interval_tlv));

	if (!tlv)
		return;

	dw_tlv = (struct mt7925_nan_dw_interval_tlv *)tlv;

	/* Set DW interval for 2.4GHz and 5GHz bands if available */
	if (conf->band_cfgs[NL80211_BAND_2GHZ].awake_dw_interval > 0) {
		dw_tlv->dw_interval = conf->band_cfgs[NL80211_BAND_2GHZ].awake_dw_interval;
	} else if (conf->band_cfgs[NL80211_BAND_5GHZ].awake_dw_interval > 0) {
		dw_tlv->dw_interval = conf->band_cfgs[NL80211_BAND_5GHZ].awake_dw_interval;
	} else {
		/* Fallback to a default value or log a warning */
		dw_tlv->dw_interval = NAN_DEFAULT_DW_INTERVAL;
	}

	/* Validate and set NAN Discovery Beacon Interval */
	interval = conf->discovery_beacon_interval > 0 ?
		   conf->discovery_beacon_interval :
		   NAN_DEFAULT_DISC_BCN_INTERVAL;

	dw_tlv->disc_bcn_interval = cpu_to_le16(interval);
}

static void
mt7925_nan_cluster_id_tlv(struct sk_buff *skb, const u8 *cluster_id)
{
	struct mt7925_nan_cluster_id_tlv *cluster_tlv = NULL;
	struct tlv *tlv = NULL;

	if (!skb || !cluster_id)
		return;

	tlv = mt76_connac_mcu_add_tlv(skb, NAN_UNI_CMD_SET_CLUSTER_ID,
				      sizeof(struct mt7925_nan_cluster_id_tlv));

	if (!tlv)
		return;

	cluster_tlv = (struct mt7925_nan_cluster_id_tlv *)tlv;

	memcpy(cluster_tlv->cluster_id, cluster_id, ETH_ALEN);
}

static void
mt7925_nan_sync_rssi_tlv(struct sk_buff *skb, struct cfg80211_nan_conf *conf)
{
	struct mt7925_nan_sync_rssi_tlv *rssi_tlv = NULL;
	struct tlv *tlv = NULL;

	if (!skb || !conf)
		return;

	tlv = mt76_connac_mcu_add_tlv(skb, NAN_UNI_CMD_SET_SYNC_RSSI,
				      sizeof(struct mt7925_nan_sync_rssi_tlv));

	if (!tlv)
		return;

	rssi_tlv = (struct mt7925_nan_sync_rssi_tlv *)tlv;

	if (conf->band_cfgs[NL80211_BAND_2GHZ].chan) {
		rssi_tlv->rssi_close_2g =
			conf->band_cfgs[NL80211_BAND_2GHZ].rssi_close;
		rssi_tlv->rssi_middle_2g =
			conf->band_cfgs[NL80211_BAND_2GHZ].rssi_middle;
	}

	if (conf->band_cfgs[NL80211_BAND_5GHZ].chan) {
		rssi_tlv->rssi_close_5g =
			conf->band_cfgs[NL80211_BAND_5GHZ].rssi_close;
		rssi_tlv->rssi_middle_5g =
			conf->band_cfgs[NL80211_BAND_5GHZ].rssi_middle;
	}
}

int mt7925_nan_change_configure(struct ieee80211_vif *vif,
				struct mt792x_dev *dev,
				struct cfg80211_nan_conf *conf)
{
	struct mt76_dev *mdev = &dev->mt76;
	struct mt7925_nan_common_hdr *hdr = NULL;
	struct sk_buff *skb = NULL;

	if (!dev || !conf)
		return -EINVAL;

	skb = mt76_mcu_msg_alloc(mdev, NULL, MT7925_NAN_CONF_MAX_SIZE);
	if (!skb)
		return -ENOMEM;

	hdr = (struct mt7925_nan_common_hdr *)skb_put(skb, sizeof(*hdr));
	memset(hdr, 0, sizeof(*hdr));

	mt7925_nan_mp_tlv(skb, conf->master_pref);
	mt7925_nan_dw_tlv(skb, conf);
	mt7925_nan_cluster_id_tlv(skb, conf->cluster_id);
	mt7925_nan_sync_rssi_tlv(skb, conf);

	return mt76_mcu_skb_send_msg(mdev, skb,
				     MCU_UNI_CMD(NAN), true);
}

static void
mt7925_nan_mcu_handle_de_event(struct mt792x_dev *dev, struct tlv *tlv)
{
	struct mt7925_nan_de_event *de_evt = NULL;
	u8 cluster_id[ETH_ALEN] __aligned(2) = {0x50, 0x6f, 0x9a, 0x01, 0x00, 0x00};
	u16 len;

	if (!dev || !tlv) {
		if (dev)
			dev_warn(dev->mt76.dev, "nan: failed to parse TLV\n");
		return;
	}

	len = le16_to_cpu(tlv->len);
	if (len < sizeof(*tlv) + sizeof(*de_evt)) {
		dev_warn(dev->mt76.dev,
			 "nan: short de_event tlv len=%u\n", len);
		return;
	}

	de_evt = (struct mt7925_nan_de_event *)tlv->data;
	if (!de_evt) {
		dev_warn(dev->mt76.dev, "nan: missing DE event payload\n");
		return;
	}

	if (de_evt->event_type == NAN_EVENT_ID_DISC_MAC_ADDR)
		return;

	memcpy(cluster_id, de_evt->cluster_id, ETH_ALEN);

	dev_dbg(dev->mt76.dev, "nan: evt=%u cluster=%pM\n",
		de_evt->event_type, de_evt->cluster_id);

	if (de_evt->event_type != NAN_EVENT_ID_JOINED_CLUSTER)
		return;

	if (!ieee80211_vif_nan_started(dev->nan_vif)) {
		dev_warn(dev->mt76.dev, "nan: joined-cluster event but NAN not started\n");
		return;
	}

	dev_dbg(dev->mt76.dev, "nan: anchor_master_rank=%*phN\n",
		NAN_ANCHOR_MASTER_RANK_NUM, de_evt->anchor_master_rank);

	dev_dbg(dev->mt76.dev, "nan: own_nmi=%pM master_nmi=%pM\n",
		de_evt->own_nmi, de_evt->master_nmi);

	ieee80211_nan_cluster_joined(dev->nan_vif, cluster_id, true, GFP_KERNEL);
}

void mt7925_nan_mcu_event(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct tlv *tlv;
	u32 tlv_len;

	if (!dev || !skb)
		return;

	if (skb->len < sizeof(struct mt7925_mcu_rxd) + 4)
		return;

	skb_pull(skb, sizeof(struct mt7925_mcu_rxd) + 4);
	tlv = (struct tlv *)skb->data;
	tlv_len = skb->len;

	while (tlv_len >= sizeof(*tlv)) {
		u16 len = le16_to_cpu(tlv->len);

		if (len < sizeof(*tlv) || len > tlv_len)
			break;

		switch (le16_to_cpu(tlv->tag)) {
		case NAN_UNI_EVENT_ID_DE_EVENT_IND:
			mt7925_nan_mcu_handle_de_event(dev, tlv);
			break;
		default:
			break;
		}

		tlv_len -= len;
		tlv = (struct tlv *)((u8 *)tlv + len);
	}
}
