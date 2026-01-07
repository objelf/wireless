// SPDX-License-Identifier: GPL-2.0-only
/*
 * NAN mode implementation
 * Copyright(c) 2025 Intel Corporation
 */
#include <net/mac80211.h>

#include "ieee80211_i.h"
#include "driver-ops.h"

static void
ieee80211_nan_init_channel(struct ieee80211_nan_channel *nan_channel,
			   struct cfg80211_nan_channel *cfg_nan_channel)
{
	memset(nan_channel, 0, sizeof(*nan_channel));

	nan_channel->chanreq.oper = cfg_nan_channel->chandef;
	memcpy(nan_channel->channel_entry, cfg_nan_channel->channel_entry,
	       sizeof(nan_channel->channel_entry));
	nan_channel->needed_rx_chains = cfg_nan_channel->rx_nss;
}

static void
ieee80211_nan_update_channel(struct ieee80211_local *local,
			     struct ieee80211_nan_channel *nan_channel,
			     struct cfg80211_nan_channel *cfg_nan_channel)
{
	struct ieee80211_chanctx_conf *conf;

	if (WARN_ON(!cfg80211_chandef_identical(&nan_channel->chanreq.oper,
						&cfg_nan_channel->chandef)))
		return;

	if (WARN_ON(memcmp(nan_channel->channel_entry,
			   cfg_nan_channel->channel_entry,
			   sizeof(nan_channel->channel_entry))))
		return;

	if (nan_channel->needed_rx_chains == cfg_nan_channel->rx_nss)
		return;

	nan_channel->needed_rx_chains = cfg_nan_channel->rx_nss;

	conf = nan_channel->chanctx_conf;
	if (!conf)
		return;

	ieee80211_recalc_smps_chanctx(local, container_of(conf,
							  struct ieee80211_chanctx,
							  conf));
}

static int
ieee80211_nan_use_chanctx(struct ieee80211_sub_if_data *sdata,
			  struct ieee80211_nan_channel *nan_channel,
			  bool assign_on_failure)
{
	struct ieee80211_chanctx *ctx;
	bool reused_ctx;

	if (!nan_channel->chanreq.oper.chan)
		return -EINVAL;

	if (ieee80211_check_combinations(sdata, &nan_channel->chanreq.oper,
					 IEEE80211_CHANCTX_SHARED, 0, -1))
		return -EBUSY;

	ctx = ieee80211_find_or_create_chanctx(sdata, &nan_channel->chanreq,
					       IEEE80211_CHANCTX_SHARED,
					       assign_on_failure,
					       &reused_ctx);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	nan_channel->chanctx_conf = &ctx->conf;

	/*
	 * In case an existing channel context is being used, we marked it as
	 * will_be_used, now that it is assigned - clear this indication
	 */
	if (reused_ctx) {
		WARN_ON(!ctx->will_be_used);
		ctx->will_be_used = false;
	}
	ieee80211_recalc_chanctx_min_def(sdata->local, ctx);
	ieee80211_recalc_smps_chanctx(sdata->local, ctx);

	return 0;
}

static void
ieee80211_nan_remove_channel(struct ieee80211_sub_if_data *sdata,
			     struct ieee80211_nan_channel *nan_channel)
{
	struct ieee80211_chanctx_conf *conf;
	struct ieee80211_chanctx *ctx;

	if (WARN_ON(!nan_channel))
		return;

	lockdep_assert_wiphy(sdata->local->hw.wiphy);

	if (!nan_channel->chanreq.oper.chan)
		return;

	for (int slot = 0; slot < ARRAY_SIZE(sdata->vif.cfg.nan_schedule); slot++)
		if (sdata->vif.cfg.nan_schedule[slot] == nan_channel)
			sdata->vif.cfg.nan_schedule[slot] = NULL;

	conf = nan_channel->chanctx_conf;

	memset(nan_channel, 0, sizeof(*nan_channel));

	/* Update the driver before (possibly) releasing the channel context */
	drv_vif_cfg_changed(sdata->local, sdata, BSS_CHANGED_NAN_LOCAL_SCHED);

	/* Channel might not have a chanctx if it was ULWed */
	if (!conf)
		return;

	ctx = container_of(conf, struct ieee80211_chanctx, conf);

	if (ieee80211_chanctx_num_assigned(sdata->local, ctx) > 0) {
		ieee80211_recalc_chanctx_chantype(sdata->local, ctx);
		ieee80211_recalc_smps_chanctx(sdata->local, ctx);
		ieee80211_recalc_chanctx_min_def(sdata->local, ctx);
	}

	if (ieee80211_chanctx_refcount(sdata->local, ctx) == 0)
		ieee80211_free_chanctx(sdata->local, ctx, false);
}

struct ieee80211_nan_slots_bitmap {
	DECLARE_BITMAP(map, CFG80211_NAN_SCHED_NUM_TIME_SLOTS);
};

static struct ieee80211_nan_slots_bitmap
ieee80211_get_channel_schedule(struct ieee80211_sub_if_data *sdata,
			       struct ieee80211_nan_channel *chan)
{
	struct ieee80211_nan_slots_bitmap schedule = {};

	for (int slot = 0; slot < ARRAY_SIZE(sdata->vif.cfg.nan_schedule); slot++) {
		if (sdata->vif.cfg.nan_schedule[slot] == chan)
			__set_bit(slot, schedule.map);
	}

	return schedule;
}

static int
ieee80211_nan_find_existing_channel(struct ieee80211_nan_channel *channels,
				    const struct cfg80211_chan_def *chandef)
{
	for (int i = 0; i < IEEE80211_NAN_MAX_CHANNELS; i++) {
		if (!channels[i].chanreq.oper.chan)
			break;

		if (cfg80211_chandef_identical(&channels[i].chanreq.oper,
					       chandef))
			return i;
	}

	return -ENOENT;
}

