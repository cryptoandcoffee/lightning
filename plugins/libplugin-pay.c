#include "common/type_to_string.h"
#include <plugins/libplugin-pay.h>
#include <stdio.h>

#include <bitcoin/preimage.h>
#include <ccan/array_size/array_size.h>
#include <ccan/tal/str/str.h>
#include <common/json_stream.h>

#define DEFAULT_FINAL_CLTV_DELTA 9

struct payment *payment_new(tal_t *ctx, struct command *cmd,
			    struct payment *parent,
			    struct payment_modifier **mods)
{
	struct payment *p = tal(ctx, struct payment);
	p->children = tal_arr(p, struct payment *, 0);
	p->parent = parent;
	p->modifiers = mods;
	p->cmd = cmd;
	p->start_time = time_now();
	p->result = NULL;
	p->why = NULL;
	p->getroute = tal(p, struct getroute_request);

	/* Copy over the relevant pieces of information. */
	if (parent != NULL) {
		assert(cmd == NULL);
		tal_arr_expand(&parent->children, p);
		p->destination = parent->destination;
		p->amount = parent->amount;
		p->payment_hash = parent->payment_hash;
		p->partid = payment_root(p->parent)->next_partid++;
		p->plugin = parent->plugin;
		p->fee_budget = parent->fee_budget;
		p->cltv_budget = parent->cltv_budget;
	} else {
		assert(cmd != NULL);
		p->partid = 0;
		p->next_partid = 1;
		p->plugin = cmd->plugin;
		p->channel_hints = tal_arr(p, struct channel_hint, 0);
		p->excluded_nodes = tal_arr(p, struct node_id, 0);
		p->abort = false;
	}

	/* Initialize all modifier data so we can point to the fields when
	 * wiring into the param() call in a JSON-RPC handler. The callback
	 * can also just `memcpy` the parent if this outside access is not
	 * required. */
	p->modifier_data = tal_arr(p, void *, 0);
	for (int i=0; mods[i] != NULL; i++) {
		if (mods[i]->data_init != NULL)
			tal_arr_expand(&p->modifier_data,
				       mods[i]->data_init(p));
		else
			tal_arr_expand(&p->modifier_data, NULL);
	}

	return p;
}

struct payment *payment_root(struct payment *p)
{
	if (p->parent == NULL)
		return p;
	else
		return payment_root(p->parent);
}

/* Generic handler for RPC failures that should end up failing the payment. */
static struct command_result *payment_rpc_failure(struct command *cmd,
						  const char *buffer,
						  const jsmntok_t *toks,
						  struct payment *p)
{
	plugin_log(p->plugin, LOG_DBG,
		   "Failing a partial payment due to a failed RPC call: %.*s",
		   toks->end - toks->start, buffer + toks->start);

	payment_fail(p);
	return command_still_pending(cmd);
}

struct payment_tree_result payment_collect_result(struct payment *p)
{
	struct payment_tree_result res;
	size_t numchildren = tal_count(p->children);
	res.sent = AMOUNT_MSAT(0);
	/* If we didn't have a route, we didn't attempt. */
	res.attempts = p->route == NULL ? 0 : 1;
	res.treestates = p->step;
	res.leafstates = 0;
	res.preimage = NULL;
	res.failure = NULL;
	if (p->step == PAYMENT_STEP_FAILED && p->result != NULL)
		res.failure = p->result;

	if (numchildren == 0) {
		res.leafstates |= p->step;
		if (p->result && p->result->state == PAYMENT_COMPLETE) {
			res.sent = p->result->amount_sent;
			res.preimage = p->result->payment_preimage;
		}
	}

	for (size_t i = 0; i < numchildren; i++) {
		struct payment_tree_result cres =
		    payment_collect_result(p->children[i]);

		/* Some of our subpayments have succeeded, aggregate how much
		 * we sent in total. */
		if (!amount_msat_add(&res.sent, res.sent, cres.sent))
			plugin_err(
			    p->plugin,
			    "Number overflow summing partial payments: %s + %s",
			    type_to_string(tmpctx, struct amount_msat,
					   &res.sent),
			    type_to_string(tmpctx, struct amount_msat,
					   &cres.sent));

		/* Bubble up the first preimage we see. */
		if (res.preimage == NULL && cres.preimage != NULL)
			res.preimage = cres.preimage;

		res.leafstates |= cres.leafstates;
		res.treestates |= cres.treestates;
		res.attempts += cres.attempts;

		/* We bubble the failure result with the highest failcode up
		 * to the root. */
		if (res.failure == NULL ||
		    (cres.failure != NULL &&
		     cres.failure->failcode > res.failure->failcode)) {
			res.failure = cres.failure;
		}
	}
	return res;
}

static struct command_result *payment_getinfo_success(struct command *cmd,
						      const char *buffer,
						      const jsmntok_t *toks,
						      struct payment *p)
{
	const jsmntok_t *blockheighttok =
	    json_get_member(buffer, toks, "blockheight");
	json_to_number(buffer, blockheighttok, &p->start_block);
	payment_continue(p);
	return command_still_pending(cmd);
}

void payment_start(struct payment *p)
{
	struct payment *root = payment_root(p);
	p->step = PAYMENT_STEP_INITIALIZED;
	p->current_modifier = -1;

	/* Pre-generate the getroute request, so modifiers can have their say,
	 * before we actually call `getroute` */
	p->getroute->destination = p->destination;
	p->getroute->max_hops = ROUTING_MAX_HOPS;
	if (root->invoice != NULL && root->invoice->min_final_cltv_expiry != 0)
		p->getroute->cltv = root->invoice->min_final_cltv_expiry;
	else
		p->getroute->cltv = DEFAULT_FINAL_CLTV_DELTA;
	p->getroute->amount = p->amount;

	/* TODO If this is not the root, we can actually skip the getinfo call
	 * and just reuse the parent's value. */
	send_outreq(p->plugin,
		    jsonrpc_request_start(p->plugin, NULL, "getinfo",
					  payment_getinfo_success,
					  payment_rpc_failure, p));
}

