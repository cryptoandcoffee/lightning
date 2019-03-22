#include <assert.h>
#include <bitcoin/block.h>
#include <bitcoin/pullpush.h>
#include <bitcoin/tx.h>
#include <ccan/cast/cast.h>
#include <ccan/crypto/sha256/sha256.h>
#include <ccan/endian/endian.h>
#include <ccan/mem/mem.h>
#include <ccan/read_write_all/read_write_all.h>
#include <ccan/str/hex/hex.h>
#include <common/type_to_string.h>
#include <stdio.h>

#define SEGREGATED_WITNESS_FLAG 0x1

int bitcoin_tx_add_output(struct bitcoin_tx *tx, u8 *script,
			  struct amount_sat *amount)
{
	size_t i = tx->used_outputs;
	struct wally_tx_output *output;
	assert(i < tal_count(tx->output));
	assert(memeqzero(&tx->output[i], sizeof(struct bitcoin_tx_output)));

	tx->output[i].amount = *amount;
	tx->output[i].script = script;

	assert(tx->wtx != NULL);
	wally_tx_output_init_alloc(amount->satoshis /* Raw: low-level helper */,
				   script, tal_bytelen(script), &output);
	wally_tx_add_output(tx->wtx, output);
	wally_tx_output_free(output);

	tx->used_outputs++;
	return i;
}

int bitcoin_tx_add_input(struct bitcoin_tx *tx, const struct bitcoin_txid *txid,
			 u32 outnum, u32 sequence,
			 const struct amount_sat *amount, u8 *script)
{
	size_t i = tx->used_inputs;
	struct wally_tx_input *input;
	assert(i < tal_count(tx->input));
	assert(memeqzero(&tx->input[i].txid, sizeof(struct bitcoin_txid)));

	tx->input[i].txid = *txid;
	tx->input[i].index = outnum;
	tx->input[i].sequence_number = sequence;
	tx->input[i].script = script;

	assert(tx->wtx != NULL);
	wally_tx_input_init_alloc(txid->shad.sha.u.u8,
				  sizeof(struct bitcoin_txid), outnum, sequence,
				  script, tal_bytelen(script),
				  NULL /* Empty witness stack */, &input);
	wally_tx_add_input(tx->wtx, input);
	wally_tx_input_free(input);

	/* Now store the input amount if we know it, so we can sign later */
	tx->input_amounts[i] = tal_free(tx->input_amounts[i]);
	tx->input_amounts[i] = tal_dup(tx, struct amount_sat, amount);

	tx->used_inputs++;
	return i;
}

bool bitcoin_tx_check(const struct bitcoin_tx *tx)
{
	u8 *oldtx = linearize_tx(tmpctx, tx);
	u8 *newtx;
	size_t written;

	if (wally_tx_get_length(tx->wtx, WALLY_TX_FLAG_USE_WITNESS, &written) !=
	    WALLY_OK)
		return false;

	newtx = tal_arr(tmpctx, u8, written);
	if (wally_tx_to_bytes(tx->wtx, WALLY_TX_FLAG_USE_WITNESS, newtx, written,
			      &written) != WALLY_OK)
		return false;

	if (written != tal_bytelen(newtx))
		return false;

	return memeq(oldtx, tal_bytelen(oldtx), newtx, tal_bytelen(newtx));
}

void bitcoin_tx_output_set_amount(struct bitcoin_tx *tx, int outnum,
				  struct amount_sat *amount)
{
	assert(outnum < tx->used_outputs);
	tx->output[outnum].amount = *amount;
	tx->wtx->outputs[outnum].satoshi = amount->satoshis; /* Raw: low-level helper */
}

void bitcoin_tx_input_set_witness(struct bitcoin_tx *tx, int innum,
				  u8 **witness)
{
	struct wally_tx_witness_stack *stack = NULL;
	size_t stack_size = tal_count(witness);

	/* Free any lingering witness */
	tal_free(tx->input[innum].witness);
	tx->input[innum].witness = witness;

	if (witness) {
		wally_tx_witness_stack_init_alloc(stack_size, &stack);
		for (size_t i = 0; i < stack_size; i++)
			wally_tx_witness_stack_add(stack, witness[i],
						   tal_bytelen(witness[i]));
	}
	wally_tx_set_input_witness(tx->wtx, innum, stack);
	if (stack)
		wally_tx_witness_stack_free(stack);
}

