#include "buf.h"

#include <stdio.h>

void buf_init(Buf* b, char* storage, size_t cap)
{
	b->data = storage;
	b->cap = cap;
	b->len = 0;
	b->truncated = false;
	if (cap > 0) storage[0] = 0;
}

void buf_addf(Buf* b, const char* fmt, ...)
{
	if (b->cap == 0 || b->len + 1 >= b->cap) {
		b->truncated = true;
		return;
	}
	size_t room = b->cap - b->len; /* includes space for the terminator */
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b->data + b->len, room, fmt, ap);
	va_end(ap);
	if (n < 0) {
		b->truncated = true;
		b->data[b->len] = 0;
		return;
	}
	if ((size_t)n >= room) {
		/* Did not fit: drop the partial write so the output stays valid. */
		b->data[b->len] = 0;
		b->truncated = true;
		return;
	}
	b->len += (size_t)n;
}

size_t buf_mark(const Buf* b)
{
	return b->len;
}

void buf_rewind(Buf* b, size_t mark)
{
	if (mark <= b->len && mark < b->cap) {
		b->len = mark;
		b->data[mark] = 0;
	}
}
