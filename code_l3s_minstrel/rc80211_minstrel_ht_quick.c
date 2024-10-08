// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2010-2013 Felix Fietkau <nbd@openwrt.org>
 */
#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/debugfs.h>
#include <linux/random.h>
#include <linux/moduleparam.h>
#include <linux/ieee80211.h>
#include <net/mac80211.h>
#include "rate.h"
#include "sta_info.h"
#include "rc80211_minstrel.h"
#include "rc80211_minstrel_ht.h"

#define AVG_AMPDU_SIZE	16
#define AVG_PKT_SIZE	1200

/* Number of bits for an average sized packet */
#define MCS_NBITS ((AVG_PKT_SIZE * AVG_AMPDU_SIZE) << 3)

/* Number of symbols for a packet with (bps) bits per symbol */
#define MCS_NSYMS(bps) DIV_ROUND_UP(MCS_NBITS, (bps))

/* Transmission time (nanoseconds) for a packet containing (syms) symbols */
#define MCS_SYMBOL_TIME(sgi, syms)					\
	(sgi ?								\
	  ((syms) * 18000 + 4000) / 5 :	/* syms * 3.6 us */		\
	  ((syms) * 1000) << 2		/* syms * 4 us */		\
	)

/* Transmit duration for the raw data part of an average sized packet */
#define MCS_DURATION(streams, sgi, bps) \
	(MCS_SYMBOL_TIME(sgi, MCS_NSYMS((streams) * (bps))) / AVG_AMPDU_SIZE)

#define BW_20			0
#define BW_40			1
#define BW_80			2

/*
 * Define group sort order: HT40 -> SGI -> #streams
 */
#define GROUP_IDX(_streams, _sgi, _ht40)	\
	MINSTREL_HT_GROUP_0 +			\
	MINSTREL_MAX_STREAMS * 2 * _ht40 +	\
	MINSTREL_MAX_STREAMS * _sgi +	\
	_streams - 1

#define _MAX(a, b) (((a)>(b))?(a):(b))

#define GROUP_SHIFT(duration)						\
	_MAX(0, 16 - __builtin_clz(duration))

/* MCS rate information for an MCS group */
#define __MCS_GROUP(_streams, _sgi, _ht40, _s)				\
	[GROUP_IDX(_streams, _sgi, _ht40)] = {				\
	.streams = _streams,						\
	.shift = _s,							\
	.flags =							\
		IEEE80211_TX_RC_MCS |					\
		(_sgi ? IEEE80211_TX_RC_SHORT_GI : 0) |			\
		(_ht40 ? IEEE80211_TX_RC_40_MHZ_WIDTH : 0),		\
	.duration = {							\
		MCS_DURATION(_streams, _sgi, _ht40 ? 54 : 26) >> _s,	\
		MCS_DURATION(_streams, _sgi, _ht40 ? 108 : 52) >> _s,	\
		MCS_DURATION(_streams, _sgi, _ht40 ? 162 : 78) >> _s,	\
		MCS_DURATION(_streams, _sgi, _ht40 ? 216 : 104) >> _s,	\
		MCS_DURATION(_streams, _sgi, _ht40 ? 324 : 156) >> _s,	\
		MCS_DURATION(_streams, _sgi, _ht40 ? 432 : 208) >> _s,	\
		MCS_DURATION(_streams, _sgi, _ht40 ? 486 : 234) >> _s,	\
		MCS_DURATION(_streams, _sgi, _ht40 ? 540 : 260) >> _s	\
	}								\
}

#define MCS_GROUP_SHIFT(_streams, _sgi, _ht40)				\
	GROUP_SHIFT(MCS_DURATION(_streams, _sgi, _ht40 ? 54 : 26))

#define MCS_GROUP(_streams, _sgi, _ht40)				\
	__MCS_GROUP(_streams, _sgi, _ht40,				\
		    MCS_GROUP_SHIFT(_streams, _sgi, _ht40))

#define VHT_GROUP_IDX(_streams, _sgi, _bw)				\
	(MINSTREL_VHT_GROUP_0 +						\
	 MINSTREL_MAX_STREAMS * 2 * (_bw) +				\
	 MINSTREL_MAX_STREAMS * (_sgi) +				\
	 (_streams) - 1)

#define BW2VBPS(_bw, r3, r2, r1)					\
	(_bw == BW_80 ? r3 : _bw == BW_40 ? r2 : r1)

#define __VHT_GROUP(_streams, _sgi, _bw, _s)				\
	[VHT_GROUP_IDX(_streams, _sgi, _bw)] = {			\
	.streams = _streams,						\
	.shift = _s,							\
	.flags =							\
		IEEE80211_TX_RC_VHT_MCS |				\
		(_sgi ? IEEE80211_TX_RC_SHORT_GI : 0) |			\
		(_bw == BW_80 ? IEEE80211_TX_RC_80_MHZ_WIDTH :		\
		 _bw == BW_40 ? IEEE80211_TX_RC_40_MHZ_WIDTH : 0),	\
	.duration = {							\
		MCS_DURATION(_streams, _sgi,				\
			     BW2VBPS(_bw,  117,  54,  26)) >> _s,	\
		MCS_DURATION(_streams, _sgi,				\
			     BW2VBPS(_bw,  234, 108,  52)) >> _s,	\
		MCS_DURATION(_streams, _sgi,				\
			     BW2VBPS(_bw,  351, 162,  78)) >> _s,	\
		MCS_DURATION(_streams, _sgi,				\
			     BW2VBPS(_bw,  468, 216, 104)) >> _s,	\
		MCS_DURATION(_streams, _sgi,				\
			     BW2VBPS(_bw,  702, 324, 156)) >> _s,	\
		MCS_DURATION(_streams, _sgi,				\
			     BW2VBPS(_bw,  936, 432, 208)) >> _s,	\
		MCS_DURATION(_streams, _sgi,				\
			     BW2VBPS(_bw, 1053, 486, 234)) >> _s,	\
		MCS_DURATION(_streams, _sgi,				\
			     BW2VBPS(_bw, 1170, 540, 260)) >> _s,	\
		MCS_DURATION(_streams, _sgi,				\
			     BW2VBPS(_bw, 1404, 648, 312)) >> _s,	\
		MCS_DURATION(_streams, _sgi,				\
			     BW2VBPS(_bw, 1560, 720, 346)) >> _s	\
	}								\
}

#define VHT_GROUP_SHIFT(_streams, _sgi, _bw)				\
	GROUP_SHIFT(MCS_DURATION(_streams, _sgi,			\
				 BW2VBPS(_bw,  117,  54,  26)))

#define VHT_GROUP(_streams, _sgi, _bw)					\
	__VHT_GROUP(_streams, _sgi, _bw,				\
		    VHT_GROUP_SHIFT(_streams, _sgi, _bw))

#define CCK_DURATION(_bitrate, _short, _len)		\
	(1000 * (10 /* SIFS */ +			\
	 (_short ? 72 + 24 : 144 + 48) +		\
	 (8 * (_len + 4) * 10) / (_bitrate)))

#define CCK_ACK_DURATION(_bitrate, _short)			\
	(CCK_DURATION((_bitrate > 10 ? 20 : 10), false, 60) +	\
	 CCK_DURATION(_bitrate, _short, AVG_PKT_SIZE))

#define CCK_DURATION_LIST(_short, _s)			\
	CCK_ACK_DURATION(10, _short) >> _s,		\
	CCK_ACK_DURATION(20, _short) >> _s,		\
	CCK_ACK_DURATION(55, _short) >> _s,		\
	CCK_ACK_DURATION(110, _short) >> _s

#define __CCK_GROUP(_s)					\
	[MINSTREL_CCK_GROUP] = {			\
		.streams = 1,				\
		.flags = 0,				\
		.shift = _s,				\
		.duration = {				\
			CCK_DURATION_LIST(false, _s),	\
			CCK_DURATION_LIST(true, _s)	\
		}					\
	}

#define CCK_GROUP_SHIFT					\
	GROUP_SHIFT(CCK_ACK_DURATION(10, false))

#define CCK_GROUP __CCK_GROUP(CCK_GROUP_SHIFT)


static bool minstrel_vht_only = true;
module_param(minstrel_vht_only, bool, 0644);
MODULE_PARM_DESC(minstrel_vht_only,
		 "Use only VHT rates when VHT is supported by sta.");

/*
 * To enable sufficiently targeted rate sampling, MCS rates are divided into
 * groups, based on the number of streams and flags (HT40, SGI) that they
 * use.
 *
 * Sortorder has to be fixed for GROUP_IDX macro to be applicable:
 * BW -> SGI -> #streams
 */
