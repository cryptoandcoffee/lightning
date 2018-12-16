#include "../json_stream.c"
#include "../jsonrpc.c"
#include "../json.c"

/* AUTOGENERATED MOCKS START */
/* Generated stub for db_begin_transaction_ */
void db_begin_transaction_(struct db *db UNNEEDED, const char *location UNNEEDED)
{ fprintf(stderr, "db_begin_transaction_ called!\n"); abort(); }
/* Generated stub for db_commit_transaction */
void db_commit_transaction(struct db *db UNNEEDED)
{ fprintf(stderr, "db_commit_transaction called!\n"); abort(); }
/* Generated stub for fatal */
void   fatal(const char *fmt UNNEEDED, ...)
{ fprintf(stderr, "fatal called!\n"); abort(); }
/* Generated stub for feerate_from_style */
u32 feerate_from_style(u32 feerate UNNEEDED, enum feerate_style style UNNEEDED)
{ fprintf(stderr, "feerate_from_style called!\n"); abort(); }
/* Generated stub for feerate_name */
const char *feerate_name(enum feerate feerate UNNEEDED)
{ fprintf(stderr, "feerate_name called!\n"); abort(); }
/* Generated stub for fmt_wireaddr_without_port */
char *fmt_wireaddr_without_port(const tal_t *ctx UNNEEDED, const struct wireaddr *a UNNEEDED)
{ fprintf(stderr, "fmt_wireaddr_without_port called!\n"); abort(); }
/* Generated stub for log_ */
void log_(struct log *log UNNEEDED, enum log_level level UNNEEDED, const char *fmt UNNEEDED, ...)

{ fprintf(stderr, "log_ called!\n"); abort(); }
/* Generated stub for log_io */
void log_io(struct log *log UNNEEDED, enum log_level dir UNNEEDED, const char *comment UNNEEDED,
	    const void *data UNNEEDED, size_t len UNNEEDED)
{ fprintf(stderr, "log_io called!\n"); abort(); }
/* Generated stub for log_prefix */
const char *log_prefix(const struct log *log UNNEEDED)
{ fprintf(stderr, "log_prefix called!\n"); abort(); }
/* Generated stub for new_log */
struct log *new_log(const tal_t *ctx UNNEEDED, struct log_book *record UNNEEDED, const char *fmt UNNEEDED, ...)
{ fprintf(stderr, "new_log called!\n"); abort(); }
/* Generated stub for new_reltimer_ */
struct oneshot *new_reltimer_(struct timers *timers UNNEEDED,
			      const tal_t *ctx UNNEEDED,
			      struct timerel expire UNNEEDED,
			      void (*cb)(void *) UNNEEDED, void *arg UNNEEDED)
{ fprintf(stderr, "new_reltimer_ called!\n"); abort(); }
/* Generated stub for param */
bool param(struct command *cmd UNNEEDED, const char *buffer UNNEEDED,
	   const jsmntok_t params[] UNNEEDED, ...)
{ fprintf(stderr, "param called!\n"); abort(); }
/* Generated stub for param_feerate_estimate */
struct command_result *param_feerate_estimate(struct command *cmd UNNEEDED,
					      u32 **feerate_per_kw UNNEEDED,
					      enum feerate feerate UNNEEDED)
{ fprintf(stderr, "param_feerate_estimate called!\n"); abort(); }
/* Generated stub for param_number */
struct command_result *param_number(struct command *cmd UNNEEDED, const char *name UNNEEDED,
				    const char *buffer UNNEEDED, const jsmntok_t *tok UNNEEDED,
				    unsigned int **num UNNEEDED)
{ fprintf(stderr, "param_number called!\n"); abort(); }
/* Generated stub for param_sha256 */
struct command_result *param_sha256(struct command *cmd UNNEEDED, const char *name UNNEEDED,
				    const char *buffer UNNEEDED, const jsmntok_t *tok UNNEEDED,
				    struct sha256 **hash UNNEEDED)
{ fprintf(stderr, "param_sha256 called!\n"); abort(); }
/* Generated stub for param_tok */
struct command_result *param_tok(struct command *cmd UNNEEDED, const char *name UNNEEDED,
				 const char *buffer UNNEEDED, const jsmntok_t * tok UNNEEDED,
				 const jsmntok_t **out UNNEEDED)
{ fprintf(stderr, "param_tok called!\n"); abort(); }
/* AUTOGENERATED MOCKS END */

bool deprecated_apis;

static int test_json_filter(void)
{
	struct json_stream *result = new_json_stream(NULL, NULL);
	jsmntok_t *toks;
	const jsmntok_t *x;
	bool valid;
	int i;
	char *badstr = tal_arr(result, char, 256);
	const char *str;

	/* Fill with junk, and nul-terminate (256 -> 0) */
	for (i = 1; i < 257; i++)
		badstr[i-1] = i;

	json_object_start(result, NULL);
	json_add_string(result, "x", badstr);
	json_object_end(result);

	/* Parse back in, make sure nothing crazy. */
	str = tal_strndup(result, membuf_elems(&result->outbuf),
			  membuf_num_elems(&result->outbuf));

	toks = json_parse_input(str, str, strlen(str), &valid);
	assert(valid);
	assert(toks);

	assert(toks[0].type == JSMN_OBJECT);
	x = json_get_member(str, toks, "x");
	assert(x);
	assert(x->type == JSMN_STRING);

	/* There are 7 one-letter escapes, and (32-5) \uXXXX escapes. */
	assert((x->end - x->start) == 255 + 7*1 + (32-5)*5);
	/* No control characters. */
	for (i = x->start; i < x->end; i++) {
		assert((unsigned)str[i] >= ' ');
		assert((unsigned)str[i] != 127);
	}
	tal_free(result);
	return 0;
}