static bool route_hop_from_json(struct route_hop *dst, const char *buffer,
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

static struct route_hop *
tal_route_from_json(const tal_t *ctx, const char *buffer, const jsmntok_t *toks)
{
	size_t num = toks->size, i;
	struct route_hop *hops;
	const jsmntok_t *rtok;
	if (toks->type != JSMN_ARRAY)
		return NULL;

	hops = tal_arr(ctx, struct route_hop, num);
	json_for_each_arr(i, rtok, toks) {
		if (!route_hop_from_json(&hops[i], buffer, rtok))
			return tal_free(hops);
	}
	return hops;
}

static struct command_result *payment_getroute_result(struct command *cmd,
						      const char *buffer,
						      const jsmntok_t *toks,
						      struct payment *p)
{
	const jsmntok_t *rtok = json_get_member(buffer, toks, "route");
	struct amount_msat fee;
	assert(rtok != NULL);
	p->route = tal_route_from_json(p, buffer, rtok);
	p->step = PAYMENT_STEP_GOT_ROUTE;

	/* Ensure that our fee and CLTV budgets are respected. */

	if (!amount_msat_sub(&fee, p->route[0].amount, p->amount)) {
		plugin_err(
		    p->plugin,
		    "gossipd returned a route with a negative fee: sending %s "
		    "to deliver %s",
		    type_to_string(tmpctx, struct amount_msat,
				   &p->route[0].amount),
		    type_to_string(tmpctx, struct amount_msat, &p->amount));
		payment_fail(p);
		return command_still_pending(cmd);
	}

	if (amount_msat_greater(fee, p->fee_budget)) {
		plugin_log(p->plugin, LOG_INFORM,
		    "Fee exceeds our fee budget: %s > %s, discarding route",
		    type_to_string(tmpctx, struct amount_msat, &fee),
		    type_to_string(tmpctx, struct amount_msat, &p->fee_budget));
		payment_fail(p);
		return command_still_pending(cmd);
	}

	if (p->route[0].delay > p->cltv_budget) {
		plugin_log(p->plugin, LOG_INFORM,
			   "CLTV delay exceeds our CLTV budget: %d > %d",
			   p->route[0].delay, p->cltv_budget);
		payment_fail(p);
		return command_still_pending(cmd);
	}

	/* Allow modifiers to modify the route, before
	 * payment_compute_onion_payloads uses the route to generate the
	 * onion_payloads */
	payment_continue(p);
	return command_still_pending(cmd);
}

static struct command_result *payment_getroute_error(struct command *cmd,
						     const char *buffer,
						     const jsmntok_t *toks,
						     struct payment *p)
{
	p->route = NULL;
	payment_fail(p);

	/* Let payment_finished_ handle this, so we mark it as pending */
	return command_still_pending(cmd);
}

static const struct short_channel_id_dir *
payment_get_excluded_channels(const tal_t *ctx, struct payment *p)
{
	struct payment *root = payment_root(p);
	struct channel_hint *hint;
	struct short_channel_id_dir *res =
	    tal_arr(ctx, struct short_channel_id_dir, 0);
	for (size_t i = 0; i < tal_count(root->channel_hints); i++) {
		hint = &root->channel_hints[i];

		if (!hint->enabled)
			tal_arr_expand(&res, hint->scid);

		else if (amount_msat_greater_eq(p->amount,
						hint->estimated_capacity))
			tal_arr_expand(&res, hint->scid);
	}
	return res;
}

static const struct node_id *payment_get_excluded_nodes(const tal_t *ctx,
						  struct payment *p)
{
	struct payment *root = payment_root(p);
	return root->excluded_nodes;
}

/* Iterate through the channel_hints and exclude any channel that we are
 * confidet will not be able to handle this payment. */
static void payment_getroute_add_excludes(struct payment *p,
					  struct json_stream *js)
{
	const struct node_id *nodes;
	const struct short_channel_id_dir *chans;

	json_array_start(js, "exclude");

	/* Collect and exclude all channels that are disabled or we know have
	 * insufficient capacity. */
	chans = payment_get_excluded_channels(tmpctx, p);
	for (size_t i=0; i<tal_count(chans); i++)
		json_add_short_channel_id_dir(js, NULL, &chans[i]);

	/* Now also exclude nodes that we think have failed. */
	nodes = payment_get_excluded_nodes(tmpctx, p);
	for (size_t i=0; i<tal_count(nodes); i++)
		json_add_node_id(js, NULL, &nodes[i]);

	json_array_end(js);
}

static void payment_getroute(struct payment *p)
{
	struct out_req *req;
	req = jsonrpc_request_start(p->plugin, NULL, "getroute",
				    payment_getroute_result,
				    payment_getroute_error, p);
	json_add_node_id(req->js, "id", p->getroute->destination);
	json_add_amount_msat_only(req->js, "msatoshi", p->getroute->amount);
	json_add_num(req->js, "riskfactor", 1);
	json_add_num(req->js, "cltv", p->getroute->cltv);
	json_add_num(req->js, "maxhops", p->getroute->max_hops);
	payment_getroute_add_excludes(p, req->js);
	send_outreq(p->plugin, req);
}

static u8 *tal_towire_legacy_payload(const tal_t *ctx, const struct legacy_payload *payload)
{
	const u8 padding[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			      0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	/* Prepend 0 byte for realm */
	u8 *buf = tal_arrz(ctx, u8, 1);
	towire_short_channel_id(&buf, &payload->scid);
	towire_u64(&buf, payload->forward_amt.millisatoshis); /* Raw: low-level serializer */
	towire_u32(&buf, payload->outgoing_cltv);
	towire(&buf, padding, ARRAY_SIZE(padding));
	assert(tal_bytelen(buf) == 1 + 32);
	return buf;
}

static struct createonion_response *
tal_createonion_response_from_json(const tal_t *ctx, const char *buffer,
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

static struct payment_result *tal_sendpay_result_from_json(const tal_t *ctx,
						    const char *buffer,
						    const jsmntok_t *toks)
{
	const jsmntok_t *idtok = json_get_member(buffer, toks, "id");
	const jsmntok_t *hashtok = json_get_member(buffer, toks, "payment_hash");
	const jsmntok_t *partidtok = json_get_member(buffer, toks, "partid");
	const jsmntok_t *senttok = json_get_member(buffer, toks, "amount_sent_msat");
	const jsmntok_t *statustok = json_get_member(buffer, toks, "status");
	const jsmntok_t *preimagetok = json_get_member(buffer, toks, "payment_preimage");
	const jsmntok_t *codetok = json_get_member(buffer, toks, "code");
	const jsmntok_t *datatok = json_get_member(buffer, toks, "data");
	const jsmntok_t *erridxtok, *msgtok, *failcodetok, *rawmsgtok,
		*failcodenametok, *errchantok, *errnodetok, *errdirtok;
	struct payment_result *result;

	/* Check if we have an error and need to descend into data to get
	 * details. */
	if (codetok != NULL && datatok != NULL) {
		idtok = json_get_member(buffer, datatok, "id");
		hashtok = json_get_member(buffer, datatok, "payment_hash");
		partidtok = json_get_member(buffer, datatok, "partid");
		senttok = json_get_member(buffer, datatok, "amount_sent_msat");
		statustok = json_get_member(buffer, datatok, "status");
	}

	/* Initial sanity checks, all these fields must exist. */
	if (idtok == NULL || idtok->type != JSMN_PRIMITIVE ||
	    hashtok == NULL || hashtok->type != JSMN_STRING ||
	    senttok == NULL || senttok->type != JSMN_STRING ||
	    statustok == NULL || statustok->type != JSMN_STRING) {
		fprintf(stderr, "XXX missing field\n");
		return NULL;
	}

	result = tal(ctx, struct payment_result);

	if (codetok != NULL)
		json_to_u32(buffer, codetok, &result->code);
	else
		result->code = 0;

	/* If the partid is 0 it'd be omitted in waitsendpay, fix this here. */
	if (partidtok != NULL)
		json_to_u32(buffer, partidtok, &result->partid);
	else
		result->partid = 0;

	json_to_u64(buffer, idtok, &result->id);
	json_to_msat(buffer, senttok, &result->amount_sent);
	if (json_tok_streq(buffer, statustok, "pending")) {
		result->state = PAYMENT_PENDING;
	} else if (json_tok_streq(buffer, statustok, "complete")) {
		result->state = PAYMENT_COMPLETE;
	} else if (json_tok_streq(buffer, statustok, "failed")) {
		result->state = PAYMENT_FAILED;
	} else {
		goto fail;
	}

	if (preimagetok != NULL) {
		result->payment_preimage = tal(result, struct preimage);
		json_to_preimage(buffer, preimagetok, result->payment_preimage);
	}

	/* Now extract the error details if the error code is not 0 */
	if (result->code != 0) {
		erridxtok = json_get_member(buffer, datatok, "erring_index");
		errnodetok = json_get_member(buffer, datatok, "erring_node");
		errchantok = json_get_member(buffer, datatok, "erring_channel");
		errdirtok = json_get_member(buffer, datatok, "erring_direction");
		failcodetok = json_get_member(buffer, datatok, "failcode");
		failcodenametok =json_get_member(buffer, datatok, "failcodename");
		msgtok = json_get_member(buffer, toks, "message");
		rawmsgtok = json_get_member(buffer, datatok, "raw_message");
		if (failcodetok == NULL || failcodetok->type != JSMN_PRIMITIVE ||
		    failcodenametok == NULL || failcodenametok->type != JSMN_STRING ||
		    (erridxtok != NULL && erridxtok->type != JSMN_PRIMITIVE) ||
		    (errnodetok != NULL && errnodetok->type != JSMN_STRING) ||
		    (errchantok != NULL && errchantok->type != JSMN_STRING) ||
		    (errdirtok != NULL && errdirtok->type != JSMN_PRIMITIVE) ||
		    msgtok == NULL || msgtok->type != JSMN_STRING ||
		    (rawmsgtok != NULL && rawmsgtok->type != JSMN_STRING))
			goto fail;

		if (rawmsgtok != NULL)
			result->raw_message = json_tok_bin_from_hex(result, buffer, rawmsgtok);
		else
			result->raw_message = NULL;

		result->failcodename = json_strdup(result, buffer, failcodenametok);
		json_to_u32(buffer, failcodetok, &result->failcode);
		result->message = json_strdup(result, buffer, msgtok);

		if (erridxtok != NULL) {
			result->erring_index = tal(result, u32);
			json_to_u32(buffer, erridxtok, result->erring_index);
		} else {
			result->erring_index = NULL;
		}

		if (errdirtok != NULL) {
			result->erring_direction = tal(result, int);
			json_to_int(buffer, errdirtok, result->erring_direction);
		} else {
			result->erring_direction = NULL;
		}

		if (errnodetok != NULL) {
			result->erring_node = tal(result, struct node_id);
			json_to_node_id(buffer, errnodetok,
					result->erring_node);
		} else {
			result->erring_node = NULL;
		}

		if (errchantok != NULL) {
			result->erring_channel =
			    tal(result, struct short_channel_id);
			json_to_short_channel_id(buffer, errchantok,
						 result->erring_channel);
		} else {
			result->erring_channel = NULL;
		}
	}

	return result;
fail:
	return tal_free(result);
}

/* Try to infer the erring_node, erring_channel and erring_direction from what
 * we know, but don't override the values that are returned by `waitsendpay`.  */
static void payment_result_infer(struct route_hop *route,
				 struct payment_result *r)
{
	int i, len = tal_count(route);
	if (r->code == 0 || r->erring_index == NULL || route == NULL)
		return;

	i = *r->erring_index;
	assert(i <= len);

	if (r->erring_node == NULL)
		r->erring_node = &route[i-1].nodeid;

	/* The above assert was enough for the erring_node, but might be off
	 * by one on channel and direction, in case the destination failed on
	 * us. */
	if (i == len)
		return;

	if (r->erring_channel == NULL)
		r->erring_channel = &route[i].channel_id;

	if (r->erring_direction == NULL)
		r->erring_direction = &route[i].direction;
}

static struct command_result *
payment_waitsendpay_finished(struct command *cmd, const char *buffer,
			     const jsmntok_t *toks, struct payment *p)
{
	struct payment *root;
	struct channel_hint hint;
	struct route_hop *hop;
	assert(p->route != NULL);

	p->result = tal_sendpay_result_from_json(p, buffer, toks);
	assert(p->result != NULL);
	payment_result_infer(p->route, p->result);

	if (p->result->state == PAYMENT_COMPLETE) {
		p->step = PAYMENT_STEP_SUCCESS;
		p->end_time = time_now();
		payment_continue(p);
		return command_still_pending(cmd);
	}

	root = payment_root(p);

	switch (p->result->failcode) {
	case WIRE_PERMANENT_CHANNEL_FAILURE:
	case WIRE_CHANNEL_DISABLED:
	case WIRE_UNKNOWN_NEXT_PEER:
	case WIRE_REQUIRED_CHANNEL_FEATURE_MISSING:
		/* All of these result in the channel being marked as disabled. */
		assert(*p->result->erring_index < tal_count(p->route));
		hop = &p->route[*p->result->erring_index];
		hint.enabled = false;
		hint.scid.scid = hop->channel_id;
		hint.scid.dir = hop->direction;
		hint.estimated_capacity = AMOUNT_MSAT(0);
		tal_arr_expand(&root->channel_hints, hint);
		break;
	case WIRE_TEMPORARY_CHANNEL_FAILURE:
		/* These are an indication that the capacity was insufficient,
		 * remember the amount we tried as an estimate. */
		assert(*p->result->erring_index < tal_count(p->route));
		hop = &p->route[*p->result->erring_index];
		hint.enabled = true;
		hint.scid.scid = hop->channel_id;
		hint.scid.dir = hop->direction;
		hint.estimated_capacity.millisatoshis = hop->amount.millisatoshis * 0.75; /* Raw: Multiplication */
		tal_arr_expand(&root->channel_hints, hint);
		break;

	case WIRE_INVALID_ONION_PAYLOAD:
	case WIRE_INVALID_REALM:
	case WIRE_PERMANENT_NODE_FAILURE:
	case WIRE_TEMPORARY_NODE_FAILURE:
	case WIRE_REQUIRED_NODE_FEATURE_MISSING:
	case WIRE_INVALID_ONION_VERSION:
	case WIRE_INVALID_ONION_HMAC:
	case WIRE_INVALID_ONION_KEY:
#if EXPERIMENTAL_FEATURES
	case WIRE_INVALID_ONION_BLINDING:
#endif
		/* These are reported by the last hop, i.e., the destination of hop i-1. */
		assert(*p->result->erring_index - 1 < tal_count(p->route));
		hop = &p->route[*p->result->erring_index - 1];
		tal_arr_expand(&root->excluded_nodes, hop->nodeid);
		break;

 	case WIRE_INCORRECT_OR_UNKNOWN_PAYMENT_DETAILS:
	case WIRE_MPP_TIMEOUT:
		/* These are permanent failures that should abort all of our
		 * attempts right away. We'll still track pending partial
		 * payments correctly, just not start new ones. */
		root->abort = true;
		break;

	case WIRE_AMOUNT_BELOW_MINIMUM:
	case WIRE_EXPIRY_TOO_FAR:
	case WIRE_EXPIRY_TOO_SOON:
	case WIRE_FEE_INSUFFICIENT:
	case WIRE_INCORRECT_CLTV_EXPIRY:
	case WIRE_FINAL_INCORRECT_CLTV_EXPIRY:
		/* These are issues that are due to gossipd being out of date,
		 * we ignore them here, and wait for gossipd to adjust
		 * instead. */
		break;
	case WIRE_FINAL_INCORRECT_HTLC_AMOUNT:
		/* These are symptoms of intermediate hops tampering with the
		 * payment. */
		break;
	}

	payment_fail(p);
	return command_still_pending(cmd);
}

static struct command_result *payment_sendonion_success(struct command *cmd,
							  const char *buffer,
							  const jsmntok_t *toks,
							  struct payment *p)
{
	struct out_req *req;
	req = jsonrpc_request_start(p->plugin, NULL, "waitsendpay",
				    payment_waitsendpay_finished,
				    payment_waitsendpay_finished, p);
	json_add_sha256(req->js, "payment_hash", p->payment_hash);
	json_add_num(req->js, "partid", p->partid);
	send_outreq(p->plugin, req);

	return command_still_pending(cmd);
}

static struct command_result *payment_createonion_success(struct command *cmd,
							  const char *buffer,
							  const jsmntok_t *toks,
							  struct payment *p)
{
	struct out_req *req;
	struct route_hop *first = &p->route[0];
	struct secret *secrets;
	p->createonion_response = tal_createonion_response_from_json(p, buffer, toks);

	req = jsonrpc_request_start(p->plugin, NULL, "sendonion",
				    payment_sendonion_success,
				    payment_rpc_failure, p);
	json_add_hex_talarr(req->js, "onion", p->createonion_response->onion);

	json_object_start(req->js, "first_hop");
	json_add_short_channel_id(req->js, "channel", &first->channel_id);
	json_add_num(req->js, "direction", first->direction);
	json_add_amount_msat_only(req->js, "amount_msat", first->amount);
	json_add_num(req->js, "delay", first->delay);
	json_add_node_id(req->js, "id", &first->nodeid);
	json_object_end(req->js);

	json_add_sha256(req->js, "payment_hash", p->payment_hash);

	json_array_start(req->js, "shared_secrets");
	secrets = p->createonion_response->shared_secrets;
	for(size_t i=0; i<tal_count(secrets); i++)
		json_add_secret(req->js, NULL, &secrets[i]);
	json_array_end(req->js);

	json_add_num(req->js, "partid", p->partid);

	send_outreq(p->plugin, req);
	return command_still_pending(cmd);
}

/* Temporary serialization method for the tlv_payload.data until we rework the
 * API that is generated from the specs to use the setter/getter interface. */
static void tlvstream_set_tlv_payload_data(struct tlv_field **stream,
					   struct secret *payment_secret,
					   u64 total_msat)
{
		u8 *ser = tal_arr(NULL, u8, 0);
		towire_secret(&ser, payment_secret);
		towire_tu64(&ser, total_msat);
		tlvstream_set_raw(stream, TLV_TLV_PAYLOAD_PAYMENT_DATA,
				  take(ser));
}

static void payment_compute_onion_payloads(struct payment *p)
{
	struct createonion_request *cr;
	size_t hopcount;
	static struct short_channel_id all_zero_scid;
	struct createonion_hop *cur;
	struct payment *root = payment_root(p);
	p->step = PAYMENT_STEP_ONION_PAYLOAD;
	hopcount = tal_count(p->route);

	/* Now compute the payload we're about to pass to `createonion` */
	cr = p->createonion_request = tal(p, struct createonion_request);
	cr->assocdata = tal_arr(cr, u8, 0);
	towire_sha256(&cr->assocdata, p->payment_hash);
	cr->session_key = NULL;
	cr->hops = tal_arr(cr, struct createonion_hop, tal_count(p->route));

	/* Non-final hops */
	for (int i = 0; i < hopcount - 1; i++) {
		/* The message is destined for hop i, but contains fields for
		 * i+1 */
		cur = &cr->hops[i];
		cur->style = p->route[i].style;
		cur->pubkey = p->route[i].nodeid;
		switch (cur->style) {
		case ROUTE_HOP_LEGACY:
			cur->legacy_payload =
			    tal(cr->hops, struct legacy_payload);
			cur->legacy_payload->forward_amt =
			    p->route[i + 1].amount;
			cur->legacy_payload->scid = p->route[i + 1].channel_id;
			cur->legacy_payload->outgoing_cltv =
			    p->start_block + p->route[i + 1].delay;
			break;
		case ROUTE_HOP_TLV:
			cur->tlv_payload = tlv_tlv_payload_new(cr->hops);
			tlvstream_set_tu64(
			    &cur->tlv_payload->fields,
			    TLV_TLV_PAYLOAD_AMT_TO_FORWARD,
			    p->route[i + 1].amount.millisatoshis); /* Raw: TLV payload generation*/
			tlvstream_set_tu32(&cur->tlv_payload->fields,
					   TLV_TLV_PAYLOAD_OUTGOING_CLTV_VALUE,
					   p->start_block +
					       p->route[i + 1].delay);
			tlvstream_set_short_channel_id(
			    &cur->tlv_payload->fields,
			    TLV_TLV_PAYLOAD_SHORT_CHANNEL_ID,
			    &p->route[i + 1].channel_id);
		}
	}

	/* Final hop */
	cur = &cr->hops[hopcount - 1];
	cur->style = p->route[hopcount - 1].style;
	cur->pubkey = p->route[hopcount - 1].nodeid;
	switch (cur->style) {
	case ROUTE_HOP_LEGACY:
		cur->legacy_payload = tal(cr->hops, struct legacy_payload);
		cur->legacy_payload->forward_amt =
		    p->route[hopcount - 1].amount;
		cur->legacy_payload->scid = all_zero_scid;
		cur->legacy_payload->outgoing_cltv =
		    p->start_block + p->route[hopcount - 1].delay;
		break;
	case ROUTE_HOP_TLV:
		cur->tlv_payload = tlv_tlv_payload_new(cr->hops);
		tlvstream_set_tu64(&cur->tlv_payload->fields,
				   TLV_TLV_PAYLOAD_AMT_TO_FORWARD,
				   p->route[hopcount - 1].amount.millisatoshis); /* Raw: TLV payload generation*/
		tlvstream_set_tu32(&cur->tlv_payload->fields,
				   TLV_TLV_PAYLOAD_OUTGOING_CLTV_VALUE,
				   p->start_block +
				       p->route[hopcount - 1].delay);

		if (root->payment_secret != NULL)
			tlvstream_set_tlv_payload_data(
			    &cur->tlv_payload->fields, root->payment_secret,
			    root->amount.millisatoshis);  /* Raw: TLV payload generation*/
	}

	/* Now allow all the modifiers to mess with the payloads, before we
	 * serialize via a call to createonion in the next step. */
	payment_continue(p);
}

static void payment_sendonion(struct payment *p)
{
	struct out_req *req;
	u8 *payload, *tlv;
	req = jsonrpc_request_start(p->plugin, NULL, "createonion",
				    payment_createonion_success,
				    payment_rpc_failure, p);

	json_array_start(req->js, "hops");
	for (int i = 0; i < tal_count(p->createonion_request->hops); i++) {
		json_object_start(req->js, NULL);
		struct createonion_hop *hop = &p->createonion_request->hops[i];
		json_add_node_id(req->js, "pubkey", &hop->pubkey);
		if (hop->style == ROUTE_HOP_LEGACY) {
			payload = tal_towire_legacy_payload(tmpctx, hop->legacy_payload);
			json_add_hex_talarr(req->js, "payload", payload);
		}else {
			tlv = tal_arr(tmpctx, u8, 0);
			towire_tlvstream_raw(&tlv, hop->tlv_payload->fields);
			payload = tal_arr(tmpctx, u8, 0);
			towire_bigsize(&payload, tal_bytelen(tlv));
			towire(&payload, tlv, tal_bytelen(tlv));
			json_add_hex_talarr(req->js, "payload", payload);
			tal_free(tlv);
		}
		tal_free(payload);
		json_object_end(req->js);
	}
	json_array_end(req->js);

	json_add_hex_talarr(req->js, "assocdata",
			    p->createonion_request->assocdata);

	if (p->createonion_request->session_key)
		json_add_secret(req->js, "sessionkey",
				p->createonion_request->session_key);

	send_outreq(p->plugin, req);
}

/* Mutual recursion. */
static struct command_result *payment_finished(struct payment *p);

/* A payment is finished if a) it is in a final state, of b) it's in a
 * child-spawning state and all of its children are in a final state. */
static bool payment_is_finished(struct payment *p)
{
	bool running_children = false;
	if (p->step == PAYMENT_STEP_FAILED || p->step == PAYMENT_STEP_SUCCESS)
		return true;
	else if(p->step == PAYMENT_STEP_SPLIT || p->step == PAYMENT_STEP_RETRY) {
		for (size_t i = 0; i < tal_count(p->children); i++)
			running_children |= !payment_is_finished(p->children[i]);
		return !running_children;
	} else {
		return false;
	}
}

static enum payment_step payment_aggregate_states(struct payment *p)
{
	enum payment_step agg = p->step;

	for (size_t i=0; i<tal_count(p->children); i++)
		agg |= payment_aggregate_states(p->children[i]);

	return agg;
}

/* A payment is finished if a) it is in a final state, of b) it's in a
 * child-spawning state and all of its children are in a final state. */
static bool payment_is_success(struct payment *p)
{
	return (payment_aggregate_states(p) & PAYMENT_STEP_SUCCESS) != 0;
}

/* Function to bubble up completions to the root, which actually holds on to
 * the command that initiated the flow. */
static struct command_result *payment_child_finished(struct payment *p,
						     struct payment *child)
{
	if (!payment_is_finished(p))
		return command_still_pending(NULL);

	/* Should we continue bubbling up? */
	return payment_finished(p);
}

/* This function is called whenever a payment ends up in a final state, or all
 * leafs in the subtree rooted in the payment are all in a final state. It is
 * called only once, and it is guaranteed to be called in post-order
 * traversal, i.e., all children are finished before the parent is called. */
static struct command_result *payment_finished(struct payment *p)
{
	struct payment_tree_result result = payment_collect_result(p);
	struct json_stream *ret;
	struct command *cmd = p->cmd;

	/* Either none of the leaf attempts succeeded yet, or we have a
	 * preimage. */
	assert((result.leafstates & PAYMENT_STEP_SUCCESS) == 0 ||
	       result.preimage != NULL);

	if (p->parent == NULL && cmd == NULL) {
		/* This is the tree root, but we already reported success or
		 * failure, so noop. */
		return command_still_pending(NULL);

	}  else if (p->parent == NULL) {
		if (payment_is_success(p)) {
			assert(result.treestates & PAYMENT_STEP_SUCCESS);
			assert(result.leafstates & PAYMENT_STEP_SUCCESS);
			assert(result.preimage != NULL);

			ret = jsonrpc_stream_success(p->cmd);
			json_add_sha256(ret, "payment_hash", p->payment_hash);
			json_add_num(ret, "parts", result.attempts);

			json_add_amount_msat_compat(ret, p->amount, "msatoshi",
						    "amount_msat");
			json_add_amount_msat_compat(ret, result.sent,
						    "msatoshi_sent",
						    "amount_sent_msat");

			if (result.leafstates != PAYMENT_STEP_SUCCESS)
				json_add_string(
				    ret, "warning",
				    "Some parts of the payment are not yet "
				    "completed, but we have the confirmation "
				    "from the recipient.");
			json_add_preimage(ret, "payment_preimage", result.preimage);

			json_add_string(ret, "status", "complete");

			/* Unset the pointer to the cmd so we don't attempt to
			 * return a response twice. */
			p->cmd = NULL;
			return command_finished(cmd, ret);
		} else if (result.failure == NULL || result.failure->failcode < NODE) {
			/* This is failing because we have no more routes to try */
			ret = jsonrpc_stream_fail(cmd, PAY_ROUTE_NOT_FOUND,
						  NULL);
			json_add_string(
			    ret, "message",
			    tal_fmt(cmd,
				    "Ran out of routes to try after "
				    "%d attempt%s: see paystatus",
				    result.attempts,
				    result.attempts == 1 ? "" : "s"));
			json_add_num(ret, "attempts", result.attempts);
			return command_finished(cmd, ret);

		}  else {
			struct payment_result *failure = result.failure;
			assert(failure!= NULL);
			ret = jsonrpc_stream_fail(cmd, p->result->code,
						  failure->message);

			json_add_u64(ret, "id", p->result->id);

			json_add_u32(ret, "failcode", result.failure->failcode);
			json_add_string(ret, "failcodename",
					result.failure->failcodename);
			json_add_num(ret, "code", result.failure->code);

			json_object_start(ret, "data");
			if (p->bolt11)
				json_add_string(ret, "bolt11", p->bolt11);

			json_add_hex_talarr(ret, "raw_message",
					    p->result->raw_message);
			json_add_num(ret, "created_at", p->start_time.ts.tv_sec);
			json_add_string(ret, "message", p->result->message);
			json_add_node_id(ret, "destination", p->destination);
			json_add_sha256(ret, "payment_hash", p->payment_hash);

			if (result.leafstates & PAYMENT_STEP_SUCCESS) {
				/* If one sub-payment succeeded then we have
				 * proof of payment, and the payment is a
				 * success. */
				json_add_string(ret, "status", "complete");

			} else if (result.leafstates & ~PAYMENT_FAILED) {
				/* If there are non-failed leafs we are still trying. */
				json_add_string(ret, "status", "pending");

			} else {
				json_add_string(ret, "status", "failed");
			}

			json_add_amount_msat_compat(ret, p->amount, "msatoshi",
						    "amount_msat");

			json_add_amount_msat_compat(ret, result.sent,
						    "msatoshi_sent",
						    "amount_sent_msat");

			if (failure != NULL) {
				if (failure->erring_index)
					json_add_num(ret, "erring_index",
						     *failure->erring_index);

				if (failure->erring_node)
					json_add_node_id(ret, "erring_node",
							 failure->erring_node);

				if (failure->erring_channel)
					json_add_short_channel_id(
					    ret, "erring_channel",
					    failure->erring_channel);

				if (failure->erring_direction)
					json_add_num(
					    ret, "erring_direction",
					    *failure->erring_direction);
			}
			json_object_end(ret);

			return command_finished(cmd, ret);
		}
	} else {
		return payment_child_finished(p->parent, p);
	}
}

void payment_continue(struct payment *p)
{
	struct payment_modifier *mod;
	void *moddata;
	/* If we are in the middle of calling the modifiers, continue calling
	 * them, otherwise we can continue with the payment state-machine. */
	p->current_modifier++;
	mod = p->modifiers[p->current_modifier];

	if (mod != NULL) {
		/* There is another modifier, so call it. */
		moddata = p->modifier_data[p->current_modifier];
		return mod->post_step_cb(moddata, p);
	} else {
		/* There are no more modifiers, so reset the call chain and
		 * proceed to the next state. */
		p->current_modifier = -1;
		switch (p->step) {
		case PAYMENT_STEP_INITIALIZED:
			payment_getroute(p);
			return;

		case PAYMENT_STEP_GOT_ROUTE:
			payment_compute_onion_payloads(p);
			return;

		case PAYMENT_STEP_ONION_PAYLOAD:
			payment_sendonion(p);
			return;

		case PAYMENT_STEP_SUCCESS:
		case PAYMENT_STEP_FAILED:
			payment_finished(p);
			return;

		case PAYMENT_STEP_RETRY:
		case PAYMENT_STEP_SPLIT:
			/* Do nothing, we'll get pinged by a child succeeding
			 * or failing. */
			return;
		}
	}
	/* We should never get here, it'd mean one of the state machine called
	 * `payment_continue` after the final state. */
	abort();
}

void payment_fail(struct payment *p)
{
	p->end_time = time_now();
	p->step = PAYMENT_STEP_FAILED;
	payment_continue(p);
}

void *payment_mod_get_data(const struct payment *p,
			   const struct payment_modifier *mod)
{
	for (int i = 0; p->modifiers[i] != NULL; i++)
		if (p->modifiers[i] == mod)
			return p->modifier_data[i];

	/* If we ever get here it means that we asked for the data for a
	 * non-existent modifier. This is a compile-time/wiring issue, so we
	 * better check that modifiers match the data we ask for. */
	abort();
}

static struct retry_mod_data *retry_data_init(struct payment *p);

static inline void retry_step_cb(struct retry_mod_data *rd,
				 struct payment *p);

static struct retry_mod_data *
retry_data_init(struct payment *p)
{
	struct retry_mod_data *rdata = tal(p, struct retry_mod_data);
	struct retry_mod_data *parent_rdata;
	if (p->parent != NULL) {
		parent_rdata = payment_mod_retry_get_data(p->parent);
		rdata->retries = parent_rdata->retries - 1;
	} else {
		rdata->retries = 10;
	}
	return rdata;
}

/* Determine whether retrying could possibly succeed. */
static bool payment_can_retry(struct payment *p)
{
	struct payment_result *res = p->result;
	u32 idx;
	bool is_final;

	if (p->result == NULL)
		return false;

	idx = res->erring_index != NULL ? *res->erring_index : 0;
	is_final = (idx == tal_count(p->route));

	/* Full matrix of failure code x is_final. Prefer to retry once too
	 * often over eagerly failing. */
	switch (res->failcode) {
	case WIRE_EXPIRY_TOO_FAR:
	case WIRE_INCORRECT_OR_UNKNOWN_PAYMENT_DETAILS:
	case WIRE_INVALID_ONION_PAYLOAD:
	case WIRE_INVALID_ONION_VERSION:
	case WIRE_INVALID_REALM:
	case WIRE_MPP_TIMEOUT:
	case WIRE_PERMANENT_NODE_FAILURE:
	case WIRE_REQUIRED_NODE_FEATURE_MISSING:
	case WIRE_TEMPORARY_NODE_FAILURE:
	case WIRE_UNKNOWN_NEXT_PEER:
		return !is_final;

	case WIRE_AMOUNT_BELOW_MINIMUM:
	case WIRE_CHANNEL_DISABLED:
	case WIRE_EXPIRY_TOO_SOON:
	case WIRE_FEE_INSUFFICIENT:
	case WIRE_FINAL_INCORRECT_CLTV_EXPIRY:
	case WIRE_FINAL_INCORRECT_HTLC_AMOUNT:
	case WIRE_INCORRECT_CLTV_EXPIRY:
	case WIRE_INVALID_ONION_HMAC:
	case WIRE_INVALID_ONION_KEY:
	case WIRE_PERMANENT_CHANNEL_FAILURE:
	case WIRE_REQUIRED_CHANNEL_FEATURE_MISSING:
	case WIRE_TEMPORARY_CHANNEL_FAILURE:
#if EXPERIMENTAL_FEATURES
	case WIRE_INVALID_ONION_BLINDING:
#endif
		return true;
	}

	/* We should never get here, otherwise the above `switch` isn't
	 * exhaustive. Nevertheless the failcode is provided by the erring
	 * node, so retry anyway. `abort()`ing on externally supplied info is
	 * not a good idea. */
	return true;
}

static inline void retry_step_cb(struct retry_mod_data *rd,
				 struct payment *p)
{
	struct payment *subpayment;
	struct retry_mod_data *rdata = payment_mod_retry_get_data(p);

	if (p->step != PAYMENT_STEP_FAILED)
		return payment_continue(p);

	/* If we failed to find a route, it's unlikely we can suddenly find a
	 * new one without any other changes, so it's time to give up. */
	if (p->route == NULL)
		return payment_continue(p);

	/* If the root is marked as abort, we do not retry anymore */
	if (payment_root(p)->abort)
		return payment_continue(p);

	if (!payment_can_retry(p))
		return payment_continue(p);

	/* If the failure was not final, and we tried a route, try again. */
	if (rdata->retries > 0) {
		subpayment = payment_new(p, NULL, p, p->modifiers);
		payment_start(subpayment);
		p->step = PAYMENT_STEP_RETRY;
		subpayment->why =
		    tal_fmt(subpayment, "Still have %d attempts left",
			    rdata->retries - 1);
	}

	payment_continue(p);
}

REGISTER_PAYMENT_MODIFIER(retry, struct retry_mod_data *, retry_data_init,
			  retry_step_cb);

static struct command_result *
local_channel_hints_listpeers(struct command *cmd, const char *buffer,
			      const jsmntok_t *toks, struct payment *p)
{
	const jsmntok_t *peers, *peer, *channels, *channel, *spendsats, *scid, *dir, *connected;
	size_t i, j;
	peers = json_get_member(buffer, toks, "peers");

	if (peers == NULL)
		goto done;
        /* cppcheck-suppress uninitvar - cppcheck can't undestand these macros. */
	json_for_each_arr(i, peer, peers) {
		channels = json_get_member(buffer, peer, "channels");
		if (channels == NULL)
			continue;

		connected = json_get_member(buffer, peer, "connected");

		json_for_each_arr(j, channel, channels) {
			struct channel_hint h;
			spendsats = json_get_member(buffer, channel, "spendable_msat");
			scid = json_get_member(buffer, channel, "short_channel_id");
			dir = json_get_member(buffer, channel, "direction");
			assert(spendsats != NULL && scid != NULL && dir != NULL);

			json_to_bool(buffer, connected, &h.enabled);
			json_to_short_channel_id(buffer, scid, &h.scid.scid);
			json_to_int(buffer, dir, &h.scid.dir);

			json_to_msat(buffer, spendsats, &h.estimated_capacity);
			tal_arr_expand(&p->channel_hints, h);
		}
	}

done:
	payment_continue(p);
	return command_still_pending(cmd);
}

static void local_channel_hints_cb(void *d UNUSED, struct payment *p)
{
	struct out_req *req;
	/* If we are not the root we don't look up the channel balances since
	 * it is unlikely that the capacities have changed much since the root
	 * payment looked at them. We also only call `listpeers` when the
	 * payment is in state PAYMENT_STEP_INITIALIZED, right before calling
	 * `getroute`. */
	if (p->parent != NULL || p->step != PAYMENT_STEP_INITIALIZED)
		return payment_continue(p);

	req = jsonrpc_request_start(p->plugin, NULL, "listpeers",
				    local_channel_hints_listpeers,
				    local_channel_hints_listpeers, p);
	send_outreq(p->plugin, req);
}

REGISTER_PAYMENT_MODIFIER(local_channel_hints, void *, NULL, local_channel_hints_cb);

/* Trim route to this length by taking from the *front* of route
 * (end points to destination, so we need that bit!) */
static void trim_route(struct route_info **route, size_t n)
{
	size_t remove = tal_count(*route) - n;
	memmove(*route, *route + remove, sizeof(**route) * n);
	tal_resize(route, n);
}

/* Make sure routehints are reasonable length, and (since we assume we
 * can append), not directly to us.  Note: untrusted data! */
static struct route_info **filter_routehints(struct routehints_data *d,
					     struct node_id *myid,
					     struct route_info **hints)
{
	char *mods = tal_strdup(tmpctx, "");
	for (size_t i = 0; i < tal_count(hints); i++) {
		/* Trim any routehint > 10 hops */
		size_t max_hops = ROUTING_MAX_HOPS / 2;
		if (tal_count(hints[i]) > max_hops) {
			tal_append_fmt(&mods,
				       "Trimmed routehint %zu (%zu hops) to %zu. ",
				       i, tal_count(hints[i]), max_hops);
			trim_route(&hints[i], max_hops);
		}

		/* If we are first hop, trim. */
		if (tal_count(hints[i]) > 0
		    && node_id_eq(&hints[i][0].pubkey, myid)) {
			tal_append_fmt(&mods,
				       "Removed ourselves from routehint %zu. ",
				       i);
			trim_route(&hints[i], tal_count(hints[i])-1);
		}

		/* If route is empty, remove altogether. */
		if (tal_count(hints[i]) == 0) {
			tal_append_fmt(&mods,
				       "Removed empty routehint %zu. ", i);
			tal_arr_remove(&hints, i);
			i--;
		}
	}

	if (!streq(mods, ""))
		d->routehint_modifications = tal_steal(d, mods);

	return tal_steal(d, hints);
}

static bool routehint_excluded(struct payment *p,
			       const struct route_info *routehint)
{
	const struct node_id *nodes = payment_get_excluded_nodes(tmpctx, p);
	const struct short_channel_id_dir *chans =
	    payment_get_excluded_channels(tmpctx, p);

	/* Note that we ignore direction here: in theory, we could have
	 * found that one direction of a channel is unavailable, but they
	 * are suggesting we use it the other way.  Very unlikely though! */
	for (size_t i = 0; i < tal_count(routehint); i++) {
		const struct route_info *r = &routehint[i];
		for (size_t j=0; tal_count(nodes); j++)
			if (node_id_eq(&r->pubkey, &nodes[j]))
			    return true;

		for (size_t j = 0; j < tal_count(chans); j++)
			if (short_channel_id_eq(&chans[j].scid, &r->short_channel_id))
				return true;
	}
	return false;
}

static struct route_info *next_routehint(struct routehints_data *d,
					     struct payment *p)
{
	while (tal_count(d->routehints) > 0) {
		if (!routehint_excluded(p, d->routehints[0])) {
			d->current_routehint = d->routehints[0];
			tal_arr_remove(&d->routehints, 0);
			return d->current_routehint;
		}
		tal_free(d->routehints[0]);
		tal_arr_remove(&d->routehints, 0);
	}
	return NULL;
}

/* Calculate how many millisatoshi we need at the start of this route
 * to get msatoshi to the end. */
static bool route_msatoshi(struct amount_msat *total,
			   const struct amount_msat msat,
			   const struct route_info *route, size_t num_route)
{
	*total = msat;
	for (ssize_t i = num_route - 1; i >= 0; i--) {
		if (!amount_msat_add_fee(total,
					 route[i].fee_base_msat,
					 route[i].fee_proportional_millionths))
			return false;
	}
	return true;
}

/* The pubkey to use is the destination of this routehint. */
static const struct node_id *route_pubkey(const struct payment *p,
					  const struct route_info *routehint,
					  size_t n)
{
	if (n == tal_count(routehint))
		return p->destination;
	return &routehint[n].pubkey;
}

static u32 route_cltv(u32 cltv,
		      const struct route_info *route, size_t num_route)
{
	for (size_t i = 0; i < num_route; i++)
		cltv += route[i].cltv_expiry_delta;
	return cltv;
}

static void routehint_step_cb(struct routehints_data *d, struct payment *p)
{
	struct routehints_data *pd;
	struct route_hop hop;
	const struct payment *root = payment_root(p);

	if (p->step == PAYMENT_STEP_INITIALIZED) {
		if (root->invoice == NULL || root->invoice->routes == NULL)
			return payment_continue(p);

		/* The root payment gets the unmodified routehints, children may
		 * start dropping some as they learn that they were not
		 * functional. */
		if (p->parent == NULL) {
			d->routehints = filter_routehints(d, p->local_id,
							  p->invoice->routes);
		} else {
			pd = payment_mod_get_data(p->parent,
						  &routehints_pay_mod);
			d->routehints = tal_dup_talarr(d, struct route_info *,
						       pd->routehints);
		}
		d->current_routehint = next_routehint(d, p);

		if (d->current_routehint != NULL) {
			/* Change the destination and compute the final msatoshi
			 * amount to send to the routehint entry point. */
			if (!route_msatoshi(&p->getroute->amount, p->amount,
				    d->current_routehint,
				    tal_count(d->current_routehint))) {
			}
			d->final_cltv = p->getroute->cltv;
			p->getroute->destination = &d->current_routehint[0].pubkey;
			p->getroute->cltv =
			    route_cltv(p->getroute->cltv, d->current_routehint,
				       tal_count(d->current_routehint));
		}
	} else if (p->step == PAYMENT_STEP_GOT_ROUTE) {
		/* Now it's time to stitch the two partial routes together. */
		struct amount_msat dest_amount;
		struct route_info *routehint = d->current_routehint;
		struct route_hop *prev_hop;
		for (ssize_t i = 0; i < tal_count(routehint); i++) {
			prev_hop = &p->route[tal_count(p->route)-1];
			if (!route_msatoshi(&dest_amount, p->amount,
				    routehint + i + 1,
					    tal_count(routehint) - i - 1)) {
				/* Just let it fail, since we couldn't stitch
				 * the routes together. */
				return payment_continue(p);
			}

			hop.nodeid = *route_pubkey(p, routehint, i + 1);
			hop.style = ROUTE_HOP_TLV;
			hop.channel_id = routehint[i].short_channel_id;
			hop.amount = dest_amount;
			hop.delay = route_cltv(d->final_cltv, routehint + i + 1,
					       tal_count(routehint) - i - 1);

			/* Should we get a failure inside the routehint we'll
			 * need the direction so we can exclude it. Luckily
			 * it's rather easy to compute given the two
			 * subsequent hops. */
			hop.direction =
			    node_id_cmp(&prev_hop->nodeid, &hop.nodeid) > 0 ? 1
									    : 0;
			tal_arr_expand(&p->route, hop);
		}
	}

	payment_continue(p);
}

static struct routehints_data *routehint_data_init(struct payment *p)
{
	/* We defer the actual initialization to the step callback when we have
	 * the invoice attached. */
	return talz(p, struct routehints_data);
}

REGISTER_PAYMENT_MODIFIER(routehints, struct routehints_data *,
			  routehint_data_init, routehint_step_cb);