const struct mcs_group minstrel_mcs_groups[] = {
	MCS_GROUP(1, 0, BW_20),
	MCS_GROUP(2, 0, BW_20),
	MCS_GROUP(3, 0, BW_20),
	MCS_GROUP(4, 0, BW_20),

	MCS_GROUP(1, 1, BW_20),
	MCS_GROUP(2, 1, BW_20),
	MCS_GROUP(3, 1, BW_20),
	MCS_GROUP(4, 1, BW_20),

	MCS_GROUP(1, 0, BW_40),
	MCS_GROUP(2, 0, BW_40),
	MCS_GROUP(3, 0, BW_40),
	MCS_GROUP(4, 0, BW_40),

	MCS_GROUP(1, 1, BW_40),
	MCS_GROUP(2, 1, BW_40),
	MCS_GROUP(3, 1, BW_40),
	MCS_GROUP(4, 1, BW_40),

	CCK_GROUP,

	VHT_GROUP(1, 0, BW_20),
	VHT_GROUP(2, 0, BW_20),
	VHT_GROUP(3, 0, BW_20),
	VHT_GROUP(4, 0, BW_20),

	VHT_GROUP(1, 1, BW_20),
	VHT_GROUP(2, 1, BW_20),
	VHT_GROUP(3, 1, BW_20),
	VHT_GROUP(4, 1, BW_20),

	VHT_GROUP(1, 0, BW_40),
	VHT_GROUP(2, 0, BW_40),
	VHT_GROUP(3, 0, BW_40),
	VHT_GROUP(4, 0, BW_40),

	VHT_GROUP(1, 1, BW_40),
	VHT_GROUP(2, 1, BW_40),
	VHT_GROUP(3, 1, BW_40),
	VHT_GROUP(4, 1, BW_40),

	VHT_GROUP(1, 0, BW_80),
	VHT_GROUP(2, 0, BW_80),
	VHT_GROUP(3, 0, BW_80),
	VHT_GROUP(4, 0, BW_80),

	VHT_GROUP(1, 1, BW_80),
	VHT_GROUP(2, 1, BW_80),
	VHT_GROUP(3, 1, BW_80),
	VHT_GROUP(4, 1, BW_80),
};

static u8 sample_table[SAMPLE_COLUMNS][MCS_GROUP_RATES] __read_mostly;

static void
minstrel_ht_update_rates(struct minstrel_priv *mp, struct minstrel_ht_sta *mi);

/*
 * Some VHT MCSes are invalid (when Ndbps / Nes is not an integer)
 * e.g for MCS9@20MHzx1Nss: Ndbps=8x52*(5/6) Nes=1
 *
 * Returns the valid mcs map for struct minstrel_mcs_group_data.supported
 */
static u16
minstrel_get_valid_vht_rates(int bw, int nss, __le16 mcs_map)
{
	u16 mask = 0;

	if (bw == BW_20) {
		if (nss != 3 && nss != 6)
			mask = BIT(9);
	} else if (bw == BW_80) {
		if (nss == 3 || nss == 7)
			mask = BIT(6);
		else if (nss == 6)
			mask = BIT(9);
	} else {
		WARN_ON(bw != BW_40);
	}

	switch ((le16_to_cpu(mcs_map) >> (2 * (nss - 1))) & 3) {
	case IEEE80211_VHT_MCS_SUPPORT_0_7:
		mask |= 0x300;
		break;
	case IEEE80211_VHT_MCS_SUPPORT_0_8:
		mask |= 0x200;
		break;
	case IEEE80211_VHT_MCS_SUPPORT_0_9:
		break;
	default:
		mask = 0x3ff;
	}

	return 0x3ff & ~mask;
}

/*
 * Look up an MCS group index based on mac80211 rate information
 */
static int
minstrel_ht_get_group_idx(struct ieee80211_tx_rate *rate)
{
	return GROUP_IDX((rate->idx / 8) + 1,
			 !!(rate->flags & IEEE80211_TX_RC_SHORT_GI),
			 !!(rate->flags & IEEE80211_TX_RC_40_MHZ_WIDTH));
}

static int
minstrel_vht_get_group_idx(struct ieee80211_tx_rate *rate)
{
	return VHT_GROUP_IDX(ieee80211_rate_get_vht_nss(rate),
			     !!(rate->flags & IEEE80211_TX_RC_SHORT_GI),
			     !!(rate->flags & IEEE80211_TX_RC_40_MHZ_WIDTH) +
			     2*!!(rate->flags & IEEE80211_TX_RC_80_MHZ_WIDTH));
}

static struct minstrel_rate_stats *
minstrel_ht_get_stats(struct minstrel_priv *mp, struct minstrel_ht_sta *mi,
		      struct ieee80211_tx_rate *rate)
{
	int group, idx;

	if (rate->flags & IEEE80211_TX_RC_MCS) {
		group = minstrel_ht_get_group_idx(rate);
		idx = rate->idx % 8;
	} else if (rate->flags & IEEE80211_TX_RC_VHT_MCS) {
		group = minstrel_vht_get_group_idx(rate);
		idx = ieee80211_rate_get_vht_mcs(rate);
	} else {
		group = MINSTREL_CCK_GROUP;

		for (idx = 0; idx < ARRAY_SIZE(mp->cck_rates); idx++)
			if (rate->idx == mp->cck_rates[idx])
				break;

		/* short preamble */
		if ((mi->supported[group] & BIT(idx + 4)) &&
		    (rate->flags & IEEE80211_TX_RC_USE_SHORT_PREAMBLE))
			idx += 4;
	}
	return &mi->groups[group].rates[idx];
}

static inline struct minstrel_rate_stats *
minstrel_get_ratestats(struct minstrel_ht_sta *mi, int index)
{
	return &mi->groups[index / MCS_GROUP_RATES].rates[index % MCS_GROUP_RATES];
}

static unsigned int
minstrel_ht_avg_ampdu_len(struct minstrel_ht_sta *mi)
{
	if (!mi->avg_ampdu_len)
		return AVG_AMPDU_SIZE;

	return MINSTREL_TRUNC(mi->avg_ampdu_len);
}

/*
 * Return current throughput based on the average A-MPDU length, taking into
 * account the expected number of retransmissions and their expected length
 */
int
minstrel_ht_get_tp_avg(struct minstrel_ht_sta *mi, int group, int rate,
		       int prob_ewma)
{
	unsigned int nsecs = 0;

	/* do not account throughput if sucess prob is below 10% */
	if (prob_ewma < MINSTREL_FRAC(10, 100))
		return 0;

	if (group != MINSTREL_CCK_GROUP)
		nsecs = 1000 * mi->overhead / minstrel_ht_avg_ampdu_len(mi);

	nsecs += minstrel_mcs_groups[group].duration[rate] <<
		 minstrel_mcs_groups[group].shift;

	/*
	 * For the throughput calculation, limit the probability value to 90% to
	 * account for collision related packet error rate fluctuation
	 * (prob is scaled - see MINSTREL_FRAC above)
	 */
	if (prob_ewma > MINSTREL_FRAC(90, 100))
		return MINSTREL_TRUNC(100000 * ((MINSTREL_FRAC(90, 100) * 1000)
								      / nsecs));
	else
		return MINSTREL_TRUNC(100000 * ((prob_ewma * 1000) / nsecs));
}

/*
 * Find & sort topmost throughput rates
 *
 * If multiple rates provide equal throughput the sorting is based on their
 * current success probability. Higher success probability is preferred among
 * MCS groups, CCK rates do not provide aggregation and are therefore at last.
 */
static void
minstrel_ht_sort_best_tp_rates(struct minstrel_ht_sta *mi, u16 index,
			       u16 *tp_list)
{
	int cur_group, cur_idx, cur_tp_avg, cur_prob;
	int tmp_group, tmp_idx, tmp_tp_avg, tmp_prob;
	int j = MAX_THR_RATES;

	cur_group = index / MCS_GROUP_RATES;
	cur_idx = index  % MCS_GROUP_RATES;
	cur_prob = mi->groups[cur_group].rates[cur_idx].prob_ewma;
	cur_tp_avg = minstrel_ht_get_tp_avg(mi, cur_group, cur_idx, cur_prob);

	do {
		tmp_group = tp_list[j - 1] / MCS_GROUP_RATES;
		tmp_idx = tp_list[j - 1] % MCS_GROUP_RATES;
		tmp_prob = mi->groups[tmp_group].rates[tmp_idx].prob_ewma;
		tmp_tp_avg = minstrel_ht_get_tp_avg(mi, tmp_group, tmp_idx,
						    tmp_prob);
		if (cur_tp_avg < tmp_tp_avg ||
		    (cur_tp_avg == tmp_tp_avg && cur_prob <= tmp_prob))
			break;
		j--;
	} while (j > 0);

	if (j < MAX_THR_RATES - 1) {
		memmove(&tp_list[j + 1], &tp_list[j], (sizeof(*tp_list) *
		       (MAX_THR_RATES - (j + 1))));
	}
	if (j < MAX_THR_RATES)
		tp_list[j] = index;
}

/*
 * Find and set the topmost probability rate per sta and per group
 */
