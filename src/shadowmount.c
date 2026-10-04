/* ShadowMountPlus exposes a JSON API on 127.0.0.1:10101 (POST with a JSON
   body). The responses are flat enough for a small key scanner, so there is
   no JSON library here: we look for "key": and read the value after it. */
#include "shadowmount.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"

#define SM_HOST "127.0.0.1"
#define SM_PORT 10101
#define APPEND(...) do { int n_ = snprintf(out + len, cap > len ? cap - len : 0, __VA_ARGS__); if (n_ > 0) len += (size_t)n_; } while (0)

static char g_resp[256 * 1024];
static int g_pass; /* which metric the item emitters write in this pass */

/* Find "key": inside [obj, end) and return a pointer to the value start. */
static const char* find_key(const char* obj, const char* end, const char* key)
{
	char needle[80];
	snprintf(needle, sizeof(needle), "\"%s\":", key);
	size_t nlen = strlen(needle);
	for (const char* p = obj; p + nlen < end; p++) {
		if (memcmp(p, needle, nlen) == 0) {
			p += nlen;
			while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
			return p;
		}
	}
	return NULL;
}

static int get_string(const char* obj, const char* end, const char* key, char* out, size_t cap)
{
	const char* v = find_key(obj, end, key);
	out[0] = 0;
	if (!v || *v != '"') return 0;
	v++;
	size_t j = 0;
	while (v < end && *v != '"' && j + 2 < cap) {
		if (*v == '\\' && v + 1 < end) {
			v++;
			if (*v == 'n' || *v == 't' || *v == 'r') { out[j++] = ' '; v++; continue; }
			if (*v == 'u') { v += 5; out[j++] = '?'; continue; }
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
	if (strncmp(v, "true", 4) == 0) return 1;
	if (strncmp(v, "false", 5) == 0) return 0;
	return dflt;
}

/* Iterate the objects of the array named key: calls fn(obj_start, obj_end). */
typedef void (*item_fn)(const char* obj, const char* end, char** out, size_t* cap);

static void for_each_item(const char* json, const char* key, item_fn fn, char** out, size_t* cap)
{
	const char* arr = find_key(json, json + strlen(json), key);
	if (!arr || *arr != '[') return;
	const char* p = arr + 1;
	const char* end = json + strlen(json);
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
		fn(p, q + 1, out, cap);
		p = q + 1;
	}
}

static void storage_item(const char* obj, const char* end, char** outp, size_t* capp)
{
	char* out = *outp;
	size_t cap = *capp, len = 0;
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
	case 0: APPEND("ps5_shadowmount_storage_total_bytes{mount_point=\"%s\",source=\"%s\",filesystem=\"%s\"} %lld\n", mount, source, fs, total); break;
	case 1: if (avail >= 0) APPEND("ps5_shadowmount_storage_available_bytes{mount_point=\"%s\",source=\"%s\",filesystem=\"%s\"} %lld\n", mount, source, fs, avail); break;
	case 2: if (used >= 0) APPEND("ps5_shadowmount_storage_used_bytes{mount_point=\"%s\",source=\"%s\",filesystem=\"%s\"} %lld\n", mount, source, fs, used); break;
	default: APPEND("ps5_shadowmount_storage_read_only{mount_point=\"%s\",source=\"%s\",filesystem=\"%s\"} %d\n", mount, source, fs, ro); break;
	}
	*outp += len;
	*capp -= len;
}

static void game_item(const char* obj, const char* end, char** outp, size_t* capp)
{
	char* out = *outp;
	size_t cap = *capp, len = 0;
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
	case 0: APPEND("ps5_shadowmount_game_info{title_id=\"%s\",name=\"%s\",platform=\"%s\",source_type=\"%s\"} 1\n", title_id, name, platform, source_type); break;
	case 1: APPEND("ps5_shadowmount_game_mounted{title_id=\"%s\"} %d\n", title_id, mounted); break;
	case 2: APPEND("ps5_shadowmount_game_installed{title_id=\"%s\"} %d\n", title_id, installed); break;
	default: APPEND("ps5_shadowmount_game_source_available{title_id=\"%s\"} %d\n", title_id, available); break;
	}
	*outp += len;
	*capp -= len;
}

size_t metrics_shadowmount(char* out, size_t cap)
{
	size_t len = 0;
	int st = http_post_json(SM_HOST, SM_PORT, "/api/v1/version", "{}", g_resp, sizeof(g_resp), 1500);
	APPEND("# HELP ps5_shadowmount_up Whether the ShadowMountPlus API answered.\n# TYPE ps5_shadowmount_up gauge\nps5_shadowmount_up %d\n", st == 200);
	if (st != 200) return len;

	char version[32];
	const char* rend = g_resp + strlen(g_resp);
	get_string(g_resp, rend, "shadowmount_version", version, sizeof(version));
	APPEND("# HELP ps5_shadowmount_info ShadowMountPlus version.\n# TYPE ps5_shadowmount_info gauge\nps5_shadowmount_info{version=\"%s\"} 1\n", version);

	if (http_post_json(SM_HOST, SM_PORT, "/api/v1/storage", "{}", g_resp, sizeof(g_resp), 3000) == 200) {
		static const char* heads[4] = {
			"# HELP ps5_shadowmount_storage_total_bytes Filesystem size as reported by ShadowMount.\n# TYPE ps5_shadowmount_storage_total_bytes gauge\n",
			"# HELP ps5_shadowmount_storage_available_bytes Space available for new files.\n# TYPE ps5_shadowmount_storage_available_bytes gauge\n",
			"# HELP ps5_shadowmount_storage_used_bytes Used space.\n# TYPE ps5_shadowmount_storage_used_bytes gauge\n",
			"# HELP ps5_shadowmount_storage_read_only Filesystem mounted read-only.\n# TYPE ps5_shadowmount_storage_read_only gauge\n",
		};
		for (g_pass = 0; g_pass < 4; g_pass++) {
			APPEND("%s", heads[g_pass]);
			char* p = out + len;
			size_t c = cap > len ? cap - len : 0;
			for_each_item(g_resp, "mounts", storage_item, &p, &c);
			len = (size_t)(p - out);
		}
	}

	if (http_post_json(SM_HOST, SM_PORT, "/api/v1/games", "{\"include_size\":false}", g_resp, sizeof(g_resp), 5000) == 200) {
		rend = g_resp + strlen(g_resp);
		long long count = get_number(g_resp, rend, "count", 0);
		APPEND("# HELP ps5_shadowmount_games Games known to ShadowMount.\n# TYPE ps5_shadowmount_games gauge\nps5_shadowmount_games %lld\n", count);
		static const char* heads[4] = {
			"# HELP ps5_shadowmount_game_info Game metadata.\n# TYPE ps5_shadowmount_game_info gauge\n",
			"# HELP ps5_shadowmount_game_mounted Game runtime mount active (1 while the game runs).\n# TYPE ps5_shadowmount_game_mounted gauge\n",
			"# HELP ps5_shadowmount_game_installed Game registered on the home screen.\n# TYPE ps5_shadowmount_game_installed gauge\n",
			"# HELP ps5_shadowmount_game_source_available Game source (folder or image) reachable.\n# TYPE ps5_shadowmount_game_source_available gauge\n",
		};
		for (g_pass = 0; g_pass < 4; g_pass++) {
			APPEND("%s", heads[g_pass]);
			char* p = out + len;
			size_t c = cap > len ? cap - len : 0;
			for_each_item(g_resp, "games", game_item, &p, &c);
			len = (size_t)(p - out);
		}
	}
	return len;
}
