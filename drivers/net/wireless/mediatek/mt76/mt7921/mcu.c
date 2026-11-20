// SPDX-License-Identifier: BSD-3-Clause-Clear
/* Copyright (C) 2020 MediaTek Inc. */

#include <linux/fs.h>
#include <linux/firmware.h>
#include <linux/of.h>
#include "mt7921.h"
#include "regd.h"
#include "mcu.h"
#include "../mt76_connac2_mac.h"
#include "../mt792x_trace.h"

#define MT_STA_BFER			BIT(0)
#define MT_STA_BFEE			BIT(1)

int mt7921_mcu_parse_response(struct mt76_dev *mdev, int cmd,
			      struct sk_buff *skb, int seq)
{
	int mcu_cmd = FIELD_GET(__MCU_CMD_FIELD_ID, cmd);
	struct mt76_connac2_mcu_rxd *rxd;
	int ret = 0;

	if (!skb) {
		dev_err(mdev->dev, "Message %08x (seq %d) timeout\n",
			cmd, seq);
		mt792x_reset(mdev);

		return -ETIMEDOUT;
	}
	if ((cmd == MCU_CMD(PATCH_SEM_CONTROL) ||
	     cmd == MCU_CMD(PATCH_FINISH_REQ)) &&
	    skb->len < sizeof(*rxd) - 4 + sizeof(u8))
		return -EINVAL;
	if (cmd != MCU_CMD(PATCH_SEM_CONTROL) &&
	    cmd != MCU_CMD(PATCH_FINISH_REQ) && skb->len < sizeof(*rxd))
		return -EINVAL;

	rxd = (struct mt76_connac2_mcu_rxd *)skb->data;
	if (seq != rxd->seq)
		return -EAGAIN;

	if (cmd == MCU_CMD(PATCH_SEM_CONTROL) ||
	    cmd == MCU_CMD(PATCH_FINISH_REQ)) {
		skb_pull(skb, sizeof(*rxd) - 4);
		ret = *skb->data;
	} else if (cmd == MCU_EXT_CMD(THERMAL_CTRL)) {
		if (skb->len < sizeof(*rxd) + 4 + sizeof(__le32))
			return -EINVAL;

		skb_pull(skb, sizeof(*rxd) + 4);
		ret = le32_to_cpu(*(__le32 *)skb->data);
	} else if (cmd == MCU_UNI_CMD(DEV_INFO_UPDATE) ||
		   cmd == MCU_UNI_CMD(BSS_INFO_UPDATE) ||
		   cmd == MCU_UNI_CMD(STA_REC_UPDATE) ||
		   cmd == MCU_UNI_CMD(HIF_CTRL) ||
		   cmd == MCU_UNI_CMD(OFFLOAD) ||
		   cmd == MCU_UNI_CMD(SUSPEND)) {
		struct mt76_connac_mcu_uni_event *event;

		if (skb->len < sizeof(*rxd) + sizeof(*event))
			return -EINVAL;

		skb_pull(skb, sizeof(*rxd));
		event = (struct mt76_connac_mcu_uni_event *)skb->data;
		/* skip invalid event */
		if (mcu_cmd != event->cid)
			ret = -EAGAIN;
		else
			ret = mt76_connac_mcu_status_err(le32_to_cpu(event->status));
	} else if (cmd == MCU_CE_QUERY(REG_READ)) {
		struct mt76_connac_mcu_reg_event *event;

		if (skb->len < sizeof(*rxd) + sizeof(*event))
			return -EINVAL;

		skb_pull(skb, sizeof(*rxd));
		event = (struct mt76_connac_mcu_reg_event *)skb->data;
		ret = (int)le32_to_cpu(event->val);
	} else if (cmd == MCU_EXT_CMD(WF_RF_PIN_CTRL)) {
		struct mt7921_wf_rf_pin_ctrl_event *event;

		if (skb->len < sizeof(*rxd) + sizeof(*event))
			return -EINVAL;

		skb_pull(skb, sizeof(*rxd));
		event = (struct mt7921_wf_rf_pin_ctrl_event *)skb->data;
		ret = (int)event->result;
	} else {
		skb_pull(skb, sizeof(struct mt76_connac2_mcu_rxd));
	}

	return ret;
}
EXPORT_SYMBOL_GPL(mt7921_mcu_parse_response);

static int mt7921_mcu_read_eeprom(struct mt792x_dev *dev, u32 offset, u8 *val)
{
	struct mt7921_mcu_eeprom_info *res, req = {
		.addr = cpu_to_le32(round_down(offset,
				    MT7921_EEPROM_BLOCK_SIZE)),
	};
	struct sk_buff *skb;
	int ret;

	ret = mt76_mcu_send_and_get_msg(&dev->mt76, MCU_EXT_QUERY(EFUSE_ACCESS),
					&req, sizeof(req), true, &skb);
	if (ret)
		return ret;
	if (skb->len < sizeof(*res)) {
		dev_kfree_skb(skb);
		return -EINVAL;
	}

	res = (struct mt7921_mcu_eeprom_info *)skb->data;
	if (!le32_to_cpu(res->valid)) {
		dev_kfree_skb(skb);
		return -ENODATA;
	}

	*val = res->data[offset % MT7921_EEPROM_BLOCK_SIZE];
	dev_kfree_skb(skb);

	return 0;
}

/* MT7921 legacy firmware: BSS, station, key and channel handling go through
 * the legacy command set, and block ack sessions are negotiated by the
 * firmware. Nothing below is reached with the linux-firmware image.
 */
static u8 mt7921_mcu_ce_band(enum nl80211_band band)
{
	switch (band) {
	case NL80211_BAND_6GHZ:
		return MT7921_CE_BAND_6G;
	case NL80211_BAND_5GHZ:
		return MT7921_CE_BAND_5G;
	default:
		return MT7921_CE_BAND_2G;
	}
}

static u16
mt7921_mcu_ce_rates(struct ieee80211_supported_band *sband, u32 rates)
{
	/* rate bit order of the firmware, in units of 100 kbps */
	static const u16 order[] = {
		10, 20, 55, 110, 220, 330, 60, 90, 120, 180, 240, 360, 480, 540,
	};
	u16 val = 0;
	int i, j;

	for (i = 0; i < sband->n_bitrates; i++) {
		if (!(rates & BIT(i)))
			continue;

		for (j = 0; j < ARRAY_SIZE(order); j++)
			if (sband->bitrates[i].bitrate == order[j])
				val |= BIT(j);
	}

	return val;
}

static struct cfg80211_chan_def *
mt7921_mcu_ce_chandef(struct mt792x_dev *dev, struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct ieee80211_chanctx_conf *ctx = mvif->bss_conf.mt76.ctx;

	return ctx ? &ctx->def : &dev->mphy.chandef;
}

static void
mt7921_mcu_ce_chan(const struct cfg80211_chan_def *chandef, u8 *sco,
		   u8 *width, u8 *center_chan, u8 *center_chan2)
{
	int bw = nl80211_chan_width_to_mhz(chandef->width);
	u32 freq = chandef->chan->center_freq;

	*sco = MT7921_CE_SCO_NONE;
	*width = MT7921_CE_CW_20_40MHZ;
	*center_chan = 0;
	*center_chan2 = 0;

	if (bw < 40)
		return;

	/* which half of its 40 MHz pair the primary channel sits in */
	if (((freq - (chandef->center_freq1 - bw / 2 + 10)) / 20) & 1)
		*sco = MT7921_CE_SCO_BELOW;
	else
		*sco = MT7921_CE_SCO_ABOVE;

	switch (chandef->width) {
	case NL80211_CHAN_WIDTH_80:
		*width = MT7921_CE_CW_80MHZ;
		break;
	case NL80211_CHAN_WIDTH_160:
		*width = MT7921_CE_CW_160MHZ;
		break;
	case NL80211_CHAN_WIDTH_80P80:
		*width = MT7921_CE_CW_80P80MHZ;
		*center_chan2 =
			ieee80211_frequency_to_channel(chandef->center_freq2);
		break;
	default:
		return;
	}

	*center_chan = ieee80211_frequency_to_channel(chandef->center_freq1);
}

static u16 mt7921_mcu_ce_fixed_rate(struct ieee80211_vif *vif,
				    enum nl80211_band band)
{
	if (band == NL80211_BAND_2GHZ && !vif->p2p)
		return MT7921_CE_RATE_CCK_1M;

	return MT7921_CE_RATE_OFDM_6M;
}

static u8
mt7921_mcu_ce_phy_type(struct mt792x_dev *dev, struct ieee80211_vif *vif,
		       struct ieee80211_sta *sta, enum nl80211_band band)
{
	struct ieee80211_supported_band *sband;
	bool ht, vht, he;
	u8 type;

	sband = mt76_hw(dev)->wiphy->bands[band];
	if (sta) {
		ht = sta->deflink.ht_cap.ht_supported;
		vht = sta->deflink.vht_cap.vht_supported;
		he = sta->deflink.he_cap.has_he;
	} else {
		ht = sband->ht_cap.ht_supported;
		vht = sband->vht_cap.vht_supported;
		he = !!ieee80211_get_he_iftype_cap_vif(sband, vif);
	}

	if (band != NL80211_BAND_2GHZ)
		type = MT7921_CE_PHY_OFDM;
	else if (vif->p2p)
		type = MT7921_CE_PHY_ERP_P2P;
	else
		type = MT7921_CE_PHY_HR_DSSS | MT7921_CE_PHY_ERP;

	if (ht)
		type |= MT7921_CE_PHY_HT;
	if (vht)
		type |= MT7921_CE_PHY_VHT;
	if (he)
		type |= MT7921_CE_PHY_HE;

	return type;
}

static int mt7921_mcu_ce_basic_config(struct mt792x_dev *dev)
{
	struct mt7921_ce_basic_config req = {
		/* IP, UDP and TCP: the driver advertises NETIF_F_RXCSUM */
		.rx_csum = cpu_to_le16(GENMASK(2, 0)),
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(BASIC_CONFIG),
				 &req, sizeof(req), false);
}

static int
mt7921_mcu_ce_bss_activate(struct mt792x_dev *dev,
			   struct ieee80211_vif *vif, bool enable)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt7921_ce_bss_activate req = {
		.bss_idx = mvif->bss_conf.mt76.idx,
		.active = enable,
		.omac_idx = mvif->bss_conf.mt76.omac_idx,
		.bmc_wlan_idx = mvif->sta.deflink.wcid.idx,
	};

	/* soft AP and P2P roles all live in the P2P network */
	if (vif->type == NL80211_IFTYPE_STATION && !vif->p2p)
		req.network_type = MT7921_CE_NETWORK_AIS;
	else
		req.network_type = MT7921_CE_NETWORK_P2P;

	memcpy(req.addr, vif->addr, ETH_ALEN);

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(BSS_ACTIVATE),
				 &req, sizeof(req), false);
}

static void
mt7921_mcu_ce_fill_rlm(struct mt792x_dev *dev, struct ieee80211_vif *vif,
		       struct cfg80211_chan_def *chandef,
		       struct mt7921_ce_bss_rlm *rlm)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct ieee80211_bss_conf *conf = &vif->bss_conf;
	enum nl80211_band band = chandef->chan->band;
	struct ieee80211_supported_band *sband;

	sband = mt76_hw(dev)->wiphy->bands[band];

	rlm->bss_idx = mvif->bss_conf.mt76.idx;
	rlm->band = mt7921_mcu_ce_band(band);
	rlm->primary_chan = chandef->chan->hw_value;
	mt7921_mcu_ce_chan(chandef, &rlm->sco, &rlm->vht_width,
			   &rlm->vht_center_chan, &rlm->vht_center_chan2);

	rlm->erp_protect = conf->use_cts_prot;
	rlm->ht_protect = conf->ht_operation_mode &
			  IEEE80211_HT_OP_MODE_PROTECTION;
	rlm->ht_op_info2 = cpu_to_le16(conf->ht_operation_mode);
	rlm->ht_op_info1 = rlm->sco;
	if (rlm->sco != MT7921_CE_SCO_NONE)
		rlm->ht_op_info1 |= IEEE80211_HT_PARAM_CHAN_WIDTH_ANY;
	rlm->short_preamble = conf->use_short_preamble;
	rlm->short_slot = conf->use_short_slot;
	/* the firmware takes the BSS basic rate set in this field */
	rlm->vht_basic_mcs = cpu_to_le16(mt7921_mcu_ce_rates(sband,
							     conf->basic_rates));
	rlm->tx_nss = hweight8(dev->mphy.antenna_mask);
	rlm->rx_nss = rlm->tx_nss;
}