void bitcoin_tx_input_set_script(struct bitcoin_tx *tx, int innum, u8 *script)
{
	tx->input[innum].script = script;
	wally_tx_set_input_script(tx->wtx, innum, script, tal_bytelen(script));
}

/* BIP 141:
 * It is followed by stack items, with each item starts with a var_int
 * to indicate the length. */
static void push_witness(const u8 *witness,
			void (*push)(const void *, size_t, void *), void *pushp)
{
	push_varint_blob(witness, push, pushp);
}

/* BIP144:
 * If the witness is empty, the old serialization format should be used. */
static bool uses_witness(const struct bitcoin_tx *tx)
{
	size_t i;

	for (i = 0; i < tal_count(tx->input); i++) {
		if (tx->input[i].witness)
			return true;
	}
	return false;
}

/* BIP 141: The witness is a serialization of all witness data of the
 * transaction. Each txin is associated with a witness field. A
 * witness field starts with a var_int to indicate the number of stack
 * items for the txin.  */
static void push_witnesses(const struct bitcoin_tx *tx,
			  void (*push)(const void *, size_t, void *), void *pushp)
{
	size_t i;
	for (i = 0; i < tal_count(tx->input); i++) {
		size_t j, elements;

		/* Not every input needs a witness. */
		if (!tx->input[i].witness) {
			push_varint(0, push, pushp);
			continue;
		}
		elements = tal_count(tx->input[i].witness);
		push_varint(elements, push, pushp);
		for (j = 0;
		     j < tal_count(tx->input[i].witness);
		     j++) {
			push_witness(tx->input[i].witness[j],
				    push, pushp);
		}
	}
}

/* For signing, we ignore input scripts on other inputs, and pretend
 * the current input has a certain script: this is indicated by a
 * non-NULL override_script.
 *
 * For this (and other signing weirdness like SIGHASH_SINGLE), we
 * also need the current input being signed; that's in input_num.
 * We also need sighash_type.
 */
static void push_tx(const struct bitcoin_tx *tx,
		    const u8 *override_script,
		    size_t input_num,
		    void (*push)(const void *, size_t, void *), void *pushp,
		    bool bip144)
{
	int res;
	size_t len, written;
	u8 *serialized;;
	u8 flag = 0;

        if (bip144 && uses_witness(tx))
		flag |= WALLY_TX_FLAG_USE_WITNESS;

	wally_tx_get_length(tx->wtx, flag, &len);
	serialized = tal_arr(tmpctx, u8, len);

	res = wally_tx_to_bytes(tx->wtx, flag, serialized, len, &written);
	assert(res == WALLY_OK && len == written);
	push(serialized, len, pushp);
	tal_free(serialized);
}

static void push_sha(const void *data, size_t len, void *shactx_)
{
	struct sha256_ctx *ctx = shactx_;
	sha256_update(ctx, memcheck(data, len), len);
}

static void push_linearize(const void *data, size_t len, void *pptr_)
{
	u8 **pptr = pptr_;
	size_t oldsize = tal_count(*pptr);

	tal_resize(pptr, oldsize + len);
	memcpy(*pptr + oldsize, memcheck(data, len), len);
}

u8 *linearize_tx(const tal_t *ctx, const struct bitcoin_tx *tx)
{
	u8 *arr = tal_arr(ctx, u8, 0);
	push_tx(tx, NULL, 0, push_linearize, &arr, true);
	return arr;
}

static void push_measure(const void *data UNUSED, size_t len, void *lenp)
{
	*(size_t *)lenp += len;
}

size_t measure_tx_weight(const struct bitcoin_tx *tx)
{
	size_t non_witness_len = 0, witness_len = 0;
	push_tx(tx, NULL, 0, push_measure, &non_witness_len, false);
	if (uses_witness(tx)) {
		push_witnesses(tx, push_measure, &witness_len);
		/* Include BIP 144 marker and flag bytes in witness length */
		witness_len += 2;
	}

	/* Normal bytes weigh 4 times more than Witness bytes */
	return non_witness_len * 4 + witness_len;
}

void bitcoin_txid(const struct bitcoin_tx *tx, struct bitcoin_txid *txid)
{
	struct sha256_ctx ctx = SHA256_INIT;

	/* For TXID, we never use extended form. */
	push_tx(tx, NULL, 0, push_sha, &ctx, false);
	sha256_double_done(&ctx, &txid->shad);
}

/* Use the bitcoin_tx destructor to also free the wally_tx */
static void bitcoin_tx_destroy(struct bitcoin_tx *tx)
{
	wally_tx_free(tx->wtx);
}

