#ifndef LIGHTNING_COMMON_RPCCLIENT_H
#define LIGHTNING_COMMON_RPCCLIENT_H

#include "config.h"
#include <bitcoin/privkey.h>
#include <common/features.h>
#include <common/json_helpers.h>
#include <common/node_id.h>
#include <common/wireaddr.h>

struct listpeers_channel {
	bool private;
	struct bitcoin_txid funding_txid;
	const char *state;
	struct short_channel_id *scid;
	int *direction;
	struct amount_msat total_msat;
	struct amount_msat spendable_msat;
	/* TODO Add fields as we need them. */
};

struct listpeers_peer {
	struct node_id id;
	bool connected;
	const char **netaddr;
	struct feature_set *features;
	struct listpeers_channel **channels;
};

struct listpeers_result {
	struct listpeers_peer **peers;
};

struct listpeers_result *json_to_listpeers_result(const tal_t *ctx,
						  const char *buffer,
						  const jsmntok_t *tok);

struct createonion_response {
	u8 *onion;
	struct secret *shared_secrets;
};

struct createonion_response *json_to_createonion_response(const tal_t *ctx,
							  const char *buffer,
							  const jsmntok_t *toks);

enum route_hop_style {
	ROUTE_HOP_LEGACY = 1,
	ROUTE_HOP_TLV = 2,
};

struct route_hop {
	struct short_channel_id channel_id;
	int direction;
	struct node_id nodeid;
	struct amount_msat amount;
	u32 delay;
	struct pubkey *blinding;
	enum route_hop_style style;
};

struct route_hop *json_to_route(const tal_t *ctx, const char *buffer,
				const jsmntok_t *toks);
#endif /* LIGHTNING_COMMON_RPCCLIENT_H */