/* @ctx is the channel context to move to, NULL for the one in use */
int mt7921_mcu_ce_bss_rlm(struct mt792x_dev *dev, struct ieee80211_vif *vif,
			  struct ieee80211_chanctx_conf *ctx)
{
	struct mt7921_ce_bss_rlm req = {};

	mt7921_mcu_ce_fill_rlm(dev, vif,
			       ctx ? &ctx->def : mt7921_mcu_ce_chandef(dev, vif),
			       &req);

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(SET_BSS_RLM),
				 &req, sizeof(req), false);
}

/* @sta is the AP on a station interface, NULL otherwise */
static int
mt7921_mcu_ce_bss_info(struct mt792x_dev *dev, struct ieee80211_vif *vif,
		       struct ieee80211_sta *sta, bool enable)
{
	struct cfg80211_chan_def *chandef = mt7921_mcu_ce_chandef(dev, vif);
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct ieee80211_bss_conf *conf = &vif->bss_conf;
	enum nl80211_band band = chandef->chan->band;
	struct ieee80211_supported_band *sband;
	struct mt7921_ce_bss_info req = {
		.bss_idx = mvif->bss_conf.mt76.idx,
		.conn_state = enable ? MT7921_CE_MEDIA_CONNECTED :
				       MT7921_CE_MEDIA_DISCONNECTED,
		.qbss = conf->qos,
		.ap_sta_idx = MT7921_CE_STA_IDX_NONE,
		.auth_mode = MT7921_CE_AUTH_OPEN,
		.enc_status = MT7921_CE_ENC_NONE,
		.bmc_wlan_idx = mvif->sta.deflink.wcid.idx,
		.hidden_ssid = conf->hidden_ssid,
		.dbdc_band = MT7921_CE_DBDC_BAND_AUTO,
		.wmm_idx = mvif->bss_conf.mt76.wmm_idx,
		.nss = hweight8(dev->mphy.antenna_mask),
	};
	u32 rates = conf->basic_rates;

	BUILD_BUG_ON(sizeof(struct mt7921_ce_bss_rlm) != 22);
	BUILD_BUG_ON(sizeof(req) != 120);

	sband = mt76_hw(dev)->wiphy->bands[band];

	switch (vif->type) {
	case NL80211_IFTYPE_AP:
		req.op_mode = MT7921_CE_OP_MODE_AP;
		req.ap_mode = !vif->p2p;
		rates = GENMASK(sband->n_bitrates - 1, 0);
		break;
	case NL80211_IFTYPE_ADHOC:
		req.op_mode = MT7921_CE_OP_MODE_IBSS;
		rates = GENMASK(sband->n_bitrates - 1, 0);
		break;
	case NL80211_IFTYPE_P2P_DEVICE:
		req.op_mode = MT7921_CE_OP_MODE_P2P_DEV;
		break;
	default:
		req.op_mode = MT7921_CE_OP_MODE_INFRA;
		if (enable && sta) {
			struct mt792x_sta *msta;

			msta = (struct mt792x_sta *)sta->drv_priv;
			req.ap_sta_idx = msta->deflink.wcid.idx;
			rates = sta->deflink.supp_rates[band];
		}
		break;
	}

	req.ssid_len = min_t(u8, vif->cfg.ssid_len, sizeof(req.ssid));
	memcpy(req.ssid, vif->cfg.ssid, req.ssid_len);
	memcpy(req.bssid, conf->bssid, ETH_ALEN);

	req.oper_rates = cpu_to_le16(mt7921_mcu_ce_rates(sband, rates));
	req.basic_rates = cpu_to_le16(mt7921_mcu_ce_rates(sband,
							  conf->basic_rates));
	req.fixed_rate = cpu_to_le16(mt7921_mcu_ce_fixed_rate(vif, band));
	req.phy_type = mt7921_mcu_ce_phy_type(dev, vif, sta, band);

	if (band != NL80211_BAND_2GHZ)
		req.basic_phy_type = MT7921_CE_PHY_IDX_OFDM;
	else if (vif->p2p)
		req.basic_phy_type = MT7921_CE_PHY_IDX_ERP_P2P;
	else
		req.basic_phy_type = MT7921_CE_PHY_IDX_ERP;

	/* one cipher is all that is known here, derive both fields from it */
	switch (mvif->bss_conf.mt76.cipher) {
	case MCU_CIPHER_NONE:
		break;
	case MCU_CIPHER_WEP40:
	case MCU_CIPHER_WEP104:
		req.enc_status = MT7921_CE_ENC_WEP;
		break;
	case MCU_CIPHER_TKIP:
		req.auth_mode = MT7921_CE_AUTH_WPA_PSK;
		req.enc_status = MT7921_CE_ENC_TKIP;
		break;
	default:
		req.auth_mode = MT7921_CE_AUTH_WPA2_PSK;
		req.enc_status = MT7921_CE_ENC_AES;
		break;
	}

	mt7921_mcu_ce_fill_rlm(dev, vif, chandef, &req.rlm);

	if (conf->he_support) {
		u32 params = conf->he_oper.params;

		req.he_op_params[0] = params;
		req.he_op_params[1] = params >> 8;
		req.he_op_params[2] = params >> 16;
		req.bss_color = conf->he_bss_color.color;
		if (!conf->he_bss_color.enabled)
			req.bss_color |= IEEE80211_HE_OPERATION_BSS_COLOR_DISABLED >> 24;
		req.he_basic_mcs = cpu_to_le16(conf->he_oper.nss_set);
	}

	if (!enable)
		mvif->bss_conf.mt76.cipher = MCU_CIPHER_NONE;

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(SET_BSS_INFO),
				 &req, sizeof(req), false);
}

static u8
mt7921_mcu_ce_sta_type(struct ieee80211_vif *vif, struct ieee80211_sta *sta)
{
	switch (vif->type) {
	case NL80211_IFTYPE_AP:
		return MT7921_CE_STA_P2P | MT7921_CE_STA_ROLE_CLIENT;
	case NL80211_IFTYPE_ADHOC:
		return MT7921_CE_STA_LEGACY | MT7921_CE_STA_ROLE_ADHOC;
	default:
		if (sta->tdls)
			return MT7921_CE_STA_LEGACY | MT7921_CE_STA_ROLE_DLS;
		if (vif->p2p)
			return MT7921_CE_STA_P2P | MT7921_CE_STA_ROLE_AP;

		return MT7921_CE_STA_LEGACY | MT7921_CE_STA_ROLE_AP;
	}
}

static int
mt7921_mcu_ce_sta_update(struct mt792x_dev *dev, struct ieee80211_sta *sta,
			 struct ieee80211_vif *vif, bool enable,
			 enum mt76_sta_info_state state)
{
	struct cfg80211_chan_def *chandef = mt7921_mcu_ce_chandef(dev, vif);
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct ieee80211_link_sta *link = &sta->deflink;
	enum nl80211_band band = chandef->chan->band;
	struct ieee80211_supported_band *sband;
	struct mt7921_ce_sta_rec *req;
	struct mt792x_sta *msta;
	struct sk_buff *skb;
	int rssi;

	BUILD_BUG_ON(sizeof(*req) != 200);

	msta = (struct mt792x_sta *)sta->drv_priv;

	if (!enable) {
		struct mt7921_ce_sta_remove rm = {
			.sta_idx = msta->deflink.wcid.idx,
			.bss_idx = mvif->bss_conf.mt76.idx,
		};

		return mt76_mcu_send_msg(&dev->mt76,
					 MCU_CE_CMD(REMOVE_STA_REC),
					 &rm, sizeof(rm), false);
	}

	skb = mt76_mcu_msg_alloc(&dev->mt76, NULL, sizeof(*req));
	if (!skb)
		return -ENOMEM;

	sband = mt76_hw(dev)->wiphy->bands[band];
	rssi = -ewma_rssi_read(&mvif->bss_conf.rssi);

	req = skb_put_zero(skb, sizeof(*req));
	/* station record and WTBL share one index space in mt76 */
	req->sta_idx = msta->deflink.wcid.idx;
	req->wlan_idx = msta->deflink.wcid.idx;
	req->bmc_wlan_idx = MT7921_CE_WLAN_IDX_NONE;
	req->sta_type = mt7921_mcu_ce_sta_type(vif, sta);
	memcpy(req->addr, sta->addr, ETH_ALEN);
	if (vif->type == NL80211_IFTYPE_STATION && !sta->tdls)
		req->aid = cpu_to_le16(vif->cfg.aid);
	else
		req->aid = cpu_to_le16(sta->aid);
	req->listen_interval = cpu_to_le16(mt76_hw(dev)->conf.listen_interval);
	req->bss_idx = mvif->bss_conf.mt76.idx;
	req->phy_type = mt7921_mcu_ce_phy_type(dev, vif, sta, band);
	req->rates = cpu_to_le16(mt7921_mcu_ce_rates(sband,
						     link->supp_rates[band]));
	req->basic_rates =
		cpu_to_le16(mt7921_mcu_ce_rates(sband,
						vif->bss_conf.basic_rates));
	req->qos = sta->wme;
	req->uapsd = !!sta->uapsd_queues;
	req->uapsd_ac = sta->uapsd_queues;
	req->uapsd_sp = sta->max_sp;
	req->state = state;
	req->rcpi = to_rcpi(rssi);
	req->rts_policy = MT7921_CE_RTS_POLICY_LEGACY;
	req->fixed_rate = cpu_to_le16(mt7921_mcu_ce_fixed_rate(vif, band));
	req->tx_ampdu = 1;
	req->rx_ampdu = 1;
	req->ba_size.ht.tx = MT7921_CE_BA_SIZE;
	req->ba_size.ht.rx = MT7921_CE_BA_SIZE;

	if (link->ht_cap.ht_supported) {
		struct ieee80211_sta_ht_cap *ht = &link->ht_cap;

		req->mcs_set = ht->mcs.rx_mask[0];
		req->mcs32 = ht->mcs.rx_mask[4] & BIT(0);
		memcpy(req->rx_mcs_bitmask, ht->mcs.rx_mask,
		       sizeof(req->rx_mcs_bitmask));
		req->rx_highest = ht->mcs.rx_highest;
		req->tx_rate_info = cpu_to_le32(ht->mcs.tx_params);
		req->ht_cap = cpu_to_le16(ht->cap);
		req->ampdu_param = ht->ampdu_factor |
				   ht->ampdu_density << 2;
		req->tx_gf = !!(ht->cap & IEEE80211_HT_CAP_GRN_FLD);
		req->tx_sgi = !!(ht->cap & (IEEE80211_HT_CAP_SGI_20 |
					    IEEE80211_HT_CAP_SGI_40));
		req->tx_stbc = !!(ht->cap & IEEE80211_HT_CAP_RX_STBC);
		req->tx_amsdu_in_ampdu = 1;
		req->rx_amsdu_in_ampdu = 1;
		req->max_amsdu_len = cpu_to_le32(MT7921_CE_MAX_AMSDU_LEN);
	}

	if (link->vht_cap.vht_supported) {
		struct ieee80211_sta_vht_cap *vht = &link->vht_cap;

		req->vht_cap = cpu_to_le32(vht->cap);
		req->vht_rx_mcs_map = vht->vht_mcs.rx_mcs_map;
		req->vht_rx_highest = vht->vht_mcs.rx_highest;
		req->vht_tx_mcs_map = vht->vht_mcs.tx_mcs_map;
		req->vht_tx_highest = vht->vht_mcs.tx_highest;
	}

