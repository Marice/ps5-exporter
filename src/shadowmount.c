/* ShadowMountPlus exposes a JSON API on 127.0.0.1:10101 (POST with a JSON
   body). The responses are flat enough for a small key scanner, so there is
   no JSON library here: we look for "key": and read the value after it.

   Every scrape must stay well inside Prometheus' timeout (10 s by default),
   so the three calls share one wall-clock budget and the game list, which
   almost never changes, is cached. */
#include "shadowmount.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "http.h"
#include "metrics.h"

#define SM_HOST "127.0.0.1"
#define SM_PORT 10101
#define SM_BUDGET_MS 4000          /* whole collector, all calls together */
#define GAMES_CACHE_SECONDS 300    /* the library rarely changes */
#define GAMES_CACHE_BYTES (48 * 1024)

static char g_resp[256 * 1024];
static int g_pass; /* which metric the item emitters write in this pass */

/* Rendered game metrics from a previous scrape, so a slow or busy
   ShadowMount does not stretch every scrape. */
static char g_games_cache[GAMES_CACHE_BYTES];
static size_t g_games_cache_len;
static time_t g_games_cache_time;
static bool g_games_cache_truncated;
static long long g_games_count;

static long long now_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Find "key": inside [obj, end) and return a pointer to the value start. */
static const char* find_key(const char* obj, const char* end, const char* key)
{
	char needle[80];
	int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
	if (n <= 0 || (size_t)n >= sizeof(needle)) return NULL;
	size_t nlen = (size_t)n;
	if (end < obj || (size_t)(end - obj) < nlen) return NULL;
	for (const char* p = obj; p + nlen <= end; p++) {
		if (memcmp(p, needle, nlen) == 0) {
			p += nlen;
			while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
			return p < end ? p : NULL;
		}
	}
	return NULL;
}

static int get_string(const char* obj, const char* end, const char* key, char* out, size_t cap)
{
	const char* v = find_key(obj, end, key);
	out[0] = 0;
	if (!v || v >= end || *v != '"') return 0;
	v++;
	size_t j = 0;
	while (v < end && *v != '"' && j + 2 < cap) {
		if (*v == '\\') {
			if (v + 1 >= end) break;
			v++;
			if (*v == 'n' || *v == 't' || *v == 'r') { out[j++] = ' '; v++; continue; }
			if (*v == 'u') {
				/* \uXXXX: skip the escape only when all four digits are there. */
				if (v + 4 >= end) break;
				v += 5;
				out[j++] = '?';
				continue;
			}
		}
		if (*v == '"' || *v == '\\') out[j++] = '\\';
		out[j++] = *v++;
	}
	out[j] = 0;
	return 1;
}

static long long get_number(const char* obj, const char* end, const char* key, long long dflt)
{
	const char* v = find_key(obj, end, key);
	if (!v || v >= end) return dflt;
	if (*v == '-' || (*v >= '0' && *v <= '9')) return atoll(v);
	return dflt;
}

static int get_bool(const char* obj, const char* end, const char* key, int dflt)
{
	const char* v = find_key(obj, end, key);
	if (!v || v >= end) return dflt;
	if ((size_t)(end - v) >= 4 && strncmp(v, "true", 4) == 0) return 1;
	if ((size_t)(end - v) >= 5 && strncmp(v, "false", 5) == 0) return 0;
	return dflt;
}

/* Iterate the objects of the array named key: calls fn(obj_start, obj_end). */
typedef void (*item_fn)(const char* obj, const char* end, Buf* b);

static void for_each_item(const char* json, size_t json_len, const char* key, item_fn fn, Buf* b)
{
	const char* end = json + json_len;
	const char* arr = find_key(json, end, key);
	if (!arr || arr >= end || *arr != '[') return;
	const char* p = arr + 1;
	while (p < end) {
		while (p < end && *p != '{' && *p != ']') p++;
		if (p >= end || *p == ']') break;
		/* Find the matching closing brace, honouring strings. */
		int depth = 0, in_str = 0;
		const char* q = p;
		for (; q < end; q++) {
			if (in_str) {
				if (*q == '\\') q++;
				else if (*q == '"') in_str = 0;
			} else if (*q == '"') in_str = 1;
			else if (*q == '{') depth++;
			else if (*q == '}' && --depth == 0) break;
		}
		if (q >= end) break;
		fn(p, q + 1, b);
		p = q + 1;
	}
}

