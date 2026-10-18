/* Looks libkernel functions up on first use: by name, then by NID (as
 * ps5-signal resolves its ICC calls). Results, misses included, are kept. */
#include "optional.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/nid.h>

/* libkernel in this process. */
#define LIBKERNEL_HANDLE 0x1
#define MAX_SYMBOLS 32

static struct {
	const char* name;
	void* address;
} g_symbols[MAX_SYMBOLS];
static int g_count;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void* optional_symbol(const char* name)
{
	pthread_mutex_lock(&g_lock);
	for (int i = 0; i < g_count; i++) {
		if (strcmp(g_symbols[i].name, name) == 0) {
			void* known = g_symbols[i].address;
			pthread_mutex_unlock(&g_lock);
			return known;
		}
	}
	intptr_t address = kernel_dynlib_dlsym(getpid(), LIBKERNEL_HANDLE, name);
	if (address == 0) {
		char nid[12];
		nid_encode(name, nid);
		address = kernel_dynlib_resolve(getpid(), LIBKERNEL_HANDLE, nid);
	}
	if (g_count < MAX_SYMBOLS) {
		g_symbols[g_count].name = name;
		g_symbols[g_count].address = (void*)address;
		g_count++;
	}
	pthread_mutex_unlock(&g_lock);
	return (void*)address;
}