	if (link->vht_cap.vht_supported || link->he_cap.has_he)
		req->vht_op_mode = link->bandwidth |
				   (max_t(u8, link->rx_nss, 1) - 1) <<
				   IEEE80211_OPMODE_NOTIF_RX_NSS_SHIFT;

	if (link->he_cap.has_he) {
		struct ieee80211_sta_he_cap *he = &link->he_cap;

		req->version = 1;
		memcpy(req->he_mac_cap, he->he_cap_elem.mac_cap_info,
		       sizeof(req->he_mac_cap));
		memcpy(req->he_phy_cap, he->he_cap_elem.phy_cap_info,
		       sizeof(req->he_phy_cap));
		req->he_rx_mcs_80 = he->he_mcs_nss_supp.rx_mcs_80;
		req->he_tx_mcs_80 = he->he_mcs_nss_supp.tx_mcs_80;
		req->he_rx_mcs_160 = he->he_mcs_nss_supp.rx_mcs_160;
		req->he_tx_mcs_160 = he->he_mcs_nss_supp.tx_mcs_160;
		req->he_rx_mcs_80p80 = he->he_mcs_nss_supp.rx_mcs_80p80;
		req->he_tx_mcs_80p80 = he->he_mcs_nss_supp.tx_mcs_80p80;
		req->he_6g_cap = link->he_6ghz_capa.capa;
		req->ba_size.he.tx = cpu_to_le16(MT7921_CE_BA_SIZE_HE);
		req->ba_size.he.rx = cpu_to_le16(MT7921_CE_BA_SIZE_HE);
		req->tx_amsdu_in_ampdu = 1;
		req->rx_amsdu_in_ampdu = 1;
		req->max_amsdu_len = cpu_to_le32(MT7921_CE_MAX_AMSDU_LEN);
	}

	return mt76_mcu_skb_send_msg(&dev->mt76, skb,
				     MCU_CE_CMD(UPDATE_STA_REC), false);
}

static int mt7921_mcu_ce_cipher(u32 cipher)
{
	switch (cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
		return MT7921_CE_CIPHER_WEP40;
	case WLAN_CIPHER_SUITE_WEP104:
		return MT7921_CE_CIPHER_WEP104;
	case WLAN_CIPHER_SUITE_TKIP:
		return MT7921_CE_CIPHER_TKIP;
	case WLAN_CIPHER_SUITE_CCMP:
		return MT7921_CE_CIPHER_CCMP;
	case WLAN_CIPHER_SUITE_CCMP_256:
		return MT7921_CE_CIPHER_CCMP_256;
	case WLAN_CIPHER_SUITE_GCMP:
		return MT7921_CE_CIPHER_GCMP;
	case WLAN_CIPHER_SUITE_GCMP_256:
		return MT7921_CE_CIPHER_GCMP_256;
	default:
		return -EOPNOTSUPP;
	}
}

/* @wcid is the peer for a pairwise key and the interface for a group key */
static int
mt7921_mcu_ce_add_key(struct mt792x_dev *dev, struct ieee80211_vif *vif,
		      struct ieee80211_sta *sta, struct mt76_wcid *wcid,
		      struct ieee80211_key_conf *key, enum set_key_cmd cmd)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	bool pairwise = key->flags & IEEE80211_KEY_FLAG_PAIRWISE;
	bool ap = vif->type == NL80211_IFTYPE_AP;
	struct mt7921_ce_key req = {
		.add = cmd == SET_KEY,
		.tx_key = pairwise || ap,
		.pairwise = pairwise,
		.authenticator = ap,
		.bss_idx = mvif->bss_conf.mt76.idx,
		.key_id = key->keyidx,
		.wlan_idx = wcid->idx,
	};
	int cipher;

	BUILD_BUG_ON(sizeof(req) != 64);

	cipher = mt7921_mcu_ce_cipher(key->cipher);
	if (cipher < 0)
		return cipher;

	if (key->keylen > sizeof(req.key))
		return -EINVAL;

	req.cipher = cipher;
	if (sta)
		memcpy(req.addr, sta->addr, ETH_ALEN);
	else if (ap)
		eth_broadcast_addr(req.addr);
	else
		memcpy(req.addr, vif->bss_conf.bssid, ETH_ALEN);

	if (cmd == SET_KEY) {
		req.key_len = key->keylen;
		req.mgmt_protection = pairwise && sta && sta->mfp;
		memcpy(req.key, key->key, key->keylen);
		if (key->cipher == WLAN_CIPHER_SUITE_TKIP) {
			/* the firmware wants the RX MIC key first */
			memcpy(req.key + 16, key->key + 24, 8);
			memcpy(req.key + 24, key->key + 16, 8);
		}
	}

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(ADD_REMOVE_KEY),
				 &req, sizeof(req), false);
}

int mt7921_mcu_ce_default_key(struct mt792x_dev *dev,
			      struct ieee80211_vif *vif, u8 key_id)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt7921_ce_default_key req = {
		.bss_idx = mvif->bss_conf.mt76.idx,
		.key_id = key_id,
		.unicast = 1,
		.multicast = 1,
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(DEFAULT_KEY_ID),
				 &req, sizeof(req), false);
}

static int
mt7921_mcu_ce_set_roc(struct mt792x_phy *phy, struct mt792x_vif *vif,
		      struct ieee80211_channel *chan, int duration,
		      enum mt7921_roc_req type, u8 token_id)
{
	struct mt7921_ce_ch_privilege req = {
		.bss_idx = vif->bss_conf.mt76.idx,
		.token_id = token_id,
		.action = MT7921_CE_CH_REQ,
		.primary_chan = chan->hw_value,
		.sco = MT7921_CE_SCO_NONE,
		.band = mt7921_mcu_ce_band(chan->band),
		.width = MT7921_CE_CW_20_40MHZ,
		.center_chan = chan->hw_value,
		.req_type = type,
		.dbdc_band = MT7921_CE_DBDC_BAND_AUTO,
		.max_interval = cpu_to_le32(duration),
	};

	BUILD_BUG_ON(sizeof(req) != 24);

	return mt76_mcu_send_msg(&phy->dev->mt76, MCU_CE_CMD(SET_ROC),
				 &req, sizeof(req), false);
}

static int
mt7921_mcu_ce_abort_roc(struct mt792x_phy *phy, struct mt792x_vif *vif,
			u8 token_id)
{
	struct mt7921_ce_ch_privilege req = {
		.bss_idx = vif->bss_conf.mt76.idx,
		.token_id = token_id,
		.action = MT7921_CE_CH_ABORT,
		.dbdc_band = MT7921_CE_DBDC_BAND_AUTO,
	};

	return mt76_mcu_send_msg(&phy->dev->mt76, MCU_CE_CMD(SET_ROC),
				 &req, sizeof(req), false);
}

static int
mt7921_mcu_ce_bss_ps(struct mt792x_dev *dev, struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt7921_ce_ps_profile req = {
		.bss_idx = mvif->bss_conf.mt76.idx,
		/* 0: stay awake, 2: fast power save */
		.profile = vif->cfg.ps ? 2 : 0,
	};

	if (vif->type != NL80211_IFTYPE_STATION)
		return -EOPNOTSUPP;

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(SET_PS_PROFILE),
				 &req, sizeof(req), false);
}

int mt7921_mcu_ce_bss_created(struct mt792x_dev *dev,
			      struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt7921_ce_bss_created req = {
		.bss_idx = mvif->bss_conf.mt76.idx,
		.dtim_period = vif->bss_conf.dtim_period,
		.bcn_interval = cpu_to_le16(vif->bss_conf.beacon_int),
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(SET_BSS_CREATED),
				 &req, sizeof(req), false);
}

static int
mt7921_mcu_ce_beacon(struct mt792x_dev *dev, struct ieee80211_hw *hw,
		     struct ieee80211_vif *vif, bool enable)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct ieee80211_mutable_offsets offs;
	struct mt7921_ce_beacon *req;
	struct ieee80211_mgmt *mgmt;
	struct sk_buff *bcn, *skb;
	int hdr_len, tim_end;

	if (!enable) {
		struct mt7921_ce_beacon del = {
			.method = MT7921_CE_BCN_DELETE_ALL,
			.bss_idx = mvif->bss_conf.mt76.idx,
		};

		return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(UPDATE_BEACON),
					 &del, sizeof(del), false);
	}

	bcn = ieee80211_beacon_get_template(hw, vif, &offs, 0);
	if (!bcn)
		return -EINVAL;

	hdr_len = offsetof(struct ieee80211_mgmt, u.beacon.variable);
	tim_end = offs.tim_offset + offs.tim_length;
	if (bcn->len < hdr_len || offs.tim_offset < hdr_len ||
	    tim_end > bcn->len) {
		dev_kfree_skb(bcn);
		return -EINVAL;
	}

	skb = mt76_mcu_msg_alloc(&dev->mt76, NULL, sizeof(*req) + bcn->len);
	if (!skb) {
		dev_kfree_skb(bcn);
		return -ENOMEM;
	}

	mgmt = (struct ieee80211_mgmt *)bcn->data;
	req = skb_put_zero(skb, sizeof(*req));
	req->method = MT7921_CE_BCN_UPDATE_ALL;
	req->bss_idx = mvif->bss_conf.mt76.idx;
	req->capability = mgmt->u.beacon.capab_info;
	/* the firmware builds the header and the TIM, hand it the rest */
	skb_put_data(skb, bcn->data + hdr_len, offs.tim_offset - hdr_len);
	skb_put_data(skb, bcn->data + tim_end, bcn->len - tim_end);
	req->ie_len = cpu_to_le16(skb->len - sizeof(*req));
	dev_kfree_skb(bcn);

	return mt76_mcu_skb_send_msg(&dev->mt76, skb,
				     MCU_CE_CMD(UPDATE_BEACON), false);
}

static int
mt7921_mcu_ce_set_monitor(struct mt792x_dev *dev, bool enable,
			  struct cfg80211_chan_def *chandef)
{
	struct mt7921_ce_monitor req = {
		.enable = enable,
	};

	if (chandef) {
		req.band = mt7921_mcu_ce_band(chandef->chan->band);
		req.primary_chan = chandef->chan->hw_value;
		mt7921_mcu_ce_chan(chandef, &req.sco, &req.width,
				   &req.center_chan, &req.center_chan2);
	}

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(SET_MONITOR),
				 &req, sizeof(req), false);
}

int mt7921_mcu_ce_set_rxfilter(struct mt792x_dev *dev, unsigned int fif)
{
	struct {
		__le32 filter;
		u8 rsv[64];
	} __packed req = {
		/* directed, multicast and broadcast data, plus the management
		 * frames mac80211 has to see: action, auth, assoc request
		 */
		.filter = cpu_to_le32(BIT(0) | BIT(1) | BIT(3) |
				      BIT(30) | BIT(29) | BIT(28)),
	};

	if (fif & FIF_ALLMULTI)
		req.filter |= cpu_to_le32(BIT(2));
	if (fif & FIF_OTHER_BSS)
		req.filter |= cpu_to_le32(BIT(5));
	if (fif & FIF_PROBE_REQ)
		req.filter |= cpu_to_le32(BIT(31));

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(SET_RX_FILTER),
				 &req, sizeof(req), false);
}

static int
mt7921_mcu_ce_update_arp_filter(struct mt792x_dev *dev,
				struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	int i, len = min_t(int, vif->cfg.arp_addr_cnt,
			   IEEE80211_BSS_ARP_ADDR_LIST_LEN);
	struct mt7921_ce_ip_addr *req;
	struct sk_buff *skb;

	skb = mt76_mcu_msg_alloc(&dev->mt76, NULL,
				 struct_size(req, ip, len));
	if (!skb)
		return -ENOMEM;

	req = skb_put_zero(skb, struct_size(req, ip, len));
	req->bss_idx = mvif->bss_conf.mt76.idx;
	req->count = len;
	req->version = 1;
	for (i = 0; i < len; i++)
		memcpy(req->ip[i].addr, &vif->cfg.arp_addr_list[i],
		       sizeof(req->ip[i].addr));

	return mt76_mcu_skb_send_msg(&dev->mt76, skb,
				     MCU_CE_CMD(SET_IP_ADDR), false);
}