struct bitcoin_tx *bitcoin_tx(const tal_t *ctx, varint_t input_count,
			      varint_t output_count)
{
	struct bitcoin_tx *tx = tal(ctx, struct bitcoin_tx);
	size_t i;
	tx->used_inputs = 0;
	tx->used_outputs = 0;

	wally_tx_init_alloc(WALLY_TX_VERSION_2, 0, input_count, output_count,
			    &tx->wtx);
	tal_add_destructor(tx, bitcoin_tx_destroy);

	tx->input_amounts = tal_arrz(tx, struct amount_sat*, input_count);
	tx->output = tal_arrz(tx, struct bitcoin_tx_output, output_count);
	tx->input = tal_arrz(tx, struct bitcoin_tx_input, input_count);
	for (i = 0; i < tal_count(tx->input); i++) {
		/* We assume NULL is a zero bitmap */
		assert(tx->input[i].script == NULL);
		tx->input[i].sequence_number = BITCOIN_TX_DEFAULT_SEQUENCE;
		tx->input[i].witness = NULL;
	}
	tx->wtx->locktime = 0;
	tx->wtx->version = 2;
	return tx;
}

static bool pull_sha256_double(const u8 **cursor, size_t *max,
			       struct sha256_double *h)
{
	return pull(cursor, max, h, sizeof(*h));
}

static u64 pull_value(const u8 **cursor, size_t *max)
{
	u64 amount;

	amount = pull_le64(cursor, max);
	return amount;
}

static struct amount_sat pull_amount_sat(const u8 **cursor, size_t *max)
{
	struct amount_sat sat;

	sat.satoshis = pull_value(cursor, max); /* Raw: low-level helper */
	return sat;
}

/* Pulls a varint which specifies n items of mult size: ensures basic
 * sanity to avoid trivial OOM */
static u64 pull_length(const u8 **cursor, size_t *max, size_t mult)
{
	u64 v = pull_varint(cursor, max);
	if (v * mult > *max) {
		*cursor = NULL;
		*max = 0;
		return 0;
	}
	return v;
}

static void pull_input(const tal_t *ctx, const u8 **cursor, size_t *max,
		       struct bitcoin_tx_input *input)
{
	u64 script_len;
	pull_sha256_double(cursor, max, &input->txid.shad);
	input->index = pull_le32(cursor, max);
	script_len = pull_length(cursor, max, 1);
	if (script_len)
		input->script = tal_arr(ctx, u8, script_len);
	else
		input->script = NULL;
	pull(cursor, max, input->script, tal_count(input->script));
	input->sequence_number = pull_le32(cursor, max);
}

static void pull_output(const tal_t *ctx, const u8 **cursor, size_t *max,
			struct bitcoin_tx_output *output)
{
	output->amount = pull_amount_sat(cursor, max);
	output->script = tal_arr(ctx, u8, pull_length(cursor, max, 1));
	pull(cursor, max, output->script, tal_count(output->script));
}

static u8 *pull_witness_item(const tal_t *ctx, const u8 **cursor, size_t *max)
{
	uint64_t len = pull_length(cursor, max, 1);
	u8 *item;

	item = tal_arr(ctx, u8, len);
	pull(cursor, max, item, len);
	return item;
}

static void pull_witness(struct bitcoin_tx_input *inputs, size_t i,
			 const u8 **cursor, size_t *max)
{
	uint64_t j, num = pull_length(cursor, max, 1);

	/* 0 means not using witness. */
	if (num == 0) {
		inputs[i].witness = NULL;
		return;
	}

	inputs[i].witness = tal_arr(inputs, u8 *, num);
	for (j = 0; j < num; j++) {
		inputs[i].witness[j] = pull_witness_item(inputs[i].witness,
							 cursor, max);
	}
}