int ieee80211_nan_set_local_sched(struct ieee80211_sub_if_data *sdata,
				  struct cfg80211_nan_local_sched *sched)
{
	struct ieee80211_nan_slots_bitmap backup_schedules[IEEE80211_NAN_MAX_CHANNELS] = {};
	struct ieee80211_nan_channel backup_channels[IEEE80211_NAN_MAX_CHANNELS] = {};
	DECLARE_BITMAP(removed_channels, IEEE80211_NAN_MAX_CHANNELS) = {};
	struct ieee80211_vif_cfg *vif_cfg = &sdata->vif.cfg;
	int ret;

	if (sched->n_channels > IEEE80211_NAN_MAX_CHANNELS)
		return -EOPNOTSUPP;

	/* Backup all existing channels and their schedules */
	for (int i = 0; i < ARRAY_SIZE(vif_cfg->nan_channels); i++) {
		if (!vif_cfg->nan_channels[i].chanreq.oper.chan)
			break;

		backup_channels[i] = vif_cfg->nan_channels[i];
		backup_schedules[i] =
			ieee80211_get_channel_schedule(sdata,
						       &vif_cfg->nan_channels[i]);
	}

	/*
	 * Remove channels that are no longer in the new schedule to free up
	 * resources before adding new channels.
	 */
	for (int i = 0; i < ARRAY_SIZE(vif_cfg->nan_channels); i++) {
		bool still_needed = false;

		if (!vif_cfg->nan_channels[i].chanreq.oper.chan)
			break;

		for (int j = 0; j < sched->n_channels; j++) {
			if (cfg80211_chandef_identical(&vif_cfg->nan_channels[i].chanreq.oper,
						       &sched->nan_channels[j].chandef)) {
				still_needed = true;
				break;
			}
		}

		if (!still_needed) {
			__set_bit(i, removed_channels);
			ieee80211_nan_remove_channel(sdata, &vif_cfg->nan_channels[i]);
		}
	}

	/* Clear the channel array and schedule, we'll rebuild them */
	memset(&vif_cfg->nan_schedule, 0, sizeof(vif_cfg->nan_schedule));
	memset(&vif_cfg->nan_channels, 0, sizeof(vif_cfg->nan_channels));

	for (int i = 0; i < sched->n_channels; i++) {
		struct ieee80211_nan_channel *chan = &vif_cfg->nan_channels[i];
		int existing_idx =
			ieee80211_nan_find_existing_channel(backup_channels,
							    &sched->nan_channels[i].chandef);

		if (existing_idx >= 0) {
			*chan = backup_channels[existing_idx];
			ieee80211_nan_update_channel(sdata->local, chan,
						     &sched->nan_channels[i]);
		} else {
			ieee80211_nan_init_channel(chan,
						   &sched->nan_channels[i]);

			ret = ieee80211_nan_use_chanctx(sdata, chan, false);
			if (ret) {
				memset(chan, 0, sizeof(*chan));
				goto err;
			}
		}

		for (int s = 0; s < ARRAY_SIZE(sched->schedule); s++)
			if (sched->schedule[s] == i)
				vif_cfg->nan_schedule[s] = chan;
	}

	drv_vif_cfg_changed(sdata->local, sdata, BSS_CHANGED_NAN_LOCAL_SCHED);

	return 0;
err:
	/* Remove newly added channels */
	for (int i = 0; i < ARRAY_SIZE(vif_cfg->nan_channels); i++) {
		struct cfg80211_chan_def *chan_def = &vif_cfg->nan_channels[i].chanreq.oper;

		if (!chan_def->chan)
			break;

		if (ieee80211_nan_find_existing_channel(backup_channels,
							chan_def) < 0)
			ieee80211_nan_remove_channel(sdata,
						     &vif_cfg->nan_channels[i]);
	}

	memset(&vif_cfg->nan_schedule, 0, sizeof(vif_cfg->nan_schedule));
	memset(&vif_cfg->nan_channels, 0, sizeof(vif_cfg->nan_channels));

	/* Re-add all backed up channels */
	for (int i = 0; i < ARRAY_SIZE(backup_channels); i++) {
		struct ieee80211_nan_channel *chan = &vif_cfg->nan_channels[i];
		int slot;

		if (!backup_channels[i].chanreq.oper.chan)
			break;

		*chan = backup_channels[i];

		if (!chan->chanctx_conf)
			continue;

		if (test_bit(i, removed_channels)) {
			/* Clear the stale chanctx pointer */
			chan->chanctx_conf = NULL;
			/*
			 * We removed the newly added channels so we don't lack
			 * resources. So the only reason that this would fail
			 * is a FW error which we ignore. Therefore, this
			 * should never fail.
			 */
			WARN_ON(ieee80211_nan_use_chanctx(sdata, chan, true));
		} else {
			struct ieee80211_chanctx_conf *conf = chan->chanctx_conf;

			/* FIXME: detect no-op? */
			/* Channel was not removed but may have been updated */
			ieee80211_recalc_smps_chanctx(sdata->local,
						     container_of(conf,
								  struct ieee80211_chanctx,
								  conf));
		}

		for_each_set_bit(slot, backup_schedules[i].map,
				 CFG80211_NAN_SCHED_NUM_TIME_SLOTS)
			vif_cfg->nan_schedule[slot] = chan;
	}

	drv_vif_cfg_changed(sdata->local, sdata, BSS_CHANGED_NAN_LOCAL_SCHED);
	return ret;
}
