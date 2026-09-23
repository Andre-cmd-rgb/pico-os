/*
 * Process memory. Every pt_malloc block carries a small header linking it
 * into its owner's list, so exit frees whatever a program leaked. Blocks come
 * from PSRAM when there is some, keeping the 300 KB of internal RAM for
 * stacks, drivers and DMA buffers.
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "pt/kernel.h"

struct alloc_hdr {
	struct alloc_hdr	*next;
	struct alloc_hdr	*prev;
	struct proc		*owner;
	size_t			 size;
};

#define PREFER_PSRAM	2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT

/* A block with its header, linked into the calling process's list. */
static void *track(struct alloc_hdr *h, size_t n)
{
	struct proc *p = proc_current();

	if (!h)
		return NULL;
	h->owner = p;
	h->size = n;
	h->prev = NULL;
	h->next = p ? p->allocs : NULL;
	if (p) {
		if (p->allocs)
			p->allocs->prev = h;
		p->allocs = h;
	}
	return h + 1;
}

void *pt_malloc(size_t n)
{
	if (n > SIZE_MAX - sizeof(struct alloc_hdr))
		return NULL;
	return track(heap_caps_malloc_prefer(sizeof(struct alloc_hdr) + n, PREFER_PSRAM), n);
}

void *pt_malloc_caps(size_t n, uint32_t caps)
{
	if (n > SIZE_MAX - sizeof(struct alloc_hdr))
		return NULL;
	return track(heap_caps_malloc(sizeof(struct alloc_hdr) + n, caps), n);
}

void pt_free(void *ptr)
{
	if (!ptr)
		return;
	struct alloc_hdr *h = (struct alloc_hdr *)ptr - 1;

	if (h->owner) {
		if (h->prev)
			h->prev->next = h->next;
		else
			h->owner->allocs = h->next;
		if (h->next)
			h->next->prev = h->prev;
	}
	heap_caps_free(h);
}

void *pt_calloc(size_t count, size_t n)
{
	if (n && count > SIZE_MAX / n)
		return NULL;
	void *p = pt_malloc(count * n);

	if (p)
		memset(p, 0, count * n);
	return p;
}

/* Grows in place when the heap can, so a string or array that keeps
 * growing is not copied every time. */
void *pt_realloc(void *ptr, size_t n)
{
	if (!ptr)
		return pt_malloc(n);
	if (n > SIZE_MAX - sizeof(struct alloc_hdr))
		return NULL;
	struct alloc_hdr *h = heap_caps_realloc_prefer((struct alloc_hdr *)ptr - 1,
						       sizeof(*h) + n, PREFER_PSRAM);

	if (!h)
		return NULL;			/* the old block is untouched */
	h->size = n;
	if (h->owner) {
		/* the block may have moved: its neighbours still point at the old one */
		if (h->prev)
			h->prev->next = h;
		else
			h->owner->allocs = h;
		if (h->next)
			h->next->prev = h;
	}
	return h + 1;
}

char *pt_strdup(const char *s)
{
	size_t len = strlen(s) + 1;
	char *d = pt_malloc(len);

	if (d)
		memcpy(d, s, len);
	return d;
}

void mem_release_all(struct proc *p)
{
	struct alloc_hdr *h = p->allocs;

	while (h) {
		struct alloc_hdr *next = h->next;
		heap_caps_free(h);
		h = next;
	}
	p->allocs = NULL;
}