struct bitcoin_tx *pull_bitcoin_tx(const tal_t *ctx, const u8 **cursor,
				   size_t *max)
{
	size_t i;
	u64 count;
	u8 flag = 0;
	const u8 *oldcursor = *cursor;
	size_t wsize;
	struct bitcoin_tx *tx = tal(ctx, struct bitcoin_tx);
	if (wally_tx_from_bytes(*cursor, *max, 0, &tx->wtx) != WALLY_OK) {
		*cursor = 0;
		return tal_free(tx);
	}
	tal_add_destructor(tx, bitcoin_tx_destroy);
	wally_tx_get_length(tx->wtx, WALLY_TX_FLAG_USE_WITNESS, &wsize);
	tx->input_amounts =
	    tal_arrz(tx, struct amount_sat *, tx->wtx->inputs_allocation_len);

	assert(pull_le32(cursor, max) == tx->wtx->version);
	count = pull_length(cursor, max, 32 + 4 + 4 + 1);
	/* BIP 144 marker is 0 (impossible to have tx with 0 inputs) */
	if (count == 0) {
		pull(cursor, max, &flag, 1);
		if (flag != SEGREGATED_WITNESS_FLAG)
			return tal_free(tx);
		count = pull_length(cursor, max, 32 + 4 + 4 + 1);
	}

	tx->input = tal_arr(tx, struct bitcoin_tx_input, count);
	tx->used_inputs = count;

	for (i = 0; i < tal_count(tx->input); i++)
		pull_input(tx, cursor, max, tx->input + i);

	count = pull_length(cursor, max, 8 + 1);
	tx->output = tal_arr(tx, struct bitcoin_tx_output, count);
	tx->used_outputs = count;

	for (i = 0; i < tal_count(tx->output); i++)
		pull_output(tx, cursor, max, tx->output + i);

	if (flag & SEGREGATED_WITNESS_FLAG) {
		for (i = 0; i < tal_count(tx->input); i++)
			pull_witness(tx->input, i, cursor, max);
	} else {
		for (i = 0; i < tal_count(tx->input); i++)
			tx->input[i].witness = NULL;
	}
	assert(pull_le32(cursor, max) == tx->wtx->locktime);

	assert(!*cursor || oldcursor + wsize == *cursor);
	/* If we ran short, fail. */
	if (!*cursor)
		tx = tal_free(tx);
	return tx;
}

struct bitcoin_tx *bitcoin_tx_from_hex(const tal_t *ctx, const char *hex,
				       size_t hexlen)
{
	const char *end;
	u8 *linear_tx;
	const u8 *p;
	struct bitcoin_tx *tx;
	size_t len;

	end = memchr(hex, '\n', hexlen);
	if (!end)
		end = hex + hexlen;

	len = hex_data_size(end - hex);
	p = linear_tx = tal_arr(ctx, u8, len);
	if (!hex_decode(hex, end - hex, linear_tx, len))
		goto fail;

	tx = pull_bitcoin_tx(ctx, &p, &len);
	if (!tx)
		goto fail;

	if (len)
		goto fail_free_tx;

	tal_free(linear_tx);
	return tx;

fail_free_tx:
	tal_free(tx);
fail:
	tal_free(linear_tx);
	return NULL;
}

/* <sigh>.  Bitcoind represents hashes as little-endian for RPC. */
static void reverse_bytes(u8 *arr, size_t len)
{
	unsigned int i;

	for (i = 0; i < len / 2; i++) {
		unsigned char tmp = arr[i];
		arr[i] = arr[len - 1 - i];
		arr[len - 1 - i] = tmp;
	}
}

bool bitcoin_txid_from_hex(const char *hexstr, size_t hexstr_len,
			   struct bitcoin_txid *txid)
{
	if (!hex_decode(hexstr, hexstr_len, txid, sizeof(*txid)))
		return false;
	reverse_bytes(txid->shad.sha.u.u8, sizeof(txid->shad.sha.u.u8));
	return true;
}

bool bitcoin_txid_to_hex(const struct bitcoin_txid *txid,
			 char *hexstr, size_t hexstr_len)
{
	struct sha256_double rev = txid->shad;
	reverse_bytes(rev.sha.u.u8, sizeof(rev.sha.u.u8));
	return hex_encode(&rev, sizeof(rev), hexstr, hexstr_len);
}

static char *fmt_bitcoin_tx(const tal_t *ctx, const struct bitcoin_tx *tx)
{
	u8 *lin = linearize_tx(ctx, tx);
	char *s = tal_hex(ctx, lin);
	tal_free(lin);
	return s;
}

static char *fmt_bitcoin_txid(const tal_t *ctx, const struct bitcoin_txid *txid)
{
	char *hexstr = tal_arr(ctx, char, hex_str_size(sizeof(*txid)));

	bitcoin_txid_to_hex(txid, hexstr, hex_str_size(sizeof(*txid)));
	return hexstr;
}

REGISTER_TYPE_TO_STRING(bitcoin_tx, fmt_bitcoin_tx);
REGISTER_TYPE_TO_STRING(bitcoin_txid, fmt_bitcoin_txid);