static void storage_item(const char* obj, const char* end, Buf* b)
{
	char mount[160], source[160], fs[48];
	get_string(obj, end, "mount_point", mount, sizeof(mount));
	get_string(obj, end, "source", source, sizeof(source));
	get_string(obj, end, "filesystem", fs, sizeof(fs));
	long long total = get_number(obj, end, "total_bytes", -1);
	long long avail = get_number(obj, end, "available_bytes", -1);
	long long used = get_number(obj, end, "used_bytes", -1);
	int ro = get_bool(obj, end, "read_only", 0);
	if (total < 0) return;
	switch (g_pass) {
	case 0: buf_addf(b, "ps5_shadowmount_storage_total_bytes{mount_point=\"%s\",source=\"%s\",filesystem=\"%s\"} %lld\n", mount, source, fs, total); break;
	case 1: if (avail >= 0) buf_addf(b, "ps5_shadowmount_storage_available_bytes{mount_point=\"%s\",source=\"%s\",filesystem=\"%s\"} %lld\n", mount, source, fs, avail); break;
	case 2: if (used >= 0) buf_addf(b, "ps5_shadowmount_storage_used_bytes{mount_point=\"%s\",source=\"%s\",filesystem=\"%s\"} %lld\n", mount, source, fs, used); break;
	default: buf_addf(b, "ps5_shadowmount_storage_read_only{mount_point=\"%s\",source=\"%s\",filesystem=\"%s\"} %d\n", mount, source, fs, ro); break;
	}
}

static void game_item(const char* obj, const char* end, Buf* b)
{
	char title_id[32], name[128], platform[16], source_type[16];
	get_string(obj, end, "title_id", title_id, sizeof(title_id));
	if (!title_id[0]) return;
	get_string(obj, end, "title_name", name, sizeof(name));
	get_string(obj, end, "platform", platform, sizeof(platform));
	get_string(obj, end, "source_type", source_type, sizeof(source_type));
	int mounted = get_bool(obj, end, "mounted", 0);
	int installed = get_bool(obj, end, "installed", 0);
	int available = get_bool(obj, end, "source_available", 1);
	switch (g_pass) {
	case 0: buf_addf(b, "ps5_shadowmount_game_info{title_id=\"%s\",name=\"%s\",platform=\"%s\",source_type=\"%s\"} 1\n", title_id, name, platform, source_type); break;
	case 1: buf_addf(b, "ps5_shadowmount_game_mounted{title_id=\"%s\"} %d\n", title_id, mounted); break;
	case 2: buf_addf(b, "ps5_shadowmount_game_installed{title_id=\"%s\"} %d\n", title_id, installed); break;
	default: buf_addf(b, "ps5_shadowmount_game_source_available{title_id=\"%s\"} %d\n", title_id, available); break;
	}
}

/* Render the game metrics into the cache buffer, from a fresh API response. */
static int refresh_games_cache(int budget_ms)
{
	int st = http_post_json(SM_HOST, SM_PORT, "/api/v1/games", "{\"include_size\":false}", g_resp, sizeof(g_resp), budget_ms);
	if (st != 200) return 0;
	size_t resp_len = strlen(g_resp);
	Buf cache;
	buf_init(&cache, g_games_cache, sizeof(g_games_cache));
	g_games_count = get_number(g_resp, g_resp + resp_len, "count", 0);
	static const char* heads[4] = {
		"# HELP ps5_shadowmount_game_info Game metadata.\n# TYPE ps5_shadowmount_game_info gauge\n",
		"# HELP ps5_shadowmount_game_mounted Game runtime mount active (1 while the game runs).\n# TYPE ps5_shadowmount_game_mounted gauge\n",
		"# HELP ps5_shadowmount_game_installed Game registered on the home screen.\n# TYPE ps5_shadowmount_game_installed gauge\n",
		"# HELP ps5_shadowmount_game_source_available Game source (folder or image) reachable.\n# TYPE ps5_shadowmount_game_source_available gauge\n",
	};
	for (g_pass = 0; g_pass < 4; g_pass++) {
		buf_addf(&cache, "%s", heads[g_pass]);
		for_each_item(g_resp, resp_len, "games", game_item, &cache);
	}
	g_games_cache_truncated = cache.truncated;
	g_games_cache_len = cache.len;
	g_games_cache_time = time(NULL);
	return 1;
}