static void test_json_escape(void)
{
	int i;

	for (i = 1; i < 256; i++) {
		char badstr[2];
		struct json_stream *result = new_json_stream(NULL, NULL);
		struct json_escaped *esc;

		badstr[0] = i;
		badstr[1] = 0;

		json_object_start(result, NULL);
		esc = json_escape(NULL, badstr);
		json_add_escaped_string(result, "x", take(esc));
		json_object_end(result);

		const char *str = tal_strndup(result, membuf_elems(&result->outbuf),
					      membuf_num_elems(&result->outbuf));
		if (i == '\\' || i == '"'
		    || i == '\n' || i == '\r' || i == '\b'
		    || i == '\t' || i == '\f')
			assert(strstarts(str, "\n{\n  \"x\": \"\\"));
		else if (i < 32 || i == 127) {
			assert(strstarts(str, "\n{\n  \"x\": \"\\u00"));
		} else {
			char expect[] = "\n{\n  \"x\": \"?\"\n}";
			expect[11] = i;
			assert(streq(str, expect));
		}
		tal_free(result);
	}
}

static void test_json_partial(void)
{
	const tal_t *ctx = tal(NULL, char);

	assert(streq(json_partial_escape(ctx, "\\")->s, "\\\\"));
	assert(streq(json_partial_escape(ctx, "\\\\")->s, "\\\\"));
	assert(streq(json_partial_escape(ctx, "\\\\\\")->s, "\\\\\\\\"));
	assert(streq(json_partial_escape(ctx, "\\\\\\\\")->s, "\\\\\\\\"));
	assert(streq(json_partial_escape(ctx, "\\n")->s, "\\n"));
	assert(streq(json_partial_escape(ctx, "\n")->s, "\\n"));
	assert(streq(json_partial_escape(ctx, "\\\"")->s, "\\\""));
	assert(streq(json_partial_escape(ctx, "\"")->s, "\\\""));
	assert(streq(json_partial_escape(ctx, "\\t")->s, "\\t"));
	assert(streq(json_partial_escape(ctx, "\t")->s, "\\t"));
	assert(streq(json_partial_escape(ctx, "\\b")->s, "\\b"));
	assert(streq(json_partial_escape(ctx, "\b")->s, "\\b"));
	assert(streq(json_partial_escape(ctx, "\\r")->s, "\\r"));
	assert(streq(json_partial_escape(ctx, "\r")->s, "\\r"));
	assert(streq(json_partial_escape(ctx, "\\f")->s, "\\f"));
	assert(streq(json_partial_escape(ctx, "\f")->s, "\\f"));
	/* You're allowed to escape / according to json.org. */
	assert(streq(json_partial_escape(ctx, "\\/")->s, "\\/"));
	assert(streq(json_partial_escape(ctx, "/")->s, "/"));

	assert(streq(json_partial_escape(ctx, "\\u0FFF")->s, "\\u0FFF"));
	assert(streq(json_partial_escape(ctx, "\\u0FFFx")->s, "\\u0FFFx"));

	/* Unknown escapes should be escaped. */
	assert(streq(json_partial_escape(ctx, "\\x")->s, "\\\\x"));
	tal_free(ctx);
}

/* Test that we can segment and parse a stream of json objects correctly. */
static void test_json_stream(void)
{
	bool valid;
	char *input, *talstr;
	jsmntok_t *toks;

	/* Multiple full messages in a single buffer (happens when buffer
	 * boundary coincides with message boundary, or read returned after
	 * timeout. */
	input = "{\"x\":\"x\"}{\"y\":\"y\"}";
	talstr = tal_strndup(NULL, input, strlen(input));
	toks = json_parse_input(talstr, talstr, strlen(talstr), &valid);
	assert(toks);
	assert(tal_count(toks) == 4);
	assert(toks[0].start == 0 && toks[0].end == 9);
	assert(valid);
	tal_free(talstr);

	/* Multiple messages, and the last one is partial, far more likely than
	 * accidentally getting the boundaries to match. */
	input = "{\"x\":\"x\"}{\"y\":\"y\"}{\"z\":\"z";
	talstr = tal_strndup(NULL, input, strlen(input));
	toks = json_parse_input(talstr, talstr, strlen(talstr), &valid);
	assert(toks);
	assert(tal_count(toks) == 4);
	assert(toks[0].start == 0 && toks[0].end == 9);
	assert(valid);
	tal_free(talstr);
}

int main(void)
{
	setup_locale();

	test_json_filter();
	test_json_escape();
	test_json_partial();
	test_json_stream();
	assert(!taken_any());
	take_cleanup();
}
