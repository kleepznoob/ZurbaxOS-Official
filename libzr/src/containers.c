/* Dynamic arrays, sorting and the string-keyed hash map. */
#include "zr.h"

void vec_grow_(void **data, usize *cap, usize need, usize elem)
{
	if (need <= *cap)
		return;
	usize c = *cap ? *cap * 2 : 8;
	while (c < need)
		c *= 2;
	*data = mem_realloc(*data, c * elem);
	*cap = c;
}

/* ---- Stable merge sort ---------------------------------------------------- */

static void merge_sort(u8 *a, u8 *tmp, usize n, usize size, CmpFn cmp, void *ctx)
{
	if (n < 12) {
		/* Insertion sort for short runs. */
		for (usize i = 1; i < n; i++) {
			memcpy(tmp, a + i * size, size);
			usize j = i;
			while (j > 0 && cmp(a + (j - 1) * size, tmp, ctx) > 0) {
				memcpy(a + j * size, a + (j - 1) * size, size);
				j--;
			}
			memcpy(a + j * size, tmp, size);
		}
		return;
	}
	usize half = n / 2;
	merge_sort(a, tmp, half, size, cmp, ctx);
	merge_sort(a + half * size, tmp, n - half, size, cmp, ctx);
	if (cmp(a + (half - 1) * size, a + half * size, ctx) <= 0)
		return; /* already ordered */
	memcpy(tmp, a, half * size);
	usize i = 0, j = half, k = 0;
	while (i < half && j < n) {
		if (cmp(tmp + i * size, a + j * size, ctx) <= 0)
			memcpy(a + k++ * size, tmp + i++ * size, size);
		else
			memcpy(a + k++ * size, a + j++ * size, size);
	}
	while (i < half)
		memcpy(a + k++ * size, tmp + i++ * size, size);
}

void sort(void *base, usize n, usize size, CmpFn cmp, void *ctx)
{
	if (n < 2)
		return;
	u8 *tmp = mem_alloc((n / 2 + 1) * size);
	merge_sort(base, tmp, n, size, cmp, ctx);
	mem_free(tmp);
}

/* ---- Hash map (open addressing, linear probing) ---------------------------- */

u64 hash_bytes(const void *data, usize n)
{
	/* FNV-1a, 64-bit */
	const u8 *p = data;
	u64 h = 0xcbf29ce484222325ull;
	for (usize i = 0; i < n; i++) {
		h ^= p[i];
		h *= 0x100000001b3ull;
	}
	return h | 1; /* 0 marks an empty slot */
}

static MapSlot *find_slot(const Map *m, const char *key, u64 h)
{
	if (!m->cap)
		return NULL;
	usize mask = m->cap - 1;
	for (usize i = h & mask;; i = (i + 1) & mask) {
		MapSlot *s = &m->slots[i];
		if (!s->hash)
			return s;
		if (s->hash == h && str_eq(s->key, key))
			return s;
	}
}

void *map_get(const Map *m, const char *key)
{
	MapSlot *s = find_slot(m, key, hash_bytes(key, strlen(key)));
	return s && s->hash ? s->value : NULL;
}

bool map_has(const Map *m, const char *key)
{
	MapSlot *s = find_slot(m, key, hash_bytes(key, strlen(key)));
	return s && s->hash;
}

static void map_resize(Map *m, usize cap)
{
	MapSlot *old = m->slots;
	usize old_cap = m->cap;
	m->slots = NEW_ARRAY(MapSlot, cap);
	m->cap = cap;
	for (usize i = 0; i < old_cap; i++) {
		if (old[i].hash) {
			MapSlot *s = find_slot(m, old[i].key, old[i].hash);
			*s = old[i];
		}
	}
	mem_free(old);
}

void map_set(Map *m, const char *key, void *value)
{
	if ((m->len + 1) * 4 >= m->cap * 3)
		map_resize(m, m->cap ? m->cap * 2 : 16);
	u64 h = hash_bytes(key, strlen(key));
	MapSlot *s = find_slot(m, key, h);
	if (!s->hash) {
		s->hash = h;
		s->key = str_dup(key);
		m->len++;
	}
	s->value = value;
}

void *map_del(Map *m, const char *key)
{
	u64 h = hash_bytes(key, strlen(key));
	MapSlot *s = find_slot(m, key, h);
	if (!s || !s->hash)
		return NULL;
	void *old = s->value;
	mem_free(s->key);
	s->hash = 0;
	m->len--;
	/* Re-insert the rest of the probe chain so lookups stay correct. */
	usize mask = m->cap - 1;
	for (usize i = ((usize)(s - m->slots) + 1) & mask; m->slots[i].hash; i = (i + 1) & mask) {
		MapSlot moved = m->slots[i];
		m->slots[i].hash = 0;
		*find_slot(m, moved.key, moved.hash) = moved;
	}
	return old;
}

void map_free(Map *m, void (*free_value)(void *))
{
	for (usize i = 0; i < m->cap; i++) {
		if (m->slots[i].hash) {
			mem_free(m->slots[i].key);
			if (free_value)
				free_value(m->slots[i].value);
		}
	}
	mem_free(m->slots);
	m->slots = NULL;
	m->cap = m->len = 0;
}

bool map_next(const Map *m, usize *iter, const char **key, void **value)
{
	while (*iter < m->cap) {
		MapSlot *s = &m->slots[(*iter)++];
		if (s->hash) {
			if (key)
				*key = s->key;
			if (value)
				*value = s->value;
			return true;
		}
	}
	return false;
}