/* Entry points shared by both firmwares. With the linux-firmware image each
 * of them makes the very call the driver made before.
 */
int mt7921_mcu_add_dev(struct mt792x_dev *dev, struct ieee80211_vif *vif,
		       bool enable)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;

	if (dev->fw_legacy)
		return mt7921_mcu_ce_bss_activate(dev, vif, enable);

	return mt76_connac_mcu_uni_add_dev(&dev->mphy, &vif->bss_conf,
					   &mvif->bss_conf.mt76,
					   &mvif->sta.deflink.wcid, enable);
}

int mt7921_mcu_add_bss(struct mt792x_dev *dev, struct ieee80211_vif *vif,
		       struct ieee80211_sta *sta, bool enable,
		       struct ieee80211_chanctx_conf *ctx)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;

	if (dev->fw_legacy)
		return mt7921_mcu_ce_bss_info(dev, vif, sta, enable);

	return mt76_connac_mcu_uni_add_bss(&dev->mphy, vif,
					   &mvif->sta.deflink.wcid, enable,
					   ctx);
}

int mt7921_mcu_set_chctx(struct mt792x_dev *dev, struct ieee80211_vif *vif,
			 struct ieee80211_chanctx_conf *ctx)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;

	if (dev->fw_legacy)
		return mt7921_mcu_ce_bss_rlm(dev, vif, ctx);

	return mt76_connac_mcu_uni_set_chctx(mvif->phy->mt76,
					     &mvif->bss_conf.mt76, ctx);
}

int mt7921_mcu_add_key(struct mt792x_dev *dev, struct ieee80211_vif *vif,
		       struct ieee80211_sta *sta, struct mt76_wcid *wcid,
		       struct mt76_connac_sta_key_conf *sta_key_conf,
		       struct ieee80211_key_conf *key, enum set_key_cmd cmd)
{
	if (dev->fw_legacy)
		return mt7921_mcu_ce_add_key(dev, vif, sta, wcid, key, cmd);

	return mt76_connac_mcu_add_key(&dev->mt76, vif, sta_key_conf, key,
				       MCU_UNI_CMD(STA_REC_UPDATE), wcid, cmd);
}

#ifdef CONFIG_PM
int mt7921_mcu_set_hif_suspend(struct mt792x_dev *dev, bool suspend)
{
	return mt76_connac_mcu_set_hif_suspend(&dev->mt76, suspend, true);
}
EXPORT_SYMBOL_GPL(mt7921_mcu_set_hif_suspend);

int mt7921_mcu_update_gtk_rekey(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct cfg80211_gtk_rekey_data *key)
{
	return mt76_connac_mcu_update_gtk_rekey(hw, vif, key);
}
#endif /* CONFIG_PM */

int mt7921_mcu_update_arp_filter(struct mt792x_dev *dev,
				 struct ieee80211_vif *vif,
				 struct ieee80211_bss_conf *info)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;

	if (dev->fw_legacy)
		return mt7921_mcu_ce_update_arp_filter(dev, vif);

	return mt76_connac_mcu_update_arp_filter(&dev->mt76,
						 &mvif->bss_conf.mt76, info);
}

static void
mt7921_mcu_ce_roc_event(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt7921_ce_ch_privilege *grant;

	if (skb->len < sizeof(struct mt76_connac2_mcu_rxd) + sizeof(*grant))
		return;

	grant = (struct mt7921_ce_ch_privilege *)
		(skb->data + sizeof(struct mt76_connac2_mcu_rxd));

	if (grant->req_type == MT7921_ROC_REQ_ROC)
		ieee80211_ready_on_channel(dev->mt76.phy.hw);

	dev->phy.roc_grant = true;
	wake_up(&dev->phy.roc_wait);
	mod_timer(&dev->phy.roc_timer,
		  jiffies + msecs_to_jiffies(le32_to_cpu(grant->max_interval)));
}

static void
mt7921_mcu_ce_tx_addba_event(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt7921_ce_tx_addba *ba;
	struct mt76_wcid *wcid;

	if (skb->len < sizeof(struct mt76_connac2_mcu_rxd) + sizeof(*ba))
		return;

	ba = (struct mt7921_ce_tx_addba *)
	     (skb->data + sizeof(struct mt76_connac2_mcu_rxd));

	/* the firmware says which TIDs may carry A-MSDU in their A-MPDU */
	rcu_read_lock();
	wcid = mt76_wcid_ptr(dev, ba->sta_idx);
	if (wcid && wcid->sta)
		wcid->amsdu = !!ba->amsdu_bitmap;
	rcu_read_unlock();
}

static void
mt7921_mcu_ce_rx_ba(struct mt792x_dev *dev, u8 sta_idx, u8 tid, bool start,
		    u16 ssn, u16 size)
{
	struct ieee80211_sta *sta;
	struct ieee80211_vif *vif;
	struct mt792x_sta *msta;
	struct mt76_wcid *wcid;

	lockdep_assert_held(&dev->mt76.mutex);

	if (tid >= IEEE80211_NUM_TIDS)
		return;

	/* station removal takes the same mutex, so the entry stays around */
	rcu_read_lock();
	wcid = mt76_wcid_ptr(dev, sta_idx);
	rcu_read_unlock();
	if (!wcid || !wcid->sta)
		return;

	msta = container_of(wcid, struct mt792x_sta, deflink.wcid);
	sta = container_of((void *)msta, struct ieee80211_sta, drv_priv);
	vif = container_of((void *)msta->vif, struct ieee80211_vif, drv_priv);

	if (!start) {
		mt76_rx_aggr_stop(&dev->mt76, wcid, tid);
		ieee80211_stop_rx_ba_session_offl(vif, sta->addr, tid);
		return;
	}

	if (mt76_rx_aggr_start(&dev->mt76, wcid, tid, ssn, size))
		return;

	ieee80211_start_rx_ba_session_offl(vif, sta->addr, tid);
}

/* The firmware answered an ADDBA request or saw a DELBA on its own. All that
 * is left for the host is the reorder window, which cannot be set up from the
 * event context.
 */
void mt7921_ce_ba_work(struct work_struct *work)
{
	struct mt792x_dev *dev = container_of(work, struct mt792x_dev,
					      ce_ba_work);
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&dev->ce_ba_list)) != NULL) {
		struct mt76_connac2_mcu_rxd *rxd;
		int len;

		rxd = (struct mt76_connac2_mcu_rxd *)skb->data;
		len = skb->len - sizeof(*rxd);

		mutex_lock(&dev->mt76.mutex);
		if (rxd->eid == MCU_CE_EVENT_RX_ADDBA) {
			struct mt7921_ce_rx_addba *ba = (void *)(rxd + 1);

			if (len >= (int)sizeof(*ba)) {
				u16 param = le16_to_cpu(ba->param);
				u16 size;

				size = u16_get_bits(param,
						    IEEE80211_ADDBA_PARAM_BUF_SIZE_MASK);
				if (!size)
					size = IEEE80211_MAX_AMPDU_BUF_HT;

				mt7921_mcu_ce_rx_ba(dev, ba->sta_idx,
						    u16_get_bits(param,
								 IEEE80211_ADDBA_PARAM_TID_MASK),
						    true,
						    IEEE80211_SEQ_TO_SN(le16_to_cpu(ba->ssn)),
						    size);
			}
		} else {
			struct mt7921_ce_rx_delba *ba = (void *)(rxd + 1);

			if (len >= (int)sizeof(*ba))
				mt7921_mcu_ce_rx_ba(dev, ba->sta_idx, ba->tid,
						    false, 0, 0);
		}
		mutex_unlock(&dev->mt76.mutex);

		dev_kfree_skb(skb);
	}
}

/* Events only the legacy firmware sends. Returns true once consumed. */
static bool
mt7921_mcu_ce_rx_event(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt76_connac2_mcu_rxd *rxd;

	rxd = (struct mt76_connac2_mcu_rxd *)skb->data;
	switch (rxd->eid) {
	case MCU_CE_EVENT_RX_ADDBA:
	case MCU_CE_EVENT_RX_DELBA:
	case MCU_CE_EVENT_TX_ADDBA:
	case MCU_CE_EVENT_CH_PRIVILEGE:
		break;
	default:
		return false;
	}

	spin_lock_bh(&dev->mcu_event_lock);
	if (dev->mcu_events_stopped ||
	    test_bit(MT76_REMOVED, &dev->mphy.state))
		goto free_skb;

	switch (rxd->eid) {
	case MCU_CE_EVENT_RX_ADDBA:
	case MCU_CE_EVENT_RX_DELBA:
		skb_queue_tail(&dev->ce_ba_list, skb);
		queue_work(dev->mt76.wq, &dev->ce_ba_work);
		goto out;
	case MCU_CE_EVENT_TX_ADDBA:
		mt7921_mcu_ce_tx_addba_event(dev, skb);
		break;
	default:
		mt7921_mcu_ce_roc_event(dev, skb);
		break;
	}
free_skb:
	dev_kfree_skb(skb);
out:
	spin_unlock_bh(&dev->mcu_event_lock);

	return true;
}

#ifdef CONFIG_PM

static int
mt7921_mcu_set_ipv6_ns_filter(struct mt76_dev *dev,
			      struct ieee80211_vif *vif, bool suspend)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct {
		struct {
			u8 bss_idx;
			u8 pad[3];
		} __packed hdr;
		struct mt76_connac_arpns_tlv arpns;
	} req = {
		.hdr = {
			.bss_idx = mvif->bss_conf.mt76.idx,
		},
		.arpns = {
			.tag = cpu_to_le16(UNI_OFFLOAD_OFFLOAD_ND),
			.len = cpu_to_le16(sizeof(struct mt76_connac_arpns_tlv)),
			.mode = suspend,
		},
	};

	return mt76_mcu_send_msg(dev, MCU_UNI_CMD_OFFLOAD, &req, sizeof(req),
				 true);
}

void mt7921_mcu_set_suspend_iter(void *priv, u8 *mac, struct ieee80211_vif *vif)
{
	if (IS_ENABLED(CONFIG_IPV6)) {
		struct mt76_phy *phy = priv;

		mt7921_mcu_set_ipv6_ns_filter(phy->dev, vif,
					      !test_bit(MT76_STATE_RUNNING,
					      &phy->state));
	}

	mt76_connac_mcu_set_suspend_iter(priv, mac, vif);
}

#endif /* CONFIG_PM */

static void
mt7921_mcu_uni_roc_event(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt7921_roc_grant_tlv *grant;
	struct mt76_connac2_mcu_rxd *rxd;
	int duration;

	rxd = (struct mt76_connac2_mcu_rxd *)skb->data;
	grant = (struct mt7921_roc_grant_tlv *)(rxd->tlv + 4);

	/* should never happen */
	WARN_ON_ONCE((le16_to_cpu(grant->tag) != UNI_EVENT_ROC_GRANT));

	if (grant->reqtype == MT7921_ROC_REQ_ROC)
		ieee80211_ready_on_channel(dev->mt76.phy.hw);

	dev->phy.roc_grant = true;
	wake_up(&dev->phy.roc_wait);
	duration = le32_to_cpu(grant->max_interval);
	mod_timer(&dev->phy.roc_timer,
		  jiffies + msecs_to_jiffies(duration));
}

static void
mt7921_mcu_scan_event(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt76_phy *mphy = &dev->mt76.phy;
	struct mt792x_phy *phy = mphy->priv;

	spin_lock_bh(&dev->mt76.lock);
	__skb_queue_tail(&phy->scan_event_list, skb);
	spin_unlock_bh(&dev->mt76.lock);

	ieee80211_queue_delayed_work(mphy->hw, &phy->scan_work,
				     MT792x_HW_SCAN_TIMEOUT);
}

static void
mt7921_mcu_connection_loss_iter(void *priv, u8 *mac,
				struct ieee80211_vif *vif)
{
	struct mt76_vif_link *mvif = (struct mt76_vif_link *)vif->drv_priv;
	struct mt76_connac_beacon_loss_event *event = priv;

