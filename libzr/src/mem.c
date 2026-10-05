/*
 * Raw memory routines and the heap allocator.
 *
 * Small blocks (up to 32 KiB) come from size classes carved out of 1 MiB
 * arenas and are recycled through per-class free lists. Larger blocks are
 * mapped directly from the kernel and returned to it when freed. Every block
 * carries a 16-byte header so that user pointers stay 16-byte aligned.
 */
#include "zr.h"

/* ---- Raw memory ----------------------------------------------------------- */

void *memcpy(void *restrict dst, const void *restrict src, usize n)
{
	void *d = dst;
	__asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
	return dst;
}

void *memmove(void *dst, const void *src, usize n)
{
	u8 *d = dst;
	const u8 *s = src;
	if (d == s || n == 0)
		return dst;
	if (d < s || d >= s + n)
		return memcpy(dst, src, n);
	/* Overlapping with dst after src: copy backwards. */
	d += n - 1;
	s += n - 1;
	__asm__ volatile("std\n\trep movsb\n\tcld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
	return dst;
}

void *memset(void *dst, int c, usize n)
{
	void *d = dst;
	__asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
	return dst;
}

int memcmp(const void *a, const void *b, usize n)
{
	const u8 *x = a, *y = b;
	for (usize i = 0; i < n; i++)
		if (x[i] != y[i])
			return x[i] - y[i];
	return 0;
}

usize strlen(const char *s)
{
	const char *p = s;
	while (*p)
		p++;
	return (usize)(p - s);
}

/* ---- Heap --------------------------------------------------------------------- */

#define HEADER 16u
#define LARGE_CLASS 0xffffu
#define ARENA_SIZE (1u << 20)
#define HEAP_MAGIC 0x5a52u /* "ZR" */

typedef struct {
	u16 cls;
	u16 magic;
	u32 pad;
	u64 size; /* usable bytes */
} Header;

typedef struct FreeBlock {
	struct FreeBlock *next;
} FreeBlock;

static const u32 class_size[] = {
	16, 32, 48, 64, 80, 96, 128, 160, 192, 256, 320, 384, 512, 640, 768, 1024,
	1280, 1536, 2048, 2560, 3072, 4096, 5120, 6144, 8192, 10240, 12288, 16384,
	20480, 24576, 32768,
};
#define NUM_CLASSES ARRAY_LEN(class_size)

static FreeBlock *free_list[NUM_CLASSES];
static u8 *arena_ptr, *arena_end;
static Mutex heap_lock;

static bool map_failed(const void *p)
{
	return (unsigned long)p > -4096ul;
}

static int class_for(usize n)
{
	for (unsigned i = 0; i < NUM_CLASSES; i++)
		if (n <= class_size[i])
			return (int)i;
	return -1;
}

NORETURN static void out_of_memory(usize n)
{
	fd_print(2, "fatal: out of memory (%zu bytes)\n", n);
	exit(127);
}

static void *carve(usize total)
{
	if ((usize)(arena_end - arena_ptr) < total) {
		u8 *a = sys_mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (map_failed(a))
			out_of_memory(total);
		arena_ptr = a;
		arena_end = a + ARENA_SIZE;
	}
	void *p = arena_ptr;
	arena_ptr += total;
	return p;
}

void *mem_alloc(usize n)
{
	if (n == 0)
		n = 1;
	int c = class_for(n);
	Header *h;
	if (c < 0) {
		usize total = ALIGN_UP(n + HEADER, 4096u);
		void *m = sys_mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (map_failed(m))
			out_of_memory(n);
		h = m;
		h->cls = LARGE_CLASS;
		h->size = total - HEADER;
	} else {
		mutex_lock(&heap_lock);
		FreeBlock *b = free_list[c];
		if (b) {
			free_list[c] = b->next;
			h = (Header *)((u8 *)b - HEADER);
		} else {
			h = carve(class_size[c] + HEADER);
		}
		mutex_unlock(&heap_lock);
		h->cls = (u16)c;
		h->size = class_size[c];
	}
	h->magic = HEAP_MAGIC;
	return (u8 *)h + HEADER;
}

void *mem_zalloc(usize n)
{
	void *p = mem_alloc(n);
	Header *h = (Header *)((u8 *)p - HEADER);
	/* Fresh mappings are already zero; only recycled small blocks need clearing. */
	memset(p, 0, h->cls == LARGE_CLASS ? n : h->size);
	return p;
}

static Header *header_of(const void *p)
{
	Header *h = (Header *)((u8 *)p - HEADER);
	if (unlikely(h->magic != HEAP_MAGIC))
		die("heap corruption or invalid free at %p", p);
	return h;
}

void mem_free(void *p)
{
	if (!p)
		return;
	Header *h = header_of(p);
	h->magic = 0;
	if (h->cls == LARGE_CLASS) {
		sys_munmap(h, h->size + HEADER);
		return;
	}
	FreeBlock *b = p;
	mutex_lock(&heap_lock);
	b->next = free_list[h->cls];
	free_list[h->cls] = b;
	mutex_unlock(&heap_lock);
}

usize mem_usable(const void *p)
{
	return p ? header_of(p)->size : 0;
}

void *mem_realloc(void *p, usize n)
{
	if (!p)
		return mem_alloc(n);
	Header *h = header_of(p);
	if (n <= h->size && (h->cls == LARGE_CLASS || h->cls == 0 || n > class_size[h->cls - 1]))
		return p;
	if (h->cls == LARGE_CLASS && n > 32768) {
		usize total = ALIGN_UP(n + HEADER, 4096u);
		Header *m = sys_mremap(h, h->size + HEADER, total, MREMAP_MAYMOVE);
		if (map_failed(m))
			out_of_memory(n);
		m->size = total - HEADER;
		return (u8 *)m + HEADER;
	}
	void *q = mem_alloc(n);
	memcpy(q, p, MIN(n, h->size));
	mem_free(p);
	return q;
}

void *mem_dup(const void *p, usize n)
{
	void *q = mem_alloc(n);
	memcpy(q, p, n);
	return q;
}