void metrics_shadowmount_reset_cache(void)
{
	g_games_cache_len = 0;
	g_games_cache_time = 0;
	g_games_cache_truncated = false;
	g_games_count = 0;
}

void metrics_shadowmount(Buf* b)
{
	long long deadline = now_ms() + SM_BUDGET_MS;
	int remaining = (int)(deadline - now_ms());
	int st = http_post_json(SM_HOST, SM_PORT, "/api/v1/version", "{}", g_resp, sizeof(g_resp), remaining > 1500 ? 1500 : remaining);
	buf_addf(b, "# HELP ps5_shadowmount_up Whether the ShadowMountPlus API answered.\n# TYPE ps5_shadowmount_up gauge\nps5_shadowmount_up %d\n", st == 200);
	metrics_note_collector("shadowmount", st == 200);
	if (st != 200) return;

	char version[32];
	get_string(g_resp, g_resp + strlen(g_resp), "shadowmount_version", version, sizeof(version));
	buf_addf(b, "# HELP ps5_shadowmount_info ShadowMountPlus version.\n# TYPE ps5_shadowmount_info gauge\nps5_shadowmount_info{version=\"%s\"} 1\n", version);

	remaining = (int)(deadline - now_ms());
	if (remaining > 200 && http_post_json(SM_HOST, SM_PORT, "/api/v1/storage", "{}", g_resp, sizeof(g_resp), remaining > 1500 ? 1500 : remaining) == 200) {
		size_t resp_len = strlen(g_resp);
		static const char* heads[4] = {
			"# HELP ps5_shadowmount_storage_total_bytes Filesystem size as reported by ShadowMount.\n# TYPE ps5_shadowmount_storage_total_bytes gauge\n",
			"# HELP ps5_shadowmount_storage_available_bytes Space available for new files.\n# TYPE ps5_shadowmount_storage_available_bytes gauge\n",
			"# HELP ps5_shadowmount_storage_used_bytes Used space.\n# TYPE ps5_shadowmount_storage_used_bytes gauge\n",
			"# HELP ps5_shadowmount_storage_read_only Filesystem mounted read-only.\n# TYPE ps5_shadowmount_storage_read_only gauge\n",
		};
		for (g_pass = 0; g_pass < 4; g_pass++) {
			buf_addf(b, "%s", heads[g_pass]);
			for_each_item(g_resp, resp_len, "mounts", storage_item, b);
		}
	}

	/* Games: refresh at most every GAMES_CACHE_SECONDS, and only when the
	   budget allows. A stale cache is served in between, with its age so a
	   dashboard can tell how fresh the list is. */
	time_t now = time(NULL);
	int fresh = g_games_cache_len > 0 && (now - g_games_cache_time) < GAMES_CACHE_SECONDS;
	remaining = (int)(deadline - now_ms());
	if (!fresh && remaining > 500) {
		refresh_games_cache(remaining > 2500 ? 2500 : remaining);
	}
	if (g_games_cache_len > 0) {
		buf_addf(b, "# HELP ps5_shadowmount_games Games known to ShadowMount.\n# TYPE ps5_shadowmount_games gauge\nps5_shadowmount_games %lld\n", g_games_count);
		buf_addf(b, "# HELP ps5_shadowmount_games_cache_age_seconds Age of the cached game list.\n# TYPE ps5_shadowmount_games_cache_age_seconds gauge\nps5_shadowmount_games_cache_age_seconds %ld\n", (long)(time(NULL) - g_games_cache_time));
		buf_addf(b, "%s", g_games_cache);
		/* The cache has its own capacity: a list that did not fit there is
		   just as incomplete as one that does not fit here. */
		if (g_games_cache_truncated) b->truncated = true;
	}
}