static void
minstrel_ht_set_best_prob_rate(struct minstrel_ht_sta *mi, u16 index)
{
	struct minstrel_mcs_group_data *mg;
	struct minstrel_rate_stats *mrs;
	int tmp_group, tmp_idx, tmp_tp_avg, tmp_prob;
	int max_tp_group, cur_tp_avg, cur_group, cur_idx;
	int max_gpr_group, max_gpr_idx;
	int max_gpr_tp_avg, max_gpr_prob;

	cur_group = index / MCS_GROUP_RATES;
	cur_idx = index % MCS_GROUP_RATES;
	mg = &mi->groups[index / MCS_GROUP_RATES];
	mrs = &mg->rates[index % MCS_GROUP_RATES];

	tmp_group = mi->max_prob_rate / MCS_GROUP_RATES;
	tmp_idx = mi->max_prob_rate % MCS_GROUP_RATES;
	tmp_prob = mi->groups[tmp_group].rates[tmp_idx].prob_ewma;
	tmp_tp_avg = minstrel_ht_get_tp_avg(mi, tmp_group, tmp_idx, tmp_prob);

	/* if max_tp_rate[0] is from MCS_GROUP max_prob_rate get selected from
	 * MCS_GROUP as well as CCK_GROUP rates do not allow aggregation */
	max_tp_group = mi->max_tp_rate[0] / MCS_GROUP_RATES;
	if((index / MCS_GROUP_RATES == MINSTREL_CCK_GROUP) &&
	    (max_tp_group != MINSTREL_CCK_GROUP))
		return;

	max_gpr_group = mg->max_group_prob_rate / MCS_GROUP_RATES;
	max_gpr_idx = mg->max_group_prob_rate % MCS_GROUP_RATES;
	max_gpr_prob = mi->groups[max_gpr_group].rates[max_gpr_idx].prob_ewma;

	if (mrs->prob_ewma > MINSTREL_FRAC(75, 100)) {
		cur_tp_avg = minstrel_ht_get_tp_avg(mi, cur_group, cur_idx,
						    mrs->prob_ewma);
		if (cur_tp_avg > tmp_tp_avg)
			mi->max_prob_rate = index;

		max_gpr_tp_avg = minstrel_ht_get_tp_avg(mi, max_gpr_group,
							max_gpr_idx,
							max_gpr_prob);
		if (cur_tp_avg > max_gpr_tp_avg)
			mg->max_group_prob_rate = index;
	} else {
		if (mrs->prob_ewma > tmp_prob)
			mi->max_prob_rate = index;
		if (mrs->prob_ewma > max_gpr_prob)
			mg->max_group_prob_rate = index;
	}
}


/*
 * Assign new rate set per sta and use CCK rates only if the fastest
 * rate (max_tp_rate[0]) is from CCK group. This prohibits such sorted
 * rate sets where MCS and CCK rates are mixed, because CCK rates can
 * not use aggregation.
 */
static void
minstrel_ht_assign_best_tp_rates(struct minstrel_ht_sta *mi,
				 u16 tmp_mcs_tp_rate[MAX_THR_RATES],
				 u16 tmp_cck_tp_rate[MAX_THR_RATES])
{
	unsigned int tmp_group, tmp_idx, tmp_cck_tp, tmp_mcs_tp, tmp_prob;
	int i;

	tmp_group = tmp_cck_tp_rate[0] / MCS_GROUP_RATES;
	tmp_idx = tmp_cck_tp_rate[0] % MCS_GROUP_RATES;
	tmp_prob = mi->groups[tmp_group].rates[tmp_idx].prob_ewma;
	tmp_cck_tp = minstrel_ht_get_tp_avg(mi, tmp_group, tmp_idx, tmp_prob);

	tmp_group = tmp_mcs_tp_rate[0] / MCS_GROUP_RATES;
	tmp_idx = tmp_mcs_tp_rate[0] % MCS_GROUP_RATES;
	tmp_prob = mi->groups[tmp_group].rates[tmp_idx].prob_ewma;
	tmp_mcs_tp = minstrel_ht_get_tp_avg(mi, tmp_group, tmp_idx, tmp_prob);

	if (tmp_cck_tp > tmp_mcs_tp) {
		for(i = 0; i < MAX_THR_RATES; i++) {
			minstrel_ht_sort_best_tp_rates(mi, tmp_cck_tp_rate[i],
						       tmp_mcs_tp_rate);
		}
	}

}

/*
 * Try to increase robustness of max_prob rate by decrease number of
 * streams if possible.
 */
static inline void
minstrel_ht_prob_rate_reduce_streams(struct minstrel_ht_sta *mi)
{
	struct minstrel_mcs_group_data *mg;
	int tmp_max_streams, group, tmp_idx, tmp_prob;
	int tmp_tp = 0;

	tmp_max_streams = minstrel_mcs_groups[mi->max_tp_rate[0] /
			  MCS_GROUP_RATES].streams;
	for (group = 0; group < ARRAY_SIZE(minstrel_mcs_groups); group++) {
		mg = &mi->groups[group];
		if (!mi->supported[group] || group == MINSTREL_CCK_GROUP)
			continue;

		tmp_idx = mg->max_group_prob_rate % MCS_GROUP_RATES;
		tmp_prob = mi->groups[group].rates[tmp_idx].prob_ewma;

		if (tmp_tp < minstrel_ht_get_tp_avg(mi, group, tmp_idx, tmp_prob) &&
		   (minstrel_mcs_groups[group].streams < tmp_max_streams)) {
				mi->max_prob_rate = mg->max_group_prob_rate;
				tmp_tp = minstrel_ht_get_tp_avg(mi, group,
								tmp_idx,
								tmp_prob);
		}
	}
}

/*
 * Update rate statistics and select new primary rates
 *
 * Rules for rate selection:
 *  - max_prob_rate must use only one stream, as a tradeoff between delivery
 *    probability and throughput during strong fluctuations
 *  - as long as the max prob rate has a probability of more than 75%, pick
 *    higher throughput rates, even if the probablity is a bit lower
 */
static void
minstrel_ht_update_stats(struct minstrel_priv *mp, struct minstrel_ht_sta *mi)
{
	struct minstrel_mcs_group_data *mg;
	struct minstrel_rate_stats *mrs;
	int group, i, j, cur_prob;
	u16 tmp_mcs_tp_rate[MAX_THR_RATES], tmp_group_tp_rate[MAX_THR_RATES];
	u16 tmp_cck_tp_rate[MAX_THR_RATES], index;

	if (mi->ampdu_packets > 0) {
		if (!ieee80211_hw_check(mp->hw, TX_STATUS_NO_AMPDU_LEN))
			mi->avg_ampdu_len = minstrel_ewma(mi->avg_ampdu_len,
				MINSTREL_FRAC(mi->ampdu_len, mi->ampdu_packets),
					      EWMA_LEVEL);
		else
			mi->avg_ampdu_len = 0;
		mi->ampdu_len = 0;
		mi->ampdu_packets = 0;
	}

	mi->sample_slow = 0;
	mi->sample_count = 0;

	/* Initialize global rate indexes */
	for(j = 0; j < MAX_THR_RATES; j++){
		tmp_mcs_tp_rate[j] = 0;
		tmp_cck_tp_rate[j] = 0;
	}

	/* Find best rate sets within all MCS groups*/
	for (group = 0; group < ARRAY_SIZE(minstrel_mcs_groups); group++) {

		mg = &mi->groups[group];
		if (!mi->supported[group])
			continue;

		mi->sample_count++;

		/* (re)Initialize group rate indexes */
		for(j = 0; j < MAX_THR_RATES; j++)
			tmp_group_tp_rate[j] = group;

		for (i = 0; i < MCS_GROUP_RATES; i++) {
			if (!(mi->supported[group] & BIT(i)))
				continue;

			index = MCS_GROUP_RATES * group + i;

			mrs = &mg->rates[i];
			mrs->retry_updated = false;
			minstrel_calc_rate_stats(mrs);
			cur_prob = mrs->prob_ewma;

			if (minstrel_ht_get_tp_avg(mi, group, i, cur_prob) == 0)
				continue;

			/* Find max throughput rate set */
			if (group != MINSTREL_CCK_GROUP) {
				minstrel_ht_sort_best_tp_rates(mi, index,
							       tmp_mcs_tp_rate);
			} else if (group == MINSTREL_CCK_GROUP) {
				minstrel_ht_sort_best_tp_rates(mi, index,
							       tmp_cck_tp_rate);
			}

			/* Find max throughput rate set within a group */
			minstrel_ht_sort_best_tp_rates(mi, index,
						       tmp_group_tp_rate);

			/* Find max probability rate per group and global */
			minstrel_ht_set_best_prob_rate(mi, index);
		}

		memcpy(mg->max_group_tp_rate, tmp_group_tp_rate,
		       sizeof(mg->max_group_tp_rate));
	}

	/* Assign new rate set per sta */
	minstrel_ht_assign_best_tp_rates(mi, tmp_mcs_tp_rate, tmp_cck_tp_rate);
	memcpy(mi->max_tp_rate, tmp_mcs_tp_rate, sizeof(mi->max_tp_rate));

	/* Try to increase robustness of max_prob_rate*/
	minstrel_ht_prob_rate_reduce_streams(mi);

	/* try to sample all available rates during each interval */
	mi->sample_count *= 8;

#ifdef CONFIG_MAC80211_DEBUGFS
	/* use fixed index if set */
	if (mp->fixed_rate_idx != -1) {
		for (i = 0; i < 4; i++)
			mi->max_tp_rate[i] = mp->fixed_rate_idx;
		mi->max_prob_rate = mp->fixed_rate_idx;
	}
#endif

	/* Reset update timer */
	mi->last_stats_update = jiffies;
}

static bool
minstrel_ht_txstat_valid(struct minstrel_priv *mp, struct ieee80211_tx_rate *rate)
{
	if (rate->idx < 0)
		return false;

	if (!rate->count)
		return false;

	if (rate->flags & IEEE80211_TX_RC_MCS ||
	    rate->flags & IEEE80211_TX_RC_VHT_MCS)
		return true;

	return rate->idx == mp->cck_rates[0] ||
	       rate->idx == mp->cck_rates[1] ||
	       rate->idx == mp->cck_rates[2] ||
	       rate->idx == mp->cck_rates[3];
}