	if (mvif->idx != event->bss_idx)
		return;

	if (!(vif->driver_flags & IEEE80211_VIF_BEACON_FILTER) ||
	    vif->type != NL80211_IFTYPE_STATION)
		return;

	ieee80211_connection_loss(vif);
}

static void
mt7921_mcu_connection_loss_event(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt76_connac_beacon_loss_event *event;
	struct mt76_phy *mphy = &dev->mt76.phy;

	skb_pull(skb, sizeof(struct mt76_connac2_mcu_rxd));
	event = (struct mt76_connac_beacon_loss_event *)skb->data;

	ieee80211_iterate_active_interfaces_atomic(mphy->hw,
					IEEE80211_IFACE_ITER_RESUME_ALL,
					mt7921_mcu_connection_loss_iter, event);
}

static void
mt7921_mcu_debug_msg_event(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt7921_debug_msg {
		__le16 id;
		u8 type;
		u8 flag;
		__le32 value;
		__le16 len;
		u8 content[512];
	} __packed * msg;

	skb_pull(skb, sizeof(struct mt76_connac2_mcu_rxd));
	msg = (struct mt7921_debug_msg *)skb->data;

	if (msg->type == 3) { /* fw log */
		u16 len = min_t(u16, le16_to_cpu(msg->len), 512);
		int i;

		for (i = 0 ; i < len; i++) {
			if (!msg->content[i])
				msg->content[i] = ' ';
		}
		wiphy_info(mt76_hw(dev)->wiphy, "%.*s", len, msg->content);
	}
}

static void
mt7921_mcu_low_power_event(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt7921_mcu_lp_event {
		u8 state;
		u8 reserved[3];
	} __packed * event;

	skb_pull(skb, sizeof(struct mt76_connac2_mcu_rxd));
	event = (struct mt7921_mcu_lp_event *)skb->data;

	trace_lp_event(dev, event->state);
}

static void
mt7921_mcu_tx_done_event(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt7921_mcu_tx_done_event *event;

	skb_pull(skb, sizeof(struct mt76_connac2_mcu_rxd));
	event = (struct mt7921_mcu_tx_done_event *)skb->data;

	mt7921_mac_add_txs(dev, event->txs);
}

static void
mt7921_mcu_rssi_monitor_iter(void *priv, u8 *mac,
			     struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt76_connac_rssi_notify_event *event = priv;
	enum nl80211_cqm_rssi_threshold_event nl_event;
	s32 rssi = le32_to_cpu(event->rssi[mvif->bss_conf.mt76.idx]);

	if (!rssi)
		return;

	if (!(vif->driver_flags & IEEE80211_VIF_SUPPORTS_CQM_RSSI))
		return;

	if (rssi > vif->bss_conf.cqm_rssi_thold)
		nl_event = NL80211_CQM_RSSI_THRESHOLD_EVENT_HIGH;
	else
		nl_event = NL80211_CQM_RSSI_THRESHOLD_EVENT_LOW;

	ieee80211_cqm_rssi_notify(vif, nl_event, rssi, GFP_ATOMIC);
}

static void
mt7921_mcu_rssi_monitor_event(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt76_connac_rssi_notify_event *event;

	skb_pull(skb, sizeof(struct mt76_connac2_mcu_rxd));
	event = (struct mt76_connac_rssi_notify_event *)skb->data;

	ieee80211_iterate_active_interfaces_atomic(mt76_hw(dev),
						   IEEE80211_IFACE_ITER_RESUME_ALL,
						   mt7921_mcu_rssi_monitor_iter, event);
}

static void
mt7921_mcu_rx_unsolicited_event(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt76_connac2_mcu_rxd *rxd;

	/* Serialize event side effects with mt7921_unregister_device(). */
	spin_lock_bh(&dev->mcu_event_lock);
	if (dev->mcu_events_stopped ||
	    test_bit(MT76_REMOVED, &dev->mphy.state))
		goto free_skb;

	rxd = (struct mt76_connac2_mcu_rxd *)skb->data;
	switch (rxd->eid) {
	case MCU_EVENT_BSS_BEACON_LOSS:
		mt7921_mcu_connection_loss_event(dev, skb);
		break;
	case MCU_EVENT_SCHED_SCAN_DONE:
	case MCU_EVENT_SCAN_DONE:
		mt7921_mcu_scan_event(dev, skb);
		goto out;
	case MCU_EVENT_DBG_MSG:
		mt7921_mcu_debug_msg_event(dev, skb);
		break;
	case MCU_EVENT_COREDUMP:
		dev->fw_assert = true;
		mt76_connac_mcu_coredump_event(&dev->mt76, skb,
					       &dev->coredump);
		goto out;
	case MCU_EVENT_LP_INFO:
		mt7921_mcu_low_power_event(dev, skb);
		break;
	case MCU_EVENT_TX_DONE:
		mt7921_mcu_tx_done_event(dev, skb);
		break;
	case MCU_EVENT_RSSI_NOTIFY:
		mt7921_mcu_rssi_monitor_event(dev, skb);
		break;
	default:
		break;
	}
free_skb:
	dev_kfree_skb(skb);
out:
	spin_unlock_bh(&dev->mcu_event_lock);
}

static void
mt7921_mcu_uni_rx_unsolicited_event(struct mt792x_dev *dev,
				    struct sk_buff *skb)
{
	struct mt76_connac2_mcu_rxd *rxd;

	spin_lock_bh(&dev->mcu_event_lock);
	if (dev->mcu_events_stopped ||
	    test_bit(MT76_REMOVED, &dev->mphy.state))
		goto out;

	rxd = (struct mt76_connac2_mcu_rxd *)skb->data;

	switch (rxd->eid) {
	case MCU_UNI_EVENT_ROC:
		mt7921_mcu_uni_roc_event(dev, skb);
		break;
	default:
		break;
	}
out:
	spin_unlock_bh(&dev->mcu_event_lock);
	dev_kfree_skb(skb);
}

void mt7921_mcu_rx_event(struct mt792x_dev *dev, struct sk_buff *skb)
{
	struct mt76_connac2_mcu_rxd *rxd;

	if (skb_linearize(skb) ||
	    skb->len < offsetofend(struct mt76_connac2_mcu_rxd, ext_eid)) {
		dev_kfree_skb(skb);
		return;
	}

	rxd = (struct mt76_connac2_mcu_rxd *)skb->data;

	if (dev->fw_legacy) {
		if (mt7921_mcu_ce_rx_event(dev, skb))
			return;
	} else if (rxd->option & MCU_UNI_CMD_UNSOLICITED_EVENT) {
		mt7921_mcu_uni_rx_unsolicited_event(dev, skb);
		return;
	}

	if (rxd->eid == 0x6) {
		mt76_mcu_rx_event(&dev->mt76, skb);
		return;
	}

	if (rxd->ext_eid == MCU_EXT_EVENT_RATE_REPORT ||
	    rxd->eid == MCU_EVENT_BSS_BEACON_LOSS ||
	    rxd->eid == MCU_EVENT_SCHED_SCAN_DONE ||
	    rxd->eid == MCU_EVENT_RSSI_NOTIFY ||
	    rxd->eid == MCU_EVENT_SCAN_DONE ||
	    rxd->eid == MCU_EVENT_TX_DONE ||
	    rxd->eid == MCU_EVENT_DBG_MSG ||
	    rxd->eid == MCU_EVENT_COREDUMP ||
	    rxd->eid == MCU_EVENT_LP_INFO ||
	    !rxd->seq)
		mt7921_mcu_rx_unsolicited_event(dev, skb);
	else
		mt76_mcu_rx_event(&dev->mt76, skb);
}

/** starec & wtbl **/
int mt7921_mcu_uni_tx_ba(struct mt792x_dev *dev,
			 struct ieee80211_ampdu_params *params,
			 bool enable)
{
	struct mt792x_sta *msta = (struct mt792x_sta *)params->sta->drv_priv;

	if (enable && !params->amsdu)
		msta->deflink.wcid.amsdu = false;

	return mt76_connac_mcu_sta_ba(&dev->mt76, &msta->vif->bss_conf.mt76, params,
				      MCU_UNI_CMD(STA_REC_UPDATE),
				      enable, true);
}

int mt7921_mcu_uni_rx_ba(struct mt792x_dev *dev,
			 struct ieee80211_ampdu_params *params,
			 bool enable)
{
	struct mt792x_sta *msta = (struct mt792x_sta *)params->sta->drv_priv;

	return mt76_connac_mcu_sta_ba(&dev->mt76, &msta->vif->bss_conf.mt76, params,
				      MCU_UNI_CMD(STA_REC_UPDATE),
				      enable, false);
}

static int mt7921_load_clc(struct mt792x_dev *dev, const char *fw_name)
{
	const struct mt76_connac2_fw_trailer *hdr;
	const struct mt76_connac2_fw_region *region;
	const struct mt7921_clc *clc;
	struct mt76_dev *mdev = &dev->mt76;
	struct mt792x_phy *phy = &dev->phy;
	const struct firmware *fw;
	size_t clc_len, len, offset = 0;
	int ret, i;
	u8 *clc_base = NULL, hw_encap = 0;

	dev->phy.clc_chan_conf = 0xff;
	dev->regd_user = false;
	if (!mt7921_regd_clc_supported(dev))
		return 0;

	if (mt76_is_mmio(&dev->mt76)) {
		ret = mt7921_mcu_read_eeprom(dev, MT_EE_HW_TYPE, &hw_encap);
		if (ret && ret != -ENODATA)
			return ret;
		hw_encap = u8_get_bits(hw_encap, MT_EE_HW_TYPE_ENCAP);
	}

	ret = request_firmware(&fw, fw_name, mdev->dev);
	if (ret)
		return ret;

	if (mt76_connac2_fw_check(fw, NULL)) {
		dev_err(mdev->dev, "Invalid firmware\n");
		ret = -EINVAL;
		goto out;
	}

	hdr = (const void *)(fw->data + fw->size - sizeof(*hdr));
	for (i = 0; i < hdr->n_region; i++) {
		region = (const void *)((const u8 *)hdr -
					(hdr->n_region - i) * sizeof(*region));
		len = le32_to_cpu(region->len);

		if ((region->feature_set & FW_FEATURE_NON_DL) &&
		    region->type == FW_TYPE_CLC) {
			clc_base = (u8 *)(fw->data + offset);
			break;
		}
		offset += len;
	}

	if (!clc_base)
		goto out;

	for (offset = 0; offset < len; offset += clc_len) {
		if (len - offset < sizeof(*clc)) {
			dev_err(mdev->dev, "Invalid CLC record\n");
			ret = -EINVAL;
			goto out;
		}

		clc = (const struct mt7921_clc *)(clc_base + offset);
		clc_len = le32_to_cpu(clc->len);
		if (clc_len < sizeof(*clc) || clc_len > len - offset) {
			dev_err(mdev->dev, "Invalid CLC record\n");
			ret = -EINVAL;
			goto out;
		}

		/* Newer firmware may add records this driver does not use yet */
		if (clc->idx >= ARRAY_SIZE(phy->clc))
			continue;

		/* do not init buf again if chip reset triggered */
		if (phy->clc[clc->idx])
			continue;

		/* header content sanity */
		if (clc->idx == MT7921_CLC_POWER &&
		    u8_get_bits(clc->type, MT_EE_HW_TYPE_ENCAP) != hw_encap)
			continue;

		phy->clc[clc->idx] = devm_kmemdup(mdev->dev, clc,
						  clc_len,
						  GFP_KERNEL);

		if (!phy->clc[clc->idx]) {
			ret = -ENOMEM;
			goto out;
		}
	}

	ret = mt7921_regd_init(phy);
out:
	release_firmware(fw);

	return ret;
}

