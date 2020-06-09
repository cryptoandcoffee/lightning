#include <common/rpcclient.h>

#include <assert.h>

static struct listpeers_channel *json_to_listpeers_channel(const tal_t *ctx,
							   const char *buffer,
							   const jsmntok_t *tok)
{
	struct listpeers_channel *chan;
	const jsmntok_t *privtok = json_get_member(buffer, tok, "private"),
			*statetok = json_get_member(buffer, tok, "state"),
			*ftxidtok =
			    json_get_member(buffer, tok, "funding_txid"),
			*scidtok =
			    json_get_member(buffer, tok, "short_channel_id"),
			*dirtok = json_get_member(buffer, tok, "direction"),
			*tmsattok =
			    json_get_member(buffer, tok, "total_msat"),
			*smsattok =
			    json_get_member(buffer, tok, "spendable_msat");

	if (privtok == NULL || privtok->type != JSMN_PRIMITIVE ||
	    statetok == NULL || statetok->type != JSMN_STRING ||
	    ftxidtok == NULL || ftxidtok->type != JSMN_STRING ||
	    (scidtok != NULL && scidtok->type != JSMN_STRING) ||
	    (dirtok != NULL && dirtok->type != JSMN_PRIMITIVE) ||
	    tmsattok == NULL || tmsattok->type != JSMN_STRING ||
	    smsattok == NULL || smsattok->type != JSMN_STRING)
		return NULL;

	chan = tal(ctx, struct listpeers_channel);

	json_to_bool(buffer, privtok, &chan->private);
	chan->state = json_strdup(chan, buffer, statetok);
	json_to_txid(buffer, ftxidtok, &chan->funding_txid);
	if (scidtok != NULL) {
		assert(dirtok != NULL);
		chan->scid = tal(chan, struct short_channel_id);
		chan->direction = tal(chan, int);
		json_to_short_channel_id(buffer, scidtok, chan->scid);
		json_to_int(buffer, dirtok, chan->direction);
	}else {
		assert(dirtok == NULL);
		chan->scid = NULL;
		chan->direction = NULL;
	}

	json_to_msat(buffer, tmsattok, &chan->total_msat);
	json_to_msat(buffer, smsattok, &chan->spendable_msat);

	return chan;
}

static struct listpeers_peer *json_to_listpeers_peer(const tal_t *ctx,
						  const char *buffer,
						  const jsmntok_t *tok)
{
	struct listpeers_peer *res;
	size_t i;
	const jsmntok_t *iter;
	const jsmntok_t *idtok = json_get_member(buffer, tok, "id"),
			*conntok = json_get_member(buffer, tok, "connected"),
			*netaddrtok = json_get_member(buffer, tok, "netaddr"),
			*channelstok = json_get_member(buffer, tok, "channels");

	/* Preliminary sanity checks. */
	if (idtok == NULL || idtok->type != JSMN_STRING || conntok == NULL ||
	    conntok->type != JSMN_PRIMITIVE ||
	    (netaddrtok != NULL && netaddrtok->type != JSMN_ARRAY) ||
	    channelstok == NULL || channelstok->type != JSMN_ARRAY)
		return NULL;

	res = tal(ctx, struct listpeers_peer);
	json_to_node_id(buffer, idtok, &res->id);
	json_to_bool(buffer, conntok, &res->connected);

	res->netaddr = tal_arr(res, const char *, 0);
	if (netaddrtok != NULL) {
		json_for_each_arr(i, iter, netaddrtok) {
			tal_arr_expand(&res->netaddr,
				       json_strdup(res, buffer, iter));
		}
	}

	res->channels = tal_arr(res, struct listpeers_channel *, 0);
	json_for_each_arr(i, iter, channelstok) {
		struct listpeers_channel *chan = json_to_listpeers_channel(res, buffer, iter);
		assert(chan != NULL);
		tal_arr_expand(&res->channels, chan);
	}

	return res;
}

struct listpeers_result *json_to_listpeers_result(const tal_t *ctx,
							  const char *buffer,
							  const jsmntok_t *toks)
{
	size_t i;
	const jsmntok_t *iter;
	struct listpeers_result *res;
	const jsmntok_t *peerstok = json_get_member(buffer, toks, "peers");

	if (peerstok == NULL || peerstok->type != JSMN_ARRAY)
		return NULL;

	res = tal(ctx, struct listpeers_result);
	res->peers = tal_arr(res, struct listpeers_peer *, 0);

	json_for_each_obj(i, iter, peerstok) {
		struct listpeers_peer *p =
		    json_to_listpeers_peer(res, buffer, iter);
		if (p == NULL)
			return tal_free(res);
		tal_arr_expand(&res->peers, p);
	}
	return res;
}

struct createonion_response *json_to_createonion_response(const tal_t *ctx,
							  const char *buffer,
							  const jsmntok_t *toks)
{
	size_t i;
	struct createonion_response *resp;
	const jsmntok_t *oniontok = json_get_member(buffer, toks, "onion");
	const jsmntok_t *secretstok = json_get_member(buffer, toks, "shared_secrets");
	const jsmntok_t *cursectok;

	if (oniontok == NULL || secretstok == NULL)
		return NULL;

	resp = tal(ctx, struct createonion_response);

	if (oniontok->type != JSMN_STRING)
		goto fail;

	resp->onion = json_tok_bin_from_hex(resp, buffer, oniontok);
	resp->shared_secrets = tal_arr(resp, struct secret, secretstok->size);

	json_for_each_arr(i, cursectok, secretstok) {
		if (cursectok->type != JSMN_STRING)
			goto fail;
		json_to_secret(buffer, cursectok, &resp->shared_secrets[i]);
	}
	return resp;

fail:
	return tal_free(resp);
}

static bool json_to_route_hop_inplace(struct route_hop *dst, const char *buffer,
				      const jsmntok_t *toks)
{
	const jsmntok_t *idtok = json_get_member(buffer, toks, "id");
	const jsmntok_t *channeltok = json_get_member(buffer, toks, "channel");
	const jsmntok_t *directiontok = json_get_member(buffer, toks, "direction");
	const jsmntok_t *amounttok = json_get_member(buffer, toks, "amount_msat");
	const jsmntok_t *delaytok = json_get_member(buffer, toks, "delay");
	const jsmntok_t *styletok = json_get_member(buffer, toks, "style");

	if (idtok == NULL || channeltok == NULL || directiontok == NULL ||
	    amounttok == NULL || delaytok == NULL || styletok == NULL)
		return false;

	json_to_node_id(buffer, idtok, &dst->nodeid);
	json_to_short_channel_id(buffer, channeltok, &dst->channel_id);
	json_to_int(buffer, directiontok, &dst->direction);
	json_to_msat(buffer, amounttok, &dst->amount);
	json_to_number(buffer, delaytok, &dst->delay);
	dst->style = json_tok_streq(buffer, styletok, "legacy")
			 ? ROUTE_HOP_LEGACY
			 : ROUTE_HOP_TLV;
	return true;
}

struct route_hop *json_to_route(const tal_t *ctx, const char *buffer,
				const jsmntok_t *toks)
{
	size_t num = toks->size, i;
	struct route_hop *hops;
	const jsmntok_t *rtok;
	if (toks->type != JSMN_ARRAY)
		return NULL;

	hops = tal_arr(ctx, struct route_hop, num);
	json_for_each_arr(i, rtok, toks) {
		if (!json_to_route_hop_inplace(&hops[i], buffer, rtok))
			return tal_free(hops);
	}
	return hops;
}