static void
minstrel_set_next_sample_idx(struct minstrel_ht_sta *mi)
{
	struct minstrel_mcs_group_data *mg;

	for (;;) {
		mi->sample_group++;
		mi->sample_group %= ARRAY_SIZE(minstrel_mcs_groups);
		mg = &mi->groups[mi->sample_group];

		if (!mi->supported[mi->sample_group])
			continue;

		if (++mg->index >= MCS_GROUP_RATES) {
			mg->index = 0;
			if (++mg->column >= ARRAY_SIZE(sample_table))
				mg->column = 0;
		}
		break;
	}
}

// static void
// minstrel_downgrade_rate(struct minstrel_ht_sta *mi, u16 *idx, bool primary)
// {
// 	int group, orig_group;

// 	orig_group = group = *idx / MCS_GROUP_RATES;
// 	while (group > 0) {
// 		group--;

// 		if (!mi->supported[group])
// 			continue;

// 		if (minstrel_mcs_groups[group].streams >
// 		    minstrel_mcs_groups[orig_group].streams)
// 			continue;

// 		if (primary)
// 			*idx = mi->groups[group].max_group_tp_rate[0];
// 		else
// 			*idx = mi->groups[group].max_group_tp_rate[1];
// 		break;
// 	}
// }

static void
minstrel_aggr_check(struct ieee80211_sta *pubsta, struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *) skb->data;
	struct sta_info *sta = container_of(pubsta, struct sta_info, sta);
	u16 tid;

	if (skb_get_queue_mapping(skb) == IEEE80211_AC_VO)
		return;

	if (unlikely(!ieee80211_is_data_qos(hdr->frame_control)))
		return;

	if (unlikely(skb->protocol == cpu_to_be16(ETH_P_PAE)))
		return;

	tid = ieee80211_get_tid(hdr);
	if (likely(sta->ampdu_mlme.tid_tx[tid]))
		return;

	ieee80211_start_tx_ba_session(pubsta, tid, 0);
}

//function to go the left mcs group when is the second series of probing
static void go_to_the_left_group(int idx, struct minstrel_ht_sta *mi) {
	mi->rates[0] = idx;
	//if [0,7) position of the group change group making -10 and -9 to decrease the number of streams
	//beacuse there are 10 MCS rates in a group
	if(idx%MCS_GROUP_RATES>=0 && idx%MCS_GROUP_RATES<7) {
		mi->rates[1] = idx-9;
		mi->rates[2] = idx-10;
	}
	//if position=7 of the group we dont want 8 and 9 positions 
	//by making -10 and -7 the immediately next MCS index from -10 position
	else if(idx%MCS_GROUP_RATES==7) {
		mi->rates[1] = idx-7;
		mi->rates[2] = idx-10;
	}
}

//function to go the right mcs group when is the second series of probing
static void go_to_the_right_group(int idx, struct minstrel_ht_sta *mi) {
	//if (0,7] position of the group change group making +10 and +9 to increase the number of streams
	//beacuse there are 10 MCS rates in a group
	if(idx%MCS_GROUP_RATES>0 && idx%MCS_GROUP_RATES<=7) {
		mi->rates[0] = idx+10;
		mi->rates[1] = idx+9;
	}
	//if position=0 of the group we dont want 8 and 9 positions 
	//by making +10 and +7 the immediately previous MCS index from +10 position 
	else if(idx%MCS_GROUP_RATES==0) {
		mi->rates[0] = idx+10;
		mi->rates[1] = idx+7;
	}
	mi->rates[2] = idx;
}

//function that is called when the state is in transmission state to make the rates three consecutives rates in decreasing order
static void tx_state(struct minstrel_ht_sta *mi, int i) {
	if(i==0) { //if the i is in position=0 keeps the same r1,r2,r3, if the packet is transmitted successfully with r1
		mi->rates[0] = mi->max_tp_rate[0];
		mi->rates[1] = mi->max_tp_rate[1];
		mi->rates[2] = mi->max_tp_rate[2];
	}
	else if(i==1) { //if the index is in position=1 assign r1 = r2, r2 = r3 and r3 = r3 -1 or -3 or the same
		mi->rates[0] = mi->max_tp_rate[1];
		mi->rates[1] = mi->max_tp_rate[2];
		if (mi->max_tp_rate[2]%MCS_GROUP_RATES!=0) {
			mi->rates[2] = mi->max_tp_rate[2]-1;
		}
		else if (mi->max_tp_rate[2]%MCS_GROUP_RATES==0&&mi->max_tp_rate[2]!=0) {
			mi->rates[2] = mi->max_tp_rate[2]-3;
		}
		else {
			mi->rates[2] = mi->max_tp_rate[2];
		}
	}
	else if(i==2) { //if the index is in position=2 assign r1 = r3, r2 = r3 -1 or -3 or the same and r3 = r3 -2 or -4 or the same
		mi->rates[0] = mi->max_tp_rate[2];
		if (mi->max_tp_rate[2]%MCS_GROUP_RATES!=0) { //greater than zero position in groups
			mi->rates[1] = mi->max_tp_rate[2]-1;
			if (mi->max_tp_rate[2]-1%MCS_GROUP_RATES!=0) {
				mi->rates[2] = mi->max_tp_rate[2]-2;
			}
			else if (mi->max_tp_rate[2]-1%MCS_GROUP_RATES==0&&mi->max_tp_rate[2]-1!=0) { 
				mi->rates[2] = mi->max_tp_rate[2]-4;
			}
			else {
				mi->rates[2] = mi->max_tp_rate[2];
			}
		}
		else if (mi->max_tp_rate[2]%MCS_GROUP_RATES==0 && mi->max_tp_rate[2]!=0) { //group 1 or 2 in zero position so must fall to the group 0
			mi->rates[1] = mi->max_tp_rate[2]-3;
			mi->rates[2] = mi->max_tp_rate[2]-4;
		}
		else {
			mi->rates[1] = mi->max_tp_rate[2];
			mi->rates[2] = mi->max_tp_rate[2];
		}
	}
}

//function that is called when the state is in probe state with two probe series(first and second probe series)
static void probe_state(int idx, struct minstrel_ht_sta *mi) {
	if(!mi->first_probe) { //first probe series
		//if (0,7) position keep in the same group with +-1 in r1 and r3
		if(idx%MCS_GROUP_RATES>0 && idx%MCS_GROUP_RATES<7) {
			mi->rates[0] = idx+1;
			mi->rates[1] = idx;
			mi->rates[2] = idx-1;
		}
		//if position=7 decrease -1 and -2 for r2 and r3 respectively
		else if(idx%MCS_GROUP_RATES==7) {
			mi->rates[0] = idx;
			mi->rates[1] = idx-1;
			mi->rates[2] = idx-2;
		}
		//if position=0 increase +2 and +1 for r1 and r2 respectively
		else if(idx%MCS_GROUP_RATES==0) {
			mi->rates[0] = idx+2;
			mi->rates[1] = idx+1;
			mi->rates[2] = idx;
		}
		mi->first_probe = true;
		// if (mi->probe_interval != (10 * HZ) / 1000) { //for slow probing
		// 	mi->probe_interval = (10 * HZ) / 1000;
		// 	mi->time_of_probing = jiffies;
		// }
		mi->probe_interval = (10 * HZ) / 1000; //for quick probing
		printk(KERN_INFO "Probe interval from probe_state = %ld\n", mi->probe_interval);
	}
	else { //second probe series
		mi->first_probe = false;
		mi->probe_interval = (60 * HZ) / 1000; //for quick probing
		// if (mi->probe_interval != (60 * HZ) / 1000) { //for slow probing
		// 	mi->probe_interval = (60 * HZ) / 1000;
		// 	mi->time_of_probing = jiffies;
		// }
		printk(KERN_INFO "Probe interval from probe_state = %ld\n", mi->probe_interval);
		if(idx>=0&&idx<=7) { //if the group is the left of the three groups
			go_to_the_right_group(idx,mi);
		}
		else if(idx>=20&&idx<=27) { //if the group is the right of the three groups
			go_to_the_left_group(idx,mi);
		}
		else if(mi->left_or_right_group==0) { //if the group is the middle one go to the left group
			//printk(KERN_INFO "MIDDLE->LEFT\n");
			mi->left_or_right_group = 1;
			go_to_the_left_group(idx,mi);
		}
		else if(mi->left_or_right_group==1) { //if the group is the middle one go to the right group
			//printk(KERN_INFO "MIDDLE->RIGHT\n");
			mi->left_or_right_group = 0;
			go_to_the_right_group(idx,mi);
		}
	}
	printk(KERN_INFO "r1=%d, r2=%d, r3=%d\n", mi->rates[0], mi->rates[1], mi->rates[2]);
	mi->probe_state = false;
}

//function that implements the l3s get rate function
static void get_rate(struct minstrel_ht_sta *mi, struct minstrel_priv *mp, int index, int i) {
	//l3s
	//probe()==true
	if(mi->probe_state) {
		printk(KERN_INFO "PROBE_STATE\n");
		probe_state(index,mi);
		minstrel_ht_update_rates(mp, mi);
		mi->current_state = 1;
	}
	else { //tx()==true
		//printk(KERN_INFO "TX_STATE\n");
		tx_state(mi,i);
		if (mi->current_state == 1) { //if previous state was probe state update the rates and reset the counters
			minstrel_ht_update_rates(mp, mi);
		}
		mi->current_state = 0;
	}
}