static void mt7921_mcu_parse_tx_resource(struct mt76_dev *dev,
					 struct sk_buff *skb, int len)
{
	struct mt76_sdio *sdio = &dev->sdio;
	struct mt7921_tx_resource {
		__le32 version;
		__le32 pse_data_quota;
		__le32 pse_mcu_quota;
		__le32 ple_data_quota;
		__le32 ple_mcu_quota;
		__le16 pse_page_size;
		__le16 ple_page_size;
		u8 pp_padding;
		u8 pad[3];
	} __packed * tx_res;

	if (len < offsetofend(struct mt7921_tx_resource, pp_padding))
		return;

	tx_res = (struct mt7921_tx_resource *)skb->data;
	sdio->sched.pse_data_quota = le32_to_cpu(tx_res->pse_data_quota);
	sdio->pse_mcu_quota_max = le32_to_cpu(tx_res->pse_mcu_quota);
	/* The mcu quota usage of this function itself must be taken into consideration */
	sdio->sched.pse_mcu_quota =
		sdio->sched.pse_mcu_quota ? sdio->pse_mcu_quota_max : sdio->pse_mcu_quota_max - 1;
	sdio->sched.ple_data_quota = le32_to_cpu(tx_res->ple_data_quota);
	sdio->sched.pse_page_size = le16_to_cpu(tx_res->pse_page_size);
	sdio->sched.deficit = tx_res->pp_padding;
}

static void mt7921_mcu_parse_phy_cap(struct mt76_dev *dev,
				     struct sk_buff *skb, int len)
{
	struct mt7921_phy_cap {
		u8 ht;
		u8 vht;
		u8 _5g;
		u8 max_bw;
		u8 nss;
		u8 dbdc;
		u8 tx_ldpc;
		u8 rx_ldpc;
		u8 tx_stbc;
		u8 rx_stbc;
		u8 hw_path;
		u8 he;
	} __packed * cap;

	enum {
		WF0_24G,
		WF0_5G
	};

	if (len < offsetofend(struct mt7921_phy_cap, hw_path))
		return;

	cap = (struct mt7921_phy_cap *)skb->data;

	dev->phy.antenna_mask = BIT(cap->nss) - 1;
	dev->phy.chainmask = dev->phy.antenna_mask;
	dev->phy.cap.has_2ghz = cap->hw_path & BIT(WF0_24G);
	dev->phy.cap.has_5ghz = cap->hw_path & BIT(WF0_5G);
}

static int mt7921_mcu_get_nic_capability(struct mt792x_phy *mphy)
{
	struct mt76_connac_cap_hdr {
		__le16 n_element;
		u8 rsv[2];
	} __packed * hdr;
	struct sk_buff *skb;
	struct mt76_phy *phy = mphy->mt76;
	int ret, i;

	ret = mt76_mcu_send_and_get_msg(phy->dev, MCU_CE_CMD(GET_NIC_CAPAB),
					NULL, 0, true, &skb);
	if (ret)
		return ret;

	hdr = (struct mt76_connac_cap_hdr *)skb->data;
	if (skb->len < sizeof(*hdr)) {
		ret = -EINVAL;
		goto out;
	}

	skb_pull(skb, sizeof(*hdr));

	for (i = 0; i < le16_to_cpu(hdr->n_element); i++) {
		struct tlv_hdr {
			__le32 type;
			__le32 len;
		} __packed * tlv = (struct tlv_hdr *)skb->data;
		int len;

		if (skb->len < sizeof(*tlv))
			break;

		skb_pull(skb, sizeof(*tlv));

		len = le32_to_cpu(tlv->len);
		if (skb->len < len)
			break;

		switch (le32_to_cpu(tlv->type)) {
		case MT_NIC_CAP_6G:
			if (len)
				phy->cap.has_6ghz = skb->data[0];
			break;
		case MT_NIC_CAP_MAC_ADDR:
			if (len >= ETH_ALEN)
				memcpy(phy->macaddr, (void *)skb->data, ETH_ALEN);
			break;
		case MT_NIC_CAP_PHY:
			mt7921_mcu_parse_phy_cap(phy->dev, skb, len);
			break;
		case MT_NIC_CAP_TX_RESOURCE:
			if (mt76_is_sdio(phy->dev))
				mt7921_mcu_parse_tx_resource(phy->dev,
							     skb, len);
			break;
		case MT_NIC_CAP_CHIP_CAP:
			/* the legacy firmware uses this tag for something else */
			if (mphy->dev->fw_legacy)
				break;
			if (len >= sizeof(u64))
				memcpy(&mphy->chip_cap, (void *)skb->data,
				       sizeof(u64));
			break;
		default:
			break;
		}
		skb_pull(skb, len);
	}
out:
	dev_kfree_skb(skb);

	return ret;
}

int mt7921_mcu_fw_log_2_host(struct mt792x_dev *dev, u8 ctrl)
{
	struct {
		u8 ctrl_val;
		u8 pad[3];
	} data = {
		.ctrl_val = ctrl
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(FWLOG_2_HOST),
				 &data, sizeof(data), false);
}

int mt7921_run_firmware(struct mt792x_dev *dev)
{
	int err;

	err = mt792x_load_firmware(dev);
	if (err)
		return err;

	err = mt7921_mcu_get_nic_capability(&dev->phy);
	if (err)
		return err;

	if (dev->fw_legacy) {
		set_bit(MT76_STATE_MCU_RUNNING, &dev->mphy.state);

		err = mt7921_mcu_ce_basic_config(dev);
		if (err)
			return err;

		return mt7921_mcu_fw_log_2_host(dev, 1);
	}

	err = mt7921_load_clc(dev, mt792x_ram_name(dev));
	if (err)
		return err;
	set_bit(MT76_STATE_MCU_RUNNING, &dev->mphy.state);

	err = mt7921_mcu_set_thermal_protect(dev);
	if (err)
		return err;

	return mt7921_mcu_fw_log_2_host(dev, 1);
}
EXPORT_SYMBOL_GPL(mt7921_run_firmware);

int mt7921_mcu_radio_led_ctrl(struct mt792x_dev *dev, u8 value)
{
	struct {
		u8 ctrlid;
		u8 rsv[3];
	} __packed req = {
		.ctrlid = value,
	};

	if (dev->fw_legacy)
		return 0;

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(ID_RADIO_ON_OFF_CTRL),
				&req, sizeof(req), false);
}
EXPORT_SYMBOL_GPL(mt7921_mcu_radio_led_ctrl);

int mt7921_mcu_set_tx(struct mt792x_dev *dev, struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct edca {
		__le16 cw_min;
		__le16 cw_max;
		__le16 txop;
		__le16 aifs;
		u8 guardtime;
		u8 acm;
	} __packed;
	struct mt7921_mcu_tx {
		struct edca edca[IEEE80211_NUM_ACS];
		u8 bss_idx;
		u8 qos;
		u8 wmm_idx;
		u8 pad;
	} __packed req = {
		.bss_idx = mvif->bss_conf.mt76.idx,
		.qos = vif->bss_conf.qos,
		.wmm_idx = mvif->bss_conf.mt76.wmm_idx,
	};
	struct mu_edca {
		u8 cw_min;
		u8 cw_max;
		u8 aifsn;
		u8 acm;
		u8 timer;
		u8 padding[3];
	};
	struct mt7921_mcu_mu_tx {
		u8 ver;
		u8 pad0;
		__le16 len;
		u8 bss_idx;
		u8 qos;
		u8 wmm_idx;
		u8 pad1;
		struct mu_edca edca[IEEE80211_NUM_ACS];
		u8 pad3[32];
	} __packed req_mu = {
		.bss_idx = mvif->bss_conf.mt76.idx,
		.qos = vif->bss_conf.qos,
		.wmm_idx = mvif->bss_conf.mt76.wmm_idx,
	};
	static const int to_aci[] = { 1, 0, 2, 3 };
	int ac, ret;

	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
		struct ieee80211_tx_queue_params *q = &mvif->bss_conf.queue_params[ac];
		struct edca *e = &req.edca[to_aci[ac]];

		e->aifs = cpu_to_le16(q->aifs);
		e->txop = cpu_to_le16(q->txop);

		if (q->cw_min)
			e->cw_min = cpu_to_le16(q->cw_min);
		else
			e->cw_min = cpu_to_le16(5);

		if (q->cw_max)
			e->cw_max = cpu_to_le16(q->cw_max);
		else
			e->cw_max = cpu_to_le16(10);
	}

	ret = mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(SET_EDCA_PARMS), &req,
				sizeof(req), false);
	if (ret)
		return ret;

	if (!vif->bss_conf.he_support)
		return 0;

	for (ac = 0; ac < IEEE80211_NUM_ACS; ac++) {
		struct ieee80211_he_mu_edca_param_ac_rec *q;
		struct mu_edca *e;

		if (!mvif->bss_conf.queue_params[ac].mu_edca)
			break;

		q = &mvif->bss_conf.queue_params[ac].mu_edca_param_rec;
		e = &(req_mu.edca[to_aci[ac]]);

		e->cw_min = q->ecw_min_max & 0xf;
		e->cw_max = (q->ecw_min_max & 0xf0) >> 4;
		e->aifsn = q->aifsn;
		e->timer = q->mu_edca_timer;
	}

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(SET_MU_EDCA_PARMS),
				 &req_mu, sizeof(req_mu), false);
}

int mt7921_mcu_set_roc(struct mt792x_phy *phy, struct mt792x_vif *vif,
		       struct ieee80211_channel *chan, int duration,
		       enum mt7921_roc_req type, u8 token_id)
{
	int center_ch = ieee80211_frequency_to_channel(chan->center_freq);
	struct mt792x_dev *dev = phy->dev;
	struct {
		struct {
			u8 rsv[4];
		} __packed hdr;
		struct roc_acquire_tlv {
			__le16 tag;
			__le16 len;
			u8 bss_idx;
			u8 tokenid;
			u8 control_channel;
			u8 sco;
			u8 band;
			u8 bw;
			u8 center_chan;
			u8 center_chan2;
			u8 bw_from_ap;
			u8 center_chan_from_ap;
			u8 center_chan2_from_ap;
			u8 reqtype;
			__le32 maxinterval;
			u8 dbdcband;
			u8 rsv[3];
		} __packed roc;
	} __packed req = {
		.roc = {
			.tag = cpu_to_le16(UNI_ROC_ACQUIRE),
			.len = cpu_to_le16(sizeof(struct roc_acquire_tlv)),
			.tokenid = token_id,
			.reqtype = type,
			.maxinterval = cpu_to_le32(duration),
			.bss_idx = vif->bss_conf.mt76.idx,
			.control_channel = chan->hw_value,
			.bw = CMD_CBW_20MHZ,
			.bw_from_ap = CMD_CBW_20MHZ,
			.center_chan = center_ch,
			.center_chan_from_ap = center_ch,
			.dbdcband = 0xff, /* auto */
		},
	};

	if (dev->fw_legacy)
		return mt7921_mcu_ce_set_roc(phy, vif, chan, duration, type,
					     token_id);

	if (chan->hw_value < center_ch)
		req.roc.sco = 1; /* SCA */
	else if (chan->hw_value > center_ch)
		req.roc.sco = 3; /* SCB */

	switch (chan->band) {
	case NL80211_BAND_6GHZ:
		req.roc.band = 3;
		break;
	case NL80211_BAND_5GHZ:
		req.roc.band = 2;
		break;
	default:
		req.roc.band = 1;
		break;
	}

	return mt76_mcu_send_msg(&dev->mt76, MCU_UNI_CMD(ROC),
				 &req, sizeof(req), false);
}

int mt7921_mcu_abort_roc(struct mt792x_phy *phy, struct mt792x_vif *vif,
			 u8 token_id)
{
	struct mt792x_dev *dev = phy->dev;
	struct {
		struct {
			u8 rsv[4];
		} __packed hdr;
		struct roc_abort_tlv {
			__le16 tag;
			__le16 len;
			u8 bss_idx;
			u8 tokenid;
			u8 dbdcband;
			u8 rsv[5];
		} __packed abort;
	} __packed req = {
		.abort = {
			.tag = cpu_to_le16(UNI_ROC_ABORT),
			.len = cpu_to_le16(sizeof(struct roc_abort_tlv)),
			.tokenid = token_id,
			.bss_idx = vif->bss_conf.mt76.idx,
			.dbdcband = 0xff, /* auto*/
		},
	};

	if (dev->fw_legacy)
		return mt7921_mcu_ce_abort_roc(phy, vif, token_id);

	return mt76_mcu_send_msg(&dev->mt76, MCU_UNI_CMD(ROC),
				 &req, sizeof(req), false);
}

