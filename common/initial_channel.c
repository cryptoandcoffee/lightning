#include <assert.h>
#include <bitcoin/chainparams.h>
#include <bitcoin/script.h>
#include <ccan/tal/str/str.h>
#include <common/initial_channel.h>
#include <common/initial_commit_tx.h>
#include <common/keyset.h>
#include <common/type_to_string.h>
#include <inttypes.h>

struct channel *new_initial_channel(const tal_t *ctx,
				    const struct bitcoin_blkid *chain_hash,
				    const struct bitcoin_txid *funding_txid,
				    unsigned int funding_txout,
				    u32 minimum_depth,
				    struct amount_sat funding,
				    struct amount_msat local_msatoshi,
				    u32 feerate_per_kw,
				    const struct channel_config *local,
				    const struct channel_config *remote,
				    const struct basepoints *local_basepoints,
				    const struct basepoints *remote_basepoints,
				    const struct pubkey *local_funding_pubkey,
				    const struct pubkey *remote_funding_pubkey,
				    enum side funder)
{
	struct channel *channel = tal(ctx, struct channel);
	struct amount_msat remote_msatoshi;

	channel->funding_txid = *funding_txid;
	channel->funding_txout = funding_txout;
	channel->funding = funding;
	channel->minimum_depth = minimum_depth;
	if (!amount_sat_sub_msat(&remote_msatoshi,
				 channel->funding, local_msatoshi))
		return tal_free(channel);

	channel->funder = funder;
	channel->config[LOCAL] = *local;
	channel->config[REMOTE] = *remote;
	channel->funding_pubkey[LOCAL] = *local_funding_pubkey;
	channel->funding_pubkey[REMOTE] = *remote_funding_pubkey;
	channel->htlcs = NULL;
	channel->changes_pending[LOCAL] = channel->changes_pending[REMOTE]
		= false;

	channel->view[LOCAL].feerate_per_kw
		= channel->view[REMOTE].feerate_per_kw
		= feerate_per_kw;

	channel->view[LOCAL].owed[LOCAL]
		= channel->view[REMOTE].owed[LOCAL]
		= local_msatoshi;
	channel->view[REMOTE].owed[REMOTE]
		= channel->view[LOCAL].owed[REMOTE]
		= remote_msatoshi;

	channel->basepoints[LOCAL] = *local_basepoints;
	channel->basepoints[REMOTE] = *remote_basepoints;

	channel->commitment_number_obscurer
		= commit_number_obscurer(&channel->basepoints[funder].payment,
					 &channel->basepoints[!funder].payment);
	channel->chainparams = chainparams_by_chainhash(chain_hash);
	if (channel->chainparams == NULL)
		return tal_free(channel);

	return channel;
}

/* FIXME: We could cache this. */
struct bitcoin_tx *initial_channel_tx(const tal_t *ctx,
				      const u8 **wscript,
				      const struct channel *channel,
				      const struct pubkey *per_commitment_point,
				      enum side side,
				      char** err_reason)
{
	struct keyset keyset;

	/* This assumes no HTLCs! */
	assert(!channel->htlcs);

	if (!derive_keyset(per_commitment_point,
			   &channel->basepoints[side],
			   &channel->basepoints[!side],
			   &keyset)){
		*err_reason = "Cannot derive keyset";
		return NULL;
	}

	*wscript = bitcoin_redeem_2of2(ctx,
				       &channel->funding_pubkey[side],
				       &channel->funding_pubkey[!side]);

	return initial_commit_tx(ctx, channel->chainparams,
				 &channel->funding_txid,
				 channel->funding_txout,
				 channel->funding,
				 channel->funder,
				 /* They specify our to_self_delay and v.v. */
				 channel->config[!side].to_self_delay,
				 &keyset,
				 channel->view[side].feerate_per_kw,
				 channel->config[side].dust_limit,
				 channel->view[side].owed[side],
				 channel->view[side].owed[!side],
				 channel->config[!side].channel_reserve,
				 0 ^ channel->commitment_number_obscurer,
				 side,
				 err_reason);
}

static char *fmt_channel_view(const tal_t *ctx, const struct channel_view *view)
{
	return tal_fmt(ctx, "{ feerate_per_kw=%"PRIu32","
		       " owed_local=%s,"
		       " owed_remote=%s }",
		       view->feerate_per_kw,
		       type_to_string(tmpctx, struct amount_msat,
				      &view->owed[LOCAL]),
		       type_to_string(tmpctx, struct amount_msat,
				      &view->owed[REMOTE]));
}

/* FIXME: This should reference HTLCs somehow. */
static char *fmt_channel(const tal_t *ctx, const struct channel *channel)
{
	return tal_fmt(ctx, "{ funding=%s,"
		       " funder=%s,"
		       " local=%s,"
		       " remote=%s }",
		       type_to_string(tmpctx, struct amount_sat,
				      &channel->funding),
		       side_to_str(channel->funder),
		       fmt_channel_view(ctx, &channel->view[LOCAL]),
		       fmt_channel_view(ctx, &channel->view[REMOTE]));
}
REGISTER_TYPE_TO_STRING(channel, fmt_channel);