//function that makes recovery and goes immediately in the previous MCS rate
static void recovery(struct minstrel_ht_sta *mi) {
	int i;

	for(i=0; i<MAX_THR_RATES-1; i++) {
		if(mi->rates[i]%MCS_GROUP_RATES>=1) { //if position=1 make -1 in the index
			mi->rates[i] -= 1;
		}
		else if(mi->rates[i]!=0) { //if position=0 of 2nd and 3rd group make -3 to go the previous valid MCS rate
			mi->rates[i] -= 3;
		}
	}
}

//function that calculates the rate statistics(short-term) of l3s algorithm
static void rate_statistics(struct ieee80211_tx_info *info, struct minstrel_ht_sta *mi, struct minstrel_priv *mp, bool last, 
							struct ieee80211_tx_rate *ar, int i) {
	int index, j;
	//ar[i].count is the number of attempts to transmit a packet with this rate
	//so we reduce by 1 to get the retransmissions, if the packet is transmitted 
	//with r2 first attempt the consecutive_retries are 2 with r1
	mi->consecutive_retries[i] += ar[i].count - 1;
	if (mi->consecutive_retries[i]==0 && last) { //no retries
		//printk(KERN_INFO "ACK\n");
		//packet was acknowledged, update L3S-specific counters
		//increase successes, reset failures
		mi->consecutive_successes++;
		mi->consecutive_failures = 0;
		if (mi->consecutive_successes >= 10) {
			// if (mi->probe_interval != (90 * HZ) / 1000) { //for slow probing
			// 	mi->probe_interval = (90 * HZ) / 1000;
			// 	mi->time_of_probing = jiffies;
			// }
			mi->probe_interval = (90 * HZ) / 1000; //for quick probing
			printk(KERN_INFO "Probe interval from rate_statistics = %ld\n", mi->probe_interval);
		}
	} 
	else if(mi->consecutive_retries[i] >= 1) { //one or more retries, so missed acks
		printk(KERN_INFO "MISSED ACK\n");
		//packet was not acknowledged (missed ACK)
		//increase failures, reset successes
		mi->consecutive_successes = 0;
		mi->consecutive_failures += mi->consecutive_retries[i];
		if (mi->consecutive_failures >= 4) { //handle four or more consecutive failures
			// if (mi->probe_interval != (10 * HZ) / 1000) { //for slow probing
			// 	mi->probe_interval = (10 * HZ) / 1000;
			// 	mi->time_of_probing = jiffies;
			// }
			mi->probe_interval = (10 * HZ) / 1000; //for quick probing
			printk(KERN_INFO "Probe interval from rate_statistics = %ld\n", mi->probe_interval);
		}
		else if (mi->consecutive_failures >= 2) { //handle two consecutive missed ACKs
			printk(KERN_INFO "RECOVERY\n");
			//fallback immediately to the previous MCS rate
			recovery(mi); //this function is to handle recovery logic
			minstrel_ht_update_rates(mp, mi); //this function is to update the rate
			// if (mi->probe_interval != (30 * HZ) / 1000) { //for slow probing
			// 	mi->probe_interval = (30 * HZ) / 1000;
			// 	mi->time_of_probing = jiffies;
			// }
			mi->probe_interval = (30 * HZ) / 1000; //for quick probing
			printk(KERN_INFO "Probe interval from rate_statistics = %ld\n", mi->probe_interval);
		}
	}
	if(last) {
		if (mi->consecutive_failures!=0) { //reset the consecutive_failures and consecutive_retries for the next packet
			mi->consecutive_failures = 0;
			for(j=0; j<MAX_THR_RATES-1; j++) {
				mi->consecutive_retries[j]=0;
			}
		}
		index = ar[i].idx+(ar[i].idx/8)*(MCS_GROUP_RATES-8); //idx is for mcs and index for index of the group
		//packet may be more than one frame so the total packets counter must be incremented for all frames
		//and the ack counter must be incremented for all frames of the packet that were acknowledged
		mi->total_packets_per_mcs[index] += info->status.ampdu_len;
		mi->ack_counter_per_mcs[index] += info->status.ampdu_ack_len;
		//calculation of per statistics according the index
		if(mi->total_packets_per_mcs[index]!=0) {
			mi->per[index] = (100 * (mi->total_packets_per_mcs[index] - mi->ack_counter_per_mcs[index])) / mi->total_packets_per_mcs[index];
		}

		get_rate(mi, mp, index, i);
	}
}

static void
minstrel_ht_tx_status(void *priv, struct ieee80211_supported_band *sband,
                      void *priv_sta, struct ieee80211_tx_status *st)
{
	struct ieee80211_tx_info *info = st->info;
	struct minstrel_ht_sta_priv *msp = priv_sta;
	struct minstrel_ht_sta *mi = &msp->ht;
	struct ieee80211_tx_rate *ar = info->status.rates;
	struct minstrel_rate_stats *rate; //*rate2;
	struct minstrel_priv *mp = priv;
	bool last; //update = false;
	int i;

	if (!msp->is_ht)
		return mac80211_minstrel.tx_status_ext(priv, sband,
						       &msp->legacy, st);

	/* This packet was aggregated but doesn't carry status info */
	if ((info->flags & IEEE80211_TX_CTL_AMPDU) &&
	    !(info->flags & IEEE80211_TX_STAT_AMPDU))
		return;

	if (!(info->flags & IEEE80211_TX_STAT_AMPDU)) {
		info->status.ampdu_ack_len =
			(info->flags & IEEE80211_TX_STAT_ACK ? 1 : 0);
		info->status.ampdu_len = 1;
	}

	mi->ampdu_packets++;
	mi->ampdu_len += info->status.ampdu_len;

	if (!mi->sample_wait && !mi->sample_tries && mi->sample_count > 0) {
		int avg_ampdu_len = minstrel_ht_avg_ampdu_len(mi);

		mi->sample_wait = 16 + 2 * avg_ampdu_len;
		mi->sample_tries = 1;
		mi->sample_count--;
	}

	if (info->flags & IEEE80211_TX_CTL_RATE_CTRL_PROBE)
		mi->sample_packets += info->status.ampdu_len;

	last = !minstrel_ht_txstat_valid(mp, &ar[0]);
	for (i = 0; !last; i++) {
		last = (i == IEEE80211_TX_MAX_RATES - 1) ||
		       !minstrel_ht_txstat_valid(mp, &ar[i + 1]);

		rate = minstrel_ht_get_stats(mp, mi, &ar[i]);

		if (last)
			rate->success += info->status.ampdu_ack_len;

		rate->attempts += ar[i].count * info->status.ampdu_len;

		//l3s start
		rate_statistics(info,mi,mp,last,ar,i);
		//l3s end
	}

	//this code is out because is minstrel logic
	/*
	 * check for sudden death of spatial multiplexing,
	 * downgrade to a lower number of streams if necessary.
	 */
	// rate = minstrel_get_ratestats(mi, mi->max_tp_rate[0]);
	// if (rate->attempts > 30 &&
	//     MINSTREL_FRAC(rate->success, rate->attempts) <
	//     MINSTREL_FRAC(20, 100)) {
	// 	minstrel_downgrade_rate(mi, &mi->max_tp_rate[0], true);
	// 	update = true;
	// }

	// rate2 = minstrel_get_ratestats(mi, mi->max_tp_rate[1]);
	// if (rate2->attempts > 30 &&
	//     MINSTREL_FRAC(rate2->success, rate2->attempts) <
	//     MINSTREL_FRAC(20, 100)) {
	// 	minstrel_downgrade_rate(mi, &mi->max_tp_rate[1], false);
	// 	update = true;
	// }

	// if (time_after(jiffies, mi->last_stats_update +
	// 			(mp->update_interval / 2 * HZ) / 1000)) {
	// 	update = true;
	// 	minstrel_ht_update_stats(mp, mi);
	// }

	// if (update)
	// 	minstrel_ht_update_rates(mp, mi);
}

static inline int
minstrel_get_duration(int index)
{
	const struct mcs_group *group = &minstrel_mcs_groups[index / MCS_GROUP_RATES];
	unsigned int duration = group->duration[index % MCS_GROUP_RATES];
	return duration << group->shift;
}

