/* Append-only text buffer that can never overflow.
 *
 * Collectors used to pass (out + len, cap - len) around and track their own
 * length, which overflows as soon as one collector returns a length past the
 * capacity: the next subtraction wraps around on size_t and the following
 * write lands outside the buffer. A single struct with its own bookkeeping
 * removes that class of bug: once the buffer is full every further append is
 * dropped and marked, and the caller can ask whether anything was lost. */
#ifndef BUF_H
#define BUF_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	char* data;      /* always NUL-terminated while len < cap */
	size_t cap;      /* bytes available, including the terminator */
	size_t len;      /* bytes used, never >= cap */
	bool truncated;  /* set once an append did not fit */
} Buf;

void buf_init(Buf* b, char* storage, size_t cap);
/* Appends a formatted string. Does nothing once the buffer is full. */
void buf_addf(Buf* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
/* Marks the position so a partially written metric family can be rolled
   back: buf_mark() returns the current length, buf_rewind() restores it. */
size_t buf_mark(const Buf* b);
void buf_rewind(Buf* b, size_t mark);

#endif