int mt7921_mcu_set_chan_info(struct mt792x_phy *phy, int cmd)
{
	struct mt792x_dev *dev = phy->dev;
	struct cfg80211_chan_def *chandef = &phy->mt76->chandef;
	int freq1 = chandef->center_freq1;
	struct {
		u8 control_ch;
		u8 center_ch;
		u8 bw;
		u8 tx_streams_num;
		u8 rx_streams;	/* mask or num */
		u8 switch_reason;
		u8 band_idx;
		u8 center_ch2;	/* for 80+80 only */
		__le16 cac_case;
		u8 channel_band;
		u8 rsv0;
		__le32 outband_freq;
		u8 txpower_drop;
		u8 ap_bw;
		u8 ap_center_ch;
		u8 rsv1[57];
	} __packed req = {
		.control_ch = chandef->chan->hw_value,
		.center_ch = ieee80211_frequency_to_channel(freq1),
		.bw = mt76_connac_chan_bw(chandef),
		.tx_streams_num = hweight8(phy->mt76->antenna_mask),
		.rx_streams = phy->mt76->antenna_mask,
		.band_idx = phy != &dev->phy,
	};

	if (chandef->chan->band == NL80211_BAND_6GHZ)
		req.channel_band = 2;
	else
		req.channel_band = chandef->chan->band;

	if (cmd == MCU_EXT_CMD(SET_RX_PATH) ||
	    (dev->mt76.hw->conf.flags & IEEE80211_CONF_MONITOR &&
	     !phy->mt76->offchannel))
		req.switch_reason = CH_SWITCH_NORMAL;
	else if (phy->mt76->offchannel)
		req.switch_reason = CH_SWITCH_SCAN_BYPASS_DPD;
	else if (!cfg80211_reg_can_beacon(dev->mt76.hw->wiphy, chandef,
					  NL80211_IFTYPE_AP))
		req.switch_reason = CH_SWITCH_DFS;
	else
		req.switch_reason = CH_SWITCH_NORMAL;

	if (cmd == MCU_EXT_CMD(CHANNEL_SWITCH))
		req.rx_streams = hweight8(req.rx_streams);

	if (chandef->width == NL80211_CHAN_WIDTH_80P80) {
		int freq2 = chandef->center_freq2;

		req.center_ch2 = ieee80211_frequency_to_channel(freq2);
	}

	return mt76_mcu_send_msg(&dev->mt76, cmd, &req, sizeof(req), true);
}

int mt7921_mcu_set_eeprom(struct mt792x_dev *dev)
{
	struct req_hdr {
		u8 buffer_mode;
		u8 format;
		__le16 len;
	} __packed req = {
		.buffer_mode = EE_MODE_EFUSE,
		.format = EE_FORMAT_WHOLE,
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(EFUSE_BUFFER_MODE),
				 &req, sizeof(req), true);
}
EXPORT_SYMBOL_GPL(mt7921_mcu_set_eeprom);

int mt7921_mcu_uni_bss_ps(struct mt792x_dev *dev, struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct {
		struct {
			u8 bss_idx;
			u8 pad[3];
		} __packed hdr;
		struct ps_tlv {
			__le16 tag;
			__le16 len;
			u8 ps_state; /* 0: device awake
				      * 1: static power save
				      * 2: dynamic power saving
				      * 3: enter TWT power saving
				      * 4: leave TWT power saving
				      */
			u8 pad[3];
		} __packed ps;
	} __packed ps_req = {
		.hdr = {
			.bss_idx = mvif->bss_conf.mt76.idx,
		},
		.ps = {
			.tag = cpu_to_le16(UNI_BSS_INFO_PS),
			.len = cpu_to_le16(sizeof(struct ps_tlv)),
			.ps_state = vif->cfg.ps ? 2 : 0,
		},
	};

	if (dev->fw_legacy)
		return mt7921_mcu_ce_bss_ps(dev, vif);

	if (vif->type != NL80211_IFTYPE_STATION)
		return -EOPNOTSUPP;

	return mt76_mcu_send_msg(&dev->mt76, MCU_UNI_CMD(BSS_INFO_UPDATE),
				 &ps_req, sizeof(ps_req), true);
}

static int
mt7921_mcu_uni_bss_bcnft(struct mt792x_dev *dev, struct ieee80211_vif *vif,
			 bool enable)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct {
		struct {
			u8 bss_idx;
			u8 pad[3];
		} __packed hdr;
		struct bcnft_tlv {
			__le16 tag;
			__le16 len;
			__le16 bcn_interval;
			u8 dtim_period;
			u8 pad;
		} __packed bcnft;
	} __packed bcnft_req = {
		.hdr = {
			.bss_idx = mvif->bss_conf.mt76.idx,
		},
		.bcnft = {
			.tag = cpu_to_le16(UNI_BSS_INFO_BCNFT),
			.len = cpu_to_le16(sizeof(struct bcnft_tlv)),
			.bcn_interval = cpu_to_le16(vif->bss_conf.beacon_int),
			.dtim_period = vif->bss_conf.dtim_period,
		},
	};

	if (vif->type != NL80211_IFTYPE_STATION)
		return 0;

	return mt76_mcu_send_msg(&dev->mt76, MCU_UNI_CMD(BSS_INFO_UPDATE),
				 &bcnft_req, sizeof(bcnft_req), true);
}

int
mt7921_mcu_set_bss_pm(struct mt792x_dev *dev, struct ieee80211_vif *vif,
		      bool enable)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct {
		u8 bss_idx;
		u8 dtim_period;
		__le16 aid;
		__le16 bcn_interval;
		__le16 atim_window;
		u8 uapsd;
		u8 bmc_delivered_ac;
		u8 bmc_triggered_ac;
		u8 pad;
	} req = {
		.bss_idx = mvif->bss_conf.mt76.idx,
		.aid = cpu_to_le16(vif->cfg.aid),
		.dtim_period = vif->bss_conf.dtim_period,
		.bcn_interval = cpu_to_le16(vif->bss_conf.beacon_int),
	};
	struct {
		u8 bss_idx;
		u8 pad[3];
	} req_hdr = {
		.bss_idx = mvif->bss_conf.mt76.idx,
	};
	int err;

	/* the legacy firmware is told about the abort on disconnect only */
	if (!dev->fw_legacy || !enable) {
		err = mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(SET_BSS_ABORT),
					&req_hdr, sizeof(req_hdr), false);
		if (err < 0 || !enable)
			return err;
	}

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(SET_BSS_CONNECTED),
				 &req, sizeof(req), false);
}

int mt7921_mcu_sta_update(struct mt792x_dev *dev, struct ieee80211_sta *sta,
			  struct ieee80211_vif *vif, bool enable,
			  enum mt76_sta_info_state state)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	int rssi = -ewma_rssi_read(&mvif->bss_conf.rssi);
	struct mt76_sta_cmd_info info = {
		.sta = sta,
		.vif = vif,
		.enable = enable,
		.cmd = MCU_UNI_CMD(STA_REC_UPDATE),
		.state = state,
		.offload_fw = true,
		.rcpi = to_rcpi(rssi),
	};
	struct mt792x_sta *msta;

	/* the interface's own entry comes with the BSS in the legacy flow */
	if (dev->fw_legacy)
		return sta ? mt7921_mcu_ce_sta_update(dev, sta, vif, enable,
						      state) : 0;

	msta = sta ? (struct mt792x_sta *)sta->drv_priv : NULL;
	info.wcid = msta ? &msta->deflink.wcid : &mvif->sta.deflink.wcid;
	info.newly = msta ? state != MT76_STA_INFO_STATE_ASSOC : true;

	return mt76_connac_mcu_sta_cmd(&dev->mphy, &info);
}

int mt7921_mcu_set_beacon_filter(struct mt792x_dev *dev,
				 struct ieee80211_vif *vif,
				 bool enable)
{
#define MT7921_FIF_BIT_CLR		BIT(1)
#define MT7921_FIF_BIT_SET		BIT(0)
	int err;

	if (enable) {
		if (dev->fw_legacy)
			err = mt7921_mcu_set_bss_pm(dev, vif, true);
		else
			err = mt7921_mcu_uni_bss_bcnft(dev, vif, true);
		if (err)
			return err;

		err = mt7921_mcu_set_rxfilter(dev, 0,
					      MT7921_FIF_BIT_SET,
					      MT_WF_RFCR_DROP_OTHER_BEACON);
		if (err)
			return err;

		return 0;
	}

	err = mt7921_mcu_set_bss_pm(dev, vif, false);
	if (err)
		return err;

	err = mt7921_mcu_set_rxfilter(dev, 0,
				      MT7921_FIF_BIT_CLR,
				      MT_WF_RFCR_DROP_OTHER_BEACON);
	if (err)
		return err;

	return 0;
}

int mt7921_get_txpwr_info(struct mt792x_dev *dev, struct mt7921_txpwr *txpwr)
{
	struct mt7921_txpwr_event *event;
	struct mt7921_txpwr_req req = {
		.dbdc_idx = 0,
	};
	struct sk_buff *skb;
	int ret;

	ret = mt76_mcu_send_and_get_msg(&dev->mt76, MCU_CE_CMD(GET_TXPWR),
					&req, sizeof(req), true, &skb);
	if (ret)
		return ret;

	if (skb->len < sizeof(*event)) {
		dev_kfree_skb(skb);
		return -EINVAL;
	}

	event = (struct mt7921_txpwr_event *)skb->data;
	memcpy(txpwr, &event->txpwr, sizeof(event->txpwr));

	dev_kfree_skb(skb);

	return 0;
}

int mt7921_mcu_set_sniffer(struct mt792x_dev *dev, struct ieee80211_vif *vif,
			   bool enable)
{
	struct mt76_vif_link *mvif = (struct mt76_vif_link *)vif->drv_priv;
	struct {
		struct {
			u8 band_idx;
			u8 pad[3];
		} __packed hdr;
		struct sniffer_enable_tlv {
			__le16 tag;
			__le16 len;
			u8 enable;
			u8 pad[3];
		} __packed enable;
	} req = {
		.hdr = {
			.band_idx = mvif->band_idx,
		},
		.enable = {
			.tag = cpu_to_le16(0),
			.len = cpu_to_le16(sizeof(struct sniffer_enable_tlv)),
			.enable = enable,
		},
	};

	if (dev->fw_legacy)
		return mt7921_mcu_ce_set_monitor(dev, enable,
						 &dev->mphy.chandef);

	return mt76_mcu_send_msg(&dev->mt76, MCU_UNI_CMD(SNIFFER), &req, sizeof(req),
				 true);
}