static void
minstrel_calc_retransmit(struct minstrel_priv *mp, struct minstrel_ht_sta *mi,
                         int index)
{
	struct minstrel_rate_stats *mrs;
	unsigned int tx_time, tx_time_rtscts, tx_time_data;
	unsigned int cw = mp->cw_min;
	unsigned int ctime = 0;
	unsigned int t_slot = 9; /* FIXME */
	unsigned int ampdu_len = minstrel_ht_avg_ampdu_len(mi);
	unsigned int overhead = 0, overhead_rtscts = 0;

	mrs = minstrel_get_ratestats(mi, index);
	if (mrs->prob_ewma < MINSTREL_FRAC(1, 10)) {
		mrs->retry_count = 1;
		mrs->retry_count_rtscts = 1;
		return;
	}

	mrs->retry_count = 2;
	mrs->retry_count_rtscts = 2;
	mrs->retry_updated = true;

	tx_time_data = minstrel_get_duration(index) * ampdu_len / 1000;

	/* Contention time for first 2 tries */
	ctime = (t_slot * cw) >> 1;
	cw = min((cw << 1) | 1, mp->cw_max);
	ctime += (t_slot * cw) >> 1;
	cw = min((cw << 1) | 1, mp->cw_max);

	if (index / MCS_GROUP_RATES != MINSTREL_CCK_GROUP) {
		overhead = mi->overhead;
		overhead_rtscts = mi->overhead_rtscts;
	}

	/* Total TX time for data and Contention after first 2 tries */
	tx_time = ctime + 2 * (overhead + tx_time_data);
	tx_time_rtscts = ctime + 2 * (overhead_rtscts + tx_time_data);

	/* See how many more tries we can fit inside segment size */
	do {
		/* Contention time for this try */
		ctime = (t_slot * cw) >> 1;
		cw = min((cw << 1) | 1, mp->cw_max);

		/* Total TX time after this try */
		tx_time += ctime + overhead + tx_time_data;
		tx_time_rtscts += ctime + overhead_rtscts + tx_time_data;

		if (tx_time_rtscts < mp->segment_size)
			mrs->retry_count_rtscts++;
	} while ((tx_time < mp->segment_size) &&
	         (++mrs->retry_count < mp->max_retry));
}

static void
minstrel_ht_set_rate(struct minstrel_priv *mp, struct minstrel_ht_sta *mi,
                     struct ieee80211_sta_rates *ratetbl, int offset, int index)
{
	const struct mcs_group *group = &minstrel_mcs_groups[index / MCS_GROUP_RATES];
	struct minstrel_rate_stats *mrs;
	u8 idx;
	u16 flags = group->flags;

	mrs = minstrel_get_ratestats(mi, index);
	if (!mrs->retry_updated)
		minstrel_calc_retransmit(mp, mi, index);

	//l3s we change all the retry counts to 2
	ratetbl->rate[offset].count = 2;
	ratetbl->rate[offset].count_cts = 2;
	ratetbl->rate[offset].count_rts = 2;

	if (index / MCS_GROUP_RATES == MINSTREL_CCK_GROUP)
		idx = mp->cck_rates[index % ARRAY_SIZE(mp->cck_rates)];
	else if (flags & IEEE80211_TX_RC_VHT_MCS)
		idx = ((group->streams - 1) << 4) |
		      ((index % MCS_GROUP_RATES) & 0xF);
	else
		idx = index % MCS_GROUP_RATES + (group->streams - 1) * 8;

	/* enable RTS/CTS if needed:
	 *  - if station is in dynamic SMPS (and streams > 1)
	 *  - for fallback rates, to increase chances of getting through
	 */
	if (offset > 0 ||
	    (mi->sta->smps_mode == IEEE80211_SMPS_DYNAMIC &&
	     group->streams > 1)) {
		ratetbl->rate[offset].count = ratetbl->rate[offset].count_rts;
		flags |= IEEE80211_TX_RC_USE_RTS_CTS;
	}
	ratetbl->rate[offset].idx = idx;
	//printk(KERN_INFO "RATE_BL[%d]: %d\n", offset, idx);
	ratetbl->rate[offset].flags = flags;
}

static inline int
minstrel_ht_get_prob_ewma(struct minstrel_ht_sta *mi, int rate)
{
	int group = rate / MCS_GROUP_RATES;
	rate %= MCS_GROUP_RATES;
	return mi->groups[group].rates[rate].prob_ewma;
}

static int
minstrel_ht_get_max_amsdu_len(struct minstrel_ht_sta *mi)
{
	int group = mi->max_prob_rate / MCS_GROUP_RATES;
	const struct mcs_group *g = &minstrel_mcs_groups[group];
	int rate = mi->max_prob_rate % MCS_GROUP_RATES;
	unsigned int duration;

	/* Disable A-MSDU if max_prob_rate is bad */
	if (mi->groups[group].rates[rate].prob_ewma < MINSTREL_FRAC(50, 100))
		return 1;

	duration = g->duration[rate];
	duration <<= g->shift;

	/* If the rate is slower than single-stream MCS1, make A-MSDU limit small */
	if (duration > MCS_DURATION(1, 0, 52))
		return 500;

	/*
	 * If the rate is slower than single-stream MCS4, limit A-MSDU to usual
	 * data packet size
	 */
	if (duration > MCS_DURATION(1, 0, 104))
		return 1600;

	/*
	 * If the rate is slower than single-stream MCS7, or if the max throughput
	 * rate success probability is less than 75%, limit A-MSDU to twice the usual
	 * data packet size
	 */
	if (duration > MCS_DURATION(1, 0, 260) ||
	    (minstrel_ht_get_prob_ewma(mi, mi->max_tp_rate[0]) <
	     MINSTREL_FRAC(75, 100)))
		return 3200;

	/*
	 * HT A-MPDU limits maximum MPDU size under BA agreement to 4095 bytes.
	 * Since aggregation sessions are started/stopped without txq flush, use
	 * the limit here to avoid the complexity of having to de-aggregate
	 * packets in the queue.
	 */
	if (!mi->sta->vht_cap.vht_supported)
		return IEEE80211_MAX_MPDU_LEN_HT_BA;

	/* unlimited */
	return 0;
}

static void
minstrel_ht_update_rates(struct minstrel_priv *mp, struct minstrel_ht_sta *mi)
{
	struct ieee80211_sta_rates *rates;
	int i = 0;
	
	rates = kzalloc(sizeof(*rates), GFP_ATOMIC);
	if (!rates)
		return;
	//l3s start
	mi->previous_rate = mi->max_tp_rate[0];
	//l3s end
	/* Start with max_tp_rate[0] */
	minstrel_ht_set_rate(mp, mi, rates, i++, mi->rates[0]);

	if (mp->hw->max_rates >= 3) {
		/* At least 3 tx rates supported, use max_tp_rate[1] next */
		minstrel_ht_set_rate(mp, mi, rates, i++, mi->rates[1]);
	}

	if (mp->hw->max_rates >= 2) {
		//l3s start
		//minstrel_ht_set_rate(mp, mi, rates, i++, mi->max_prob_rate);
		minstrel_ht_set_rate(mp, mi, rates, i++, mi->rates[2]);
		//l3s end
	}
	//l3s start
	mi->current_rate = mi->rates[0];
	//set the new rates in max_tp_rate array
	for(i=0; i<MAX_THR_RATES-1; i++) {
		mi->max_tp_rate[i]=mi->rates[i];
	}
	//l3s end
	mi->sta->max_rc_amsdu_len = minstrel_ht_get_max_amsdu_len(mi);
	rates->rate[i].idx = -1;
	rate_control_set_rates(mp->hw, mi->sta, rates);

	//l3s start
	//reset counters
	mi->consecutive_successes = 0;
	mi->consecutive_failures = 0;
	for(i=0; i<MAX_THR_RATES-1; i++) {
		mi->consecutive_retries[i]=0;
	}
	if(!mi->recovery) { //if recovery is true don't check if rate increase
		if(mi->previous_rate/MCS_GROUP_RATES==mi->current_rate/MCS_GROUP_RATES) { //same group
			if(mi->current_rate>mi->previous_rate) {
				printk(KERN_INFO "SAME_GROUP_INCREASE_RATE\n");
				// if (mi->probe_interval != (20 * HZ) / 1000) { //for slow probing
				// 	mi->probe_interval = (20 * HZ) / 1000;
				// 	mi->time_of_probing = jiffies;
				// }
				mi->probe_interval = (20 * HZ) / 1000; //for quick probing
				printk(KERN_INFO "Probe interval from update_rates = %ld\n", mi->probe_interval);
			}
		}
		else if(mi->current_rate/MCS_GROUP_RATES>mi->previous_rate/MCS_GROUP_RATES) { //different groups, group is increased
			printk(KERN_INFO "DIFFERENT_GROUP_INCREASE_RATE\n");
			// if (mi->probe_interval != (20 * HZ) / 1000) { //for slow probing
			// 	mi->probe_interval = (20 * HZ) / 1000;
			// 	mi->time_of_probing = jiffies;
			// }
			mi->probe_interval = (20 * HZ) / 1000; //for quick probing
			printk(KERN_INFO "Probe interval from update_rates = %ld\n", mi->probe_interval);
		}
	}
	//l3s end
}

static int
minstrel_get_sample_rate(struct minstrel_priv *mp, struct minstrel_ht_sta *mi)
{
	struct minstrel_rate_stats *mrs;
	struct minstrel_mcs_group_data *mg;
	unsigned int sample_dur, sample_group, cur_max_tp_streams;
	int tp_rate1, tp_rate2;
	int sample_idx = 0;

	if (mi->sample_wait > 0) {
		mi->sample_wait--;
		return -1;
	}

	if (!mi->sample_tries)
		return -1;

	sample_group = mi->sample_group;
	mg = &mi->groups[sample_group];
	sample_idx = sample_table[mg->column][mg->index];
	minstrel_set_next_sample_idx(mi);

	if (!(mi->supported[sample_group] & BIT(sample_idx)))
		return -1;

	mrs = &mg->rates[sample_idx];
	sample_idx += sample_group * MCS_GROUP_RATES;

	/* Set tp_rate1, tp_rate2 to the highest / second highest max_tp_rate */
	if (minstrel_get_duration(mi->max_tp_rate[0]) >
	    minstrel_get_duration(mi->max_tp_rate[1])) {
		tp_rate1 = mi->max_tp_rate[1];
		tp_rate2 = mi->max_tp_rate[0];
	} else {
		tp_rate1 = mi->max_tp_rate[0];
		tp_rate2 = mi->max_tp_rate[1];
	}

	/*
	 * Sampling might add some overhead (RTS, no aggregation)
	 * to the frame. Hence, don't use sampling for the highest currently
	 * used highest throughput or probability rate.
	 */
	if (sample_idx == mi->max_tp_rate[0] || sample_idx == mi->max_prob_rate)
		return -1;

	/*
	 * Do not sample if the probability is already higher than 95%,
	 * or if the rate is 3 times slower than the current max probability
	 * rate, to avoid wasting airtime.
	 */
	sample_dur = minstrel_get_duration(sample_idx);
	if (mrs->prob_ewma > MINSTREL_FRAC(95, 100) ||
	    minstrel_get_duration(mi->max_prob_rate) * 3 < sample_dur)
		return -1;

	/*
	 * Make sure that lower rates get sampled only occasionally,
	 * if the link is working perfectly.
	 */

	cur_max_tp_streams = minstrel_mcs_groups[tp_rate1 /
		MCS_GROUP_RATES].streams;
	if (sample_dur >= minstrel_get_duration(tp_rate2) &&
	    (cur_max_tp_streams - 1 <
	     minstrel_mcs_groups[sample_group].streams ||
	     sample_dur >= minstrel_get_duration(mi->max_prob_rate))) {
		if (mrs->sample_skipped < 20)
			return -1;

		if (mi->sample_slow++ > 2)
			return -1;
	}
	mi->sample_tries--;

	return sample_idx;
}

static void
minstrel_ht_get_rate(void *priv, struct ieee80211_sta *sta, void *priv_sta,
                     struct ieee80211_tx_rate_control *txrc)
{
	const struct mcs_group *sample_group;
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(txrc->skb);
	struct ieee80211_tx_rate *rate = &info->status.rates[0];
	struct minstrel_ht_sta_priv *msp = priv_sta;
	struct minstrel_ht_sta *mi = &msp->ht;
	struct minstrel_priv *mp = priv;
	int sample_idx;

	if (!msp->is_ht)
		return mac80211_minstrel.get_rate(priv, sta, &msp->legacy, txrc);

	if (!(info->flags & IEEE80211_TX_CTL_AMPDU) &&
	    mi->max_prob_rate / MCS_GROUP_RATES != MINSTREL_CCK_GROUP)
		minstrel_aggr_check(sta, txrc->skb);

	info->flags |= mi->tx_flags;

#ifdef CONFIG_MAC80211_DEBUGFS
	if (mp->fixed_rate_idx != -1)
		return;
#endif

	//l3s start
	//periodically called logic to manage rate control setting the probe state to true
	if (!mi->probe_state && time_after(jiffies, mi->time_of_probing + mi->probe_interval)) { //(jiffies-time_of_probing) > probe_interval
		//printk(KERN_INFO "TIMER!!! START PROBING WITH %ld\n", mi->probe_interval);
		mi->probe_state = true;
		mi->time_of_probing = jiffies; //for quick probing
	}
	//l3s end
	
	/* Don't use EAPOL frames for sampling on non-mrr hw */
	if (mp->hw->max_rates == 1 &&
	    (info->control.flags & IEEE80211_TX_CTRL_PORT_CTRL_PROTO))
		sample_idx = -1;
	else
		sample_idx = minstrel_get_sample_rate(mp, mi);

	mi->total_packets++;

	/* wraparound */
	if (mi->total_packets == ~0) {
		mi->total_packets = 0;
		mi->sample_packets = 0;
	}

	if (sample_idx < 0)
		return;

	sample_group = &minstrel_mcs_groups[sample_idx / MCS_GROUP_RATES];
	sample_idx %= MCS_GROUP_RATES;

	if (sample_group == &minstrel_mcs_groups[MINSTREL_CCK_GROUP] &&
	    (sample_idx >= 4) != txrc->short_preamble)
		return;

	info->flags |= IEEE80211_TX_CTL_RATE_CTRL_PROBE;
	rate->count = 1;

	if (sample_group == &minstrel_mcs_groups[MINSTREL_CCK_GROUP]) {
		int idx = sample_idx % ARRAY_SIZE(mp->cck_rates);
		rate->idx = mp->cck_rates[idx];
	} else if (sample_group->flags & IEEE80211_TX_RC_VHT_MCS) {
		ieee80211_rate_set_vht(rate, sample_idx % MCS_GROUP_RATES,
				       sample_group->streams);
	} else {
		rate->idx = sample_idx + (sample_group->streams - 1) * 8;
	}

	rate->flags = sample_group->flags;
}

static void
minstrel_ht_update_cck(struct minstrel_priv *mp, struct minstrel_ht_sta *mi,
		       struct ieee80211_supported_band *sband,
		       struct ieee80211_sta *sta)
{
	int i;

	if (sband->band != NL80211_BAND_2GHZ)
		return;

	if (!ieee80211_hw_check(mp->hw, SUPPORTS_HT_CCK_RATES))
		return;

	mi->cck_supported = 0;
	mi->cck_supported_short = 0;
	for (i = 0; i < 4; i++) {
		if (!rate_supported(sta, sband->band, mp->cck_rates[i]))
			continue;

		mi->cck_supported |= BIT(i);
		if (sband->bitrates[i].flags & IEEE80211_RATE_SHORT_PREAMBLE)
			mi->cck_supported_short |= BIT(i);
	}

	mi->supported[MINSTREL_CCK_GROUP] = mi->cck_supported;
}

static void
minstrel_ht_update_caps(void *priv, struct ieee80211_supported_band *sband,
			struct cfg80211_chan_def *chandef,
                        struct ieee80211_sta *sta, void *priv_sta)
{
	struct minstrel_priv *mp = priv;
	struct minstrel_ht_sta_priv *msp = priv_sta;
	struct minstrel_ht_sta *mi = &msp->ht;
	struct ieee80211_mcs_info *mcs = &sta->ht_cap.mcs;
	u16 ht_cap = sta->ht_cap.cap;
	struct ieee80211_sta_vht_cap *vht_cap = &sta->vht_cap;
	int use_vht;
	int n_supported = 0;
	int ack_dur;
	int stbc;
	int i;
	bool ldpc;

	/* fall back to the old minstrel for legacy stations */
	if (!sta->ht_cap.ht_supported)
		goto use_legacy;

	BUILD_BUG_ON(ARRAY_SIZE(minstrel_mcs_groups) != MINSTREL_GROUPS_NB);

	if (vht_cap->vht_supported)
		use_vht = vht_cap->vht_mcs.tx_mcs_map != cpu_to_le16(~0);
	else
		use_vht = 0;

	msp->is_ht = true;
	memset(mi, 0, sizeof(*mi));

	mi->sta = sta;
	mi->last_stats_update = jiffies;
	//l3s start
	printk(KERN_INFO "Initialization\n");
	mi->probe_interval = (60 * HZ) / 1000;
	mi->time_of_probing = jiffies;
	printk(KERN_INFO "Probe interval from update_caps = %ld\n", mi->probe_interval);
	//l3s end

	ack_dur = ieee80211_frame_duration(sband->band, 10, 60, 1, 1, 0);
	mi->overhead = ieee80211_frame_duration(sband->band, 0, 60, 1, 1, 0);
	mi->overhead += ack_dur;
	mi->overhead_rtscts = mi->overhead + 2 * ack_dur;

	mi->avg_ampdu_len = MINSTREL_FRAC(1, 1);

	/* When using MRR, sample more on the first attempt, without delay */
	if (mp->has_mrr) {
		mi->sample_count = 16;
		mi->sample_wait = 0;
	} else {
		mi->sample_count = 8;
		mi->sample_wait = 8;
	}
	mi->sample_tries = 4;

	if (!use_vht) {
		stbc = (ht_cap & IEEE80211_HT_CAP_RX_STBC) >>
			IEEE80211_HT_CAP_RX_STBC_SHIFT;

		ldpc = ht_cap & IEEE80211_HT_CAP_LDPC_CODING;
	} else {
		stbc = (vht_cap->cap & IEEE80211_VHT_CAP_RXSTBC_MASK) >>
			IEEE80211_VHT_CAP_RXSTBC_SHIFT;

		ldpc = vht_cap->cap & IEEE80211_VHT_CAP_RXLDPC;
	}

	mi->tx_flags |= stbc << IEEE80211_TX_CTL_STBC_SHIFT;
	if (ldpc)
		mi->tx_flags |= IEEE80211_TX_CTL_LDPC;