int mt7921_mcu_config_sniffer(struct mt792x_vif *vif,
			      struct ieee80211_chanctx_conf *ctx)
{
	struct cfg80211_chan_def *chandef = &ctx->def;
	int freq1 = chandef->center_freq1, freq2 = chandef->center_freq2;
	static const u8 ch_band[] = {
		[NL80211_BAND_2GHZ] = 1,
		[NL80211_BAND_5GHZ] = 2,
		[NL80211_BAND_6GHZ] = 3,
	};
	static const u8 ch_width[] = {
		[NL80211_CHAN_WIDTH_20_NOHT] = 0,
		[NL80211_CHAN_WIDTH_20] = 0,
		[NL80211_CHAN_WIDTH_40] = 0,
		[NL80211_CHAN_WIDTH_80] = 1,
		[NL80211_CHAN_WIDTH_160] = 2,
		[NL80211_CHAN_WIDTH_80P80] = 3,
		[NL80211_CHAN_WIDTH_5] = 4,
		[NL80211_CHAN_WIDTH_10] = 5,
		[NL80211_CHAN_WIDTH_320] = 6,
	};
	struct {
		struct {
			u8 band_idx;
			u8 pad[3];
		} __packed hdr;
		struct config_tlv {
			__le16 tag;
			__le16 len;
			u16 aid;
			u8 ch_band;
			u8 bw;
			u8 control_ch;
			u8 sco;
			u8 center_ch;
			u8 center_ch2;
			u8 drop_err;
			u8 pad[3];
		} __packed tlv;
	} __packed req = {
		.hdr = {
			.band_idx = vif->bss_conf.mt76.band_idx,
		},
		.tlv = {
			.tag = cpu_to_le16(1),
			.len = cpu_to_le16(sizeof(req.tlv)),
			.control_ch = chandef->chan->hw_value,
			.center_ch = ieee80211_frequency_to_channel(freq1),
			.drop_err = !(vif->phy->rxfilter & MT7921_FILTER_FCSFAIL),
		},
	};
	if (chandef->chan->band < ARRAY_SIZE(ch_band))
		req.tlv.ch_band = ch_band[chandef->chan->band];
	if (chandef->width < ARRAY_SIZE(ch_width))
		req.tlv.bw = ch_width[chandef->width];

	if (freq2)
		req.tlv.center_ch2 = ieee80211_frequency_to_channel(freq2);

	if (req.tlv.control_ch < req.tlv.center_ch)
		req.tlv.sco = 1; /* SCA */
	else if (req.tlv.control_ch > req.tlv.center_ch)
		req.tlv.sco = 3; /* SCB */

	if (vif->phy->dev->fw_legacy)
		return mt7921_mcu_ce_set_monitor(vif->phy->dev, true, chandef);

	return mt76_mcu_send_msg(vif->phy->mt76->dev, MCU_UNI_CMD(SNIFFER),
				 &req, sizeof(req), true);
}

int
mt7921_mcu_uni_add_beacon_offload(struct mt792x_dev *dev,
				  struct ieee80211_hw *hw,
				  struct ieee80211_vif *vif,
				  bool enable)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct mt76_wcid *wcid = &dev->mt76.global_wcid;
	struct ieee80211_mutable_offsets offs;
	struct {
		struct req_hdr {
			u8 bss_idx;
			u8 pad[3];
		} __packed hdr;
		struct bcn_content_tlv {
			__le16 tag;
			__le16 len;
			__le16 tim_ie_pos;
			__le16 csa_ie_pos;
			__le16 bcc_ie_pos;
			/* 0: disable beacon offload
			 * 1: enable beacon offload
			 * 2: update probe respond offload
			 */
			u8 enable;
			/* 0: legacy format (TXD + payload)
			 * 1: only cap field IE
			 */
			u8 type;
			__le16 pkt_len;
			u8 pkt[512];
		} __packed beacon_tlv;
	} req = {
		.hdr = {
			.bss_idx = mvif->bss_conf.mt76.idx,
		},
		.beacon_tlv = {
			.tag = cpu_to_le16(UNI_BSS_INFO_BCN_CONTENT),
			.len = cpu_to_le16(sizeof(struct bcn_content_tlv)),
			.enable = enable,
		},
	};
	struct sk_buff *skb;

	if (dev->fw_legacy)
		return mt7921_mcu_ce_beacon(dev, hw, vif, enable);

	/* support enable/update process only
	 * disable flow would be handled in bss stop handler automatically
	 */
	if (!enable)
		return -EOPNOTSUPP;

	skb = ieee80211_beacon_get_template(mt76_hw(dev), vif, &offs, 0);
	if (!skb)
		return -EINVAL;

	if (skb->len > 512 - MT_TXD_SIZE) {
		dev_err(dev->mt76.dev, "beacon size limit exceed\n");
		dev_kfree_skb(skb);
		return -EINVAL;
	}

	mt76_connac2_mac_write_txwi(&dev->mt76, (__le32 *)(req.beacon_tlv.pkt),
				    skb, wcid, NULL, 0, 0, BSS_CHANGED_BEACON);
	memcpy(req.beacon_tlv.pkt + MT_TXD_SIZE, skb->data, skb->len);
	req.beacon_tlv.pkt_len = cpu_to_le16(MT_TXD_SIZE + skb->len);
	req.beacon_tlv.tim_ie_pos = cpu_to_le16(MT_TXD_SIZE + offs.tim_offset);

	if (offs.cntdwn_counter_offs[0]) {
		u16 csa_offs;

		csa_offs = MT_TXD_SIZE + offs.cntdwn_counter_offs[0] - 4;
		req.beacon_tlv.csa_ie_pos = cpu_to_le16(csa_offs);
	}
	dev_kfree_skb(skb);

	return mt76_mcu_send_msg(&dev->mt76, MCU_UNI_CMD(BSS_INFO_UPDATE),
				 &req, sizeof(req), true);
}

static
int __mt7921_mcu_set_clc(struct mt792x_dev *dev, u8 *alpha2,
			 enum environment_cap env_cap,
			 struct mt7921_clc *clc,
			 u8 idx)
{
#define CLC_CAP_EVT_EN BIT(0)
#define CLC_CAP_DTS_EN BIT(1)
	struct sk_buff *skb, *ret_skb = NULL;
	struct {
		u8 ver;
		u8 pad0;
		__le16 len;
		u8 idx;
		u8 env;
		u8 acpi_conf;
		u8 cap;
		u8 alpha2[2];
		u8 type[2];
		u8 env_6g;
		u8 mtcl_conf;
		u8 rsvd[62];
	} __packed req = {
		.ver = 1,
		.idx = idx,
		.env = env_cap,
		.env_6g = dev->phy.power_type,
		.acpi_conf = mt792x_acpi_get_flags(&dev->phy),
		.mtcl_conf = mt792x_acpi_get_mtcl_conf(&dev->phy, alpha2),
	};
	struct device_node *np;
	int ret, valid_cnt = 0;
	u32 buf_len = 0;
	u8 *pos;

	if (!clc)
		return 0;

	if (dev->phy.chip_cap & MT792x_CHIP_CAP_CLC_EVT_EN)
		req.cap |= CLC_CAP_EVT_EN;

	np = mt76_find_power_limits_node(&dev->mt76);
	if (np)
		req.cap |= CLC_CAP_DTS_EN;
	of_node_put(np);

	buf_len = le32_to_cpu(clc->len) - sizeof(*clc);
	pos = clc->data;
	while (buf_len > 16) {
		struct mt7921_clc_rule *rule = (struct mt7921_clc_rule *)pos;
		u16 len = le16_to_cpu(rule->len);
		u16 offset = len + sizeof(*rule);

		if (buf_len < offset)
			break;

		pos += offset;
		buf_len -= offset;
		if (rule->alpha2[0] != alpha2[0] ||
		    rule->alpha2[1] != alpha2[1])
			continue;

		memcpy(req.alpha2, rule->alpha2, 2);
		memcpy(req.type, rule->type, 2);

		req.len = cpu_to_le16(sizeof(req) + len);
		skb = __mt76_mcu_msg_alloc(&dev->mt76, &req,
					   le16_to_cpu(req.len),
					   sizeof(req), GFP_KERNEL);
		if (!skb)
			return -ENOMEM;
		skb_put_data(skb, rule->data, len);

		ret = mt76_mcu_skb_send_and_get_msg(&dev->mt76, skb,
						    MCU_CE_CMD(SET_CLC),
						    !!(req.cap & CLC_CAP_EVT_EN),
						    &ret_skb);
		if (ret < 0)
			return ret;

		if (ret_skb) {
			struct mt7921_clc_info_tlv *info;

			if (ret_skb->len < 4 + offsetofend(struct mt7921_clc_info_tlv,
							   chan_conf)) {
				dev_kfree_skb(ret_skb);
				return -EINVAL;
			}

			info = (struct mt7921_clc_info_tlv *)(ret_skb->data + 4);
			dev->phy.clc_chan_conf = info->chan_conf;
			dev_kfree_skb(ret_skb);
		}

		valid_cnt++;
	}

	if (!valid_cnt)
		return -ENOENT;

	return 0;
}

int mt7921_mcu_set_clc(struct mt792x_dev *dev, u8 *alpha2,
		       enum environment_cap env_cap)
{
	struct mt792x_phy *phy = (struct mt792x_phy *)&dev->phy;
	int i, ret;

	/* the legacy firmware has no CLC command */
	if (dev->fw_legacy)
		return 0;

	/* submit all clc config */
	for (i = 0; i < ARRAY_SIZE(phy->clc); i++) {
		if (i == MT792x_CLC_REGD)
			continue;

		ret = __mt7921_mcu_set_clc(dev, alpha2, env_cap,
					   phy->clc[i], i);

		/* If no country found, set "00" as default */
		if (ret == -ENOENT)
			ret = __mt7921_mcu_set_clc(dev, "00",
						   ENVIRON_INDOOR,
						   phy->clc[i], i);
		if (ret < 0)
			return ret;
	}
	return 0;
}

int mt7921_mcu_get_temperature(struct mt792x_phy *phy)
{
	struct mt792x_dev *dev = phy->dev;
	struct {
		u8 ctrl_id;
		u8 action;
		u8 band_idx;
		u8 rsv[5];
	} req = {
		.ctrl_id = THERMAL_SENSOR_TEMP_QUERY,
		.band_idx = phy->mt76->band_idx,
	};

	if (dev->fw_legacy)
		return -EOPNOTSUPP;

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(THERMAL_CTRL), &req,
				 sizeof(req), true);
}

int mt7921_mcu_wf_rf_pin_ctrl(struct mt792x_phy *phy, u8 action)
{
	struct mt792x_dev *dev = phy->dev;
	struct {
		u8 action;
		u8 value;
	} req = {
		.action = action,
		.value = 0,
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(WF_RF_PIN_CTRL), &req,
				 sizeof(req), action ? true : false);
}

#define MT7921_THERMAL_PROT_LOW_TEMP	105
#define MT7921_THERMAL_PROT_HIGH_TEMP	115

int mt7921_mcu_set_thermal_protect(struct mt792x_dev *dev)
{
	char cmd[64];

	snprintf(cmd, sizeof(cmd), "DowngradeTxStreamTemp %d %d",
		 MT7921_THERMAL_PROT_LOW_TEMP, MT7921_THERMAL_PROT_HIGH_TEMP);

	return mt76_connac_mcu_chip_config(&dev->mt76, cmd);
}

int mt7921_mcu_set_rxfilter(struct mt792x_dev *dev, u32 fif,
			    u8 bit_op, u32 bit_map)
{
	struct {
		u8 rsv[4];
		u8 mode;
		u8 rsv2[3];
		__le32 fif;
		__le32 bit_map; /* bit_* for bitmap update */
		u8 bit_op;
		u8 pad[51];
	} __packed data = {
		.mode = fif ? 1 : 2,
		.fif = cpu_to_le32(fif),
		.bit_map = cpu_to_le32(bit_map),
		.bit_op = bit_op,
	};

	/* register bitmap updates are a linux-firmware extension, see
	 * mt7921_mcu_ce_set_rxfilter() for the legacy firmware
	 */
	if (dev->fw_legacy)
		return 0;

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(SET_RX_FILTER),
				 &data, sizeof(data), false);
}

int mt7921_mcu_set_rssimonitor(struct mt792x_dev *dev, struct ieee80211_vif *vif)
{
	struct mt792x_vif *mvif = (struct mt792x_vif *)vif->drv_priv;
	struct {
		u8 enable;
		s8 cqm_rssi_high;
		s8 cqm_rssi_low;
		u8 bss_idx;
		u16 duration;
		u8 rsv2[2];
	} __packed data = {
		.enable = vif->cfg.assoc,
		.cqm_rssi_high = vif->bss_conf.cqm_rssi_thold + vif->bss_conf.cqm_rssi_hyst,
		.cqm_rssi_low = vif->bss_conf.cqm_rssi_thold - vif->bss_conf.cqm_rssi_hyst,
		.bss_idx = mvif->bss_conf.mt76.idx,
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_CE_CMD(RSSI_MONITOR),
				 &data, sizeof(data), false);
}