	for (i = 0; i < ARRAY_SIZE(mi->groups); i++) {
		u32 gflags = minstrel_mcs_groups[i].flags;
		int bw, nss;

		mi->supported[i] = 0;
		if (i == MINSTREL_CCK_GROUP) {
			minstrel_ht_update_cck(mp, mi, sband, sta);
			continue;
		}

		if (gflags & IEEE80211_TX_RC_SHORT_GI) {
			if (gflags & IEEE80211_TX_RC_40_MHZ_WIDTH) {
				if (!(ht_cap & IEEE80211_HT_CAP_SGI_40))
					continue;
			} else {
				if (!(ht_cap & IEEE80211_HT_CAP_SGI_20))
					continue;
			}
		}

		if (gflags & IEEE80211_TX_RC_40_MHZ_WIDTH &&
		    sta->bandwidth < IEEE80211_STA_RX_BW_40)
			continue;

		nss = minstrel_mcs_groups[i].streams;

		/* Mark MCS > 7 as unsupported if STA is in static SMPS mode */
		if (sta->smps_mode == IEEE80211_SMPS_STATIC && nss > 1)
			continue;

		/* HT rate */
		if (gflags & IEEE80211_TX_RC_MCS) {
			if (use_vht && minstrel_vht_only)
				continue;

			mi->supported[i] = mcs->rx_mask[nss - 1];
			if (mi->supported[i])
				n_supported++;
			continue;
		}

		/* VHT rate */
		if (!vht_cap->vht_supported ||
		    WARN_ON(!(gflags & IEEE80211_TX_RC_VHT_MCS)) ||
		    WARN_ON(gflags & IEEE80211_TX_RC_160_MHZ_WIDTH))
			continue;

		if (gflags & IEEE80211_TX_RC_80_MHZ_WIDTH) {
			if (sta->bandwidth < IEEE80211_STA_RX_BW_80 ||
			    ((gflags & IEEE80211_TX_RC_SHORT_GI) &&
			     !(vht_cap->cap & IEEE80211_VHT_CAP_SHORT_GI_80))) {
				continue;
			}
		}

		if (gflags & IEEE80211_TX_RC_40_MHZ_WIDTH)
			bw = BW_40;
		else if (gflags & IEEE80211_TX_RC_80_MHZ_WIDTH)
			bw = BW_80;
		else
			bw = BW_20;

		mi->supported[i] = minstrel_get_valid_vht_rates(bw, nss,
				vht_cap->vht_mcs.tx_mcs_map);

		if (mi->supported[i])
			n_supported++;
	}

	if (!n_supported)
		goto use_legacy;

	mi->supported[MINSTREL_CCK_GROUP] |= mi->cck_supported_short << 4;

	/* create an initial rate table with the lowest supported rates */
	minstrel_ht_update_stats(mp, mi);
	minstrel_ht_update_rates(mp, mi);

	return;

use_legacy:
	msp->is_ht = false;
	memset(&msp->legacy, 0, sizeof(msp->legacy));
	msp->legacy.r = msp->ratelist;
	msp->legacy.sample_table = msp->sample_table;
	return mac80211_minstrel.rate_init(priv, sband, chandef, sta,
					   &msp->legacy);
}

static void
minstrel_ht_rate_init(void *priv, struct ieee80211_supported_band *sband,
		      struct cfg80211_chan_def *chandef,
                      struct ieee80211_sta *sta, void *priv_sta)
{
	minstrel_ht_update_caps(priv, sband, chandef, sta, priv_sta);
}

static void
minstrel_ht_rate_update(void *priv, struct ieee80211_supported_band *sband,
			struct cfg80211_chan_def *chandef,
                        struct ieee80211_sta *sta, void *priv_sta,
                        u32 changed)
{
	minstrel_ht_update_caps(priv, sband, chandef, sta, priv_sta);
}

static void *
minstrel_ht_alloc_sta(void *priv, struct ieee80211_sta *sta, gfp_t gfp)
{
	struct ieee80211_supported_band *sband;
	struct minstrel_ht_sta_priv *msp;
	struct minstrel_priv *mp = priv;
	struct ieee80211_hw *hw = mp->hw;
	int max_rates = 0;
	int i;

	for (i = 0; i < NUM_NL80211_BANDS; i++) {
		sband = hw->wiphy->bands[i];
		if (sband && sband->n_bitrates > max_rates)
			max_rates = sband->n_bitrates;
	}

	msp = kzalloc(sizeof(*msp), gfp);
	if (!msp)
		return NULL;

	msp->ratelist = kcalloc(max_rates, sizeof(struct minstrel_rate), gfp);
	if (!msp->ratelist)
		goto error;

	msp->sample_table = kmalloc_array(max_rates, SAMPLE_COLUMNS, gfp);
	if (!msp->sample_table)
		goto error1;

	return msp;

error1:
	kfree(msp->ratelist);
error:
	kfree(msp);
	return NULL;
}

static void
minstrel_ht_free_sta(void *priv, struct ieee80211_sta *sta, void *priv_sta)
{
	struct minstrel_ht_sta_priv *msp = priv_sta;

	kfree(msp->sample_table);
	kfree(msp->ratelist);
	kfree(msp);
}

static void
minstrel_ht_init_cck_rates(struct minstrel_priv *mp)
{
	static const int bitrates[4] = { 10, 20, 55, 110 };
	struct ieee80211_supported_band *sband;
	u32 rate_flags = ieee80211_chandef_rate_flags(&mp->hw->conf.chandef);
	int i, j;

	sband = mp->hw->wiphy->bands[NL80211_BAND_2GHZ];
	if (!sband)
		return;

	for (i = 0; i < sband->n_bitrates; i++) {
		struct ieee80211_rate *rate = &sband->bitrates[i];

		if (rate->flags & IEEE80211_RATE_ERP_G)
			continue;

		if ((rate_flags & sband->bitrates[i].flags) != rate_flags)
			continue;

		for (j = 0; j < ARRAY_SIZE(bitrates); j++) {
			if (rate->bitrate != bitrates[j])
				continue;

			mp->cck_rates[j] = i;
			break;
		}
	}
}

static void *
minstrel_ht_alloc(struct ieee80211_hw *hw, struct dentry *debugfsdir)
{
	struct minstrel_priv *mp;

	mp = kzalloc(sizeof(struct minstrel_priv), GFP_ATOMIC);
	if (!mp)
		return NULL;

	/* contention window settings
	 * Just an approximation. Using the per-queue values would complicate
	 * the calculations and is probably unnecessary */
	mp->cw_min = 15;
	mp->cw_max = 1023;

	/* number of packets (in %) to use for sampling other rates
	 * sample less often for non-mrr packets, because the overhead
	 * is much higher than with mrr */
	mp->lookaround_rate = 5;
	mp->lookaround_rate_mrr = 10;

	/* maximum time that the hw is allowed to stay in one MRR segment */
	mp->segment_size = 6000;

	if (hw->max_rate_tries > 0)
		mp->max_retry = hw->max_rate_tries;
	else
		/* safe default, does not necessarily have to match hw properties */
		mp->max_retry = 7;

	if (hw->max_rates >= 4)
		mp->has_mrr = true;

	mp->hw = hw;
	mp->update_interval = 100;

#ifdef CONFIG_MAC80211_DEBUGFS
	mp->fixed_rate_idx = (u32) -1;
	debugfs_create_u32("fixed_rate_idx", S_IRUGO | S_IWUGO, debugfsdir,
			   &mp->fixed_rate_idx);
#endif

	minstrel_ht_init_cck_rates(mp);

	return mp;
}

static void
minstrel_ht_free(void *priv)
{
	kfree(priv);
}

static u32 minstrel_ht_get_expected_throughput(void *priv_sta)
{
	struct minstrel_ht_sta_priv *msp = priv_sta;
	struct minstrel_ht_sta *mi = &msp->ht;
	int i, j, prob, tp_avg;

	if (!msp->is_ht)
		return mac80211_minstrel.get_expected_throughput(priv_sta);

	i = mi->max_tp_rate[0] / MCS_GROUP_RATES;
	j = mi->max_tp_rate[0] % MCS_GROUP_RATES;
	prob = mi->groups[i].rates[j].prob_ewma;

	/* convert tp_avg from pkt per second in kbps */
	tp_avg = minstrel_ht_get_tp_avg(mi, i, j, prob) * 10;
	tp_avg = tp_avg * AVG_PKT_SIZE * 8 / 1024;

	return tp_avg;
}

static const struct rate_control_ops mac80211_minstrel_ht = {
	.name = "minstrel_ht",
	.tx_status_ext = minstrel_ht_tx_status,
	.get_rate = minstrel_ht_get_rate,
	.rate_init = minstrel_ht_rate_init,
	.rate_update = minstrel_ht_rate_update,
	.alloc_sta = minstrel_ht_alloc_sta,
	.free_sta = minstrel_ht_free_sta,
	.alloc = minstrel_ht_alloc,
	.free = minstrel_ht_free,
#ifdef CONFIG_MAC80211_DEBUGFS
	.add_sta_debugfs = minstrel_ht_add_sta_debugfs,
#endif
	.get_expected_throughput = minstrel_ht_get_expected_throughput,
};


static void __init init_sample_table(void)
{
	int col, i, new_idx;
	u8 rnd[MCS_GROUP_RATES];

	memset(sample_table, 0xff, sizeof(sample_table));
	for (col = 0; col < SAMPLE_COLUMNS; col++) {
		prandom_bytes(rnd, sizeof(rnd));
		for (i = 0; i < MCS_GROUP_RATES; i++) {
			new_idx = (i + rnd[i]) % MCS_GROUP_RATES;
			while (sample_table[col][new_idx] != 0xff)
				new_idx = (new_idx + 1) % MCS_GROUP_RATES;

			sample_table[col][new_idx] = i;
		}
	}
}

int __init
rc80211_minstrel_init(void)
{
	init_sample_table();
	return ieee80211_rate_control_register(&mac80211_minstrel_ht);
}

void
rc80211_minstrel_exit(void)
{
	ieee80211_rate_control_unregister(&mac80211_minstrel_ht);
}