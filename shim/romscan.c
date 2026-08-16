#include "romscan.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <zlib.h>

/* Zip layout, as much of it as this needs. Only the central directory is read
 * during a search; the local header is touched once, for the one archive that
 * turned out to hold the cartridge. */
#define SIG_EOCD   0x06054b50u
#define SIG_CDIR   0x02014b50u
#define SIG_LOCAL  0x04034b50u

/* Almost no ROM archive carries an archive comment, so the end-of-central-
 * directory record sits in the last few dozen bytes. Reading a small tail and
 * only falling back to the 64KB a comment could require took a cold scan of
 * 2078 archives from 4.5s to 1.6s, and the bytes read from 122MB to 8MB. */
#define TAIL_SMALL 4096
#define TAIL_MAX   66000

static uint32_t rd32(const unsigned char* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const unsigned char* p) {
	return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static bool cartridge_name(const char* name, size_t len) {
	if (len > 4 && !strncasecmp(name + len - 4, ".gbc", 4)) return true;
	if (len > 3 && !strncasecmp(name + len - 3, ".gb", 3)) return true;
	if (len > 4 && !strncasecmp(name + len - 4, ".dmg", 4)) return true;
	return false;
}

static bool has_suffix(const char* name, const char* suffix) {
	const size_t n = strlen(name), s = strlen(suffix);
	return n > s && !strcasecmp(name + n - s, suffix);
}

bool romscan_zip_entry(const char* path, RomScanEntry* out) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	bool ok = false;
	unsigned char* buf = NULL;
	unsigned char* cd = NULL;

	if (fseek(f, 0, SEEK_END)) goto done;
	const long size = ftell(f);
	if (size < 22) goto done;

	long tail = size < TAIL_SMALL ? size : TAIL_SMALL;
	long eocd = -1;
	for (int pass = 0; pass < 2 && eocd < 0; pass++) {
		if (pass) {
			if (tail >= size) break;
			tail = size < TAIL_MAX ? size : TAIL_MAX;
		}
		free(buf);
		buf = malloc((size_t)tail);
		if (!buf) goto done;
		if (fseek(f, size - tail, SEEK_SET)) goto done;
		if (fread(buf, 1, (size_t)tail, f) != (size_t)tail) goto done;
		for (long i = tail - 22; i >= 0; i--)
			if (rd32(buf + i) == SIG_EOCD) { eocd = i; break; }
	}
	if (eocd < 0) goto done;

	const uint32_t cd_size = rd32(buf + eocd + 12);
	const uint32_t cd_off = rd32(buf + eocd + 16);
	/* A cartridge library's central directory is a few hundred bytes. The bound
	 * is here so a corrupt or hostile header cannot ask for an arbitrary
	 * allocation. */
	if (!cd_size || cd_size > 4u * 1024u * 1024u) goto done;
	cd = malloc(cd_size);
	if (!cd) goto done;
	if (fseek(f, (long)cd_off, SEEK_SET)) goto done;
	if (fread(cd, 1, cd_size, f) != cd_size) goto done;

	for (uint32_t p = 0; p + 46 <= cd_size && rd32(cd + p) == SIG_CDIR; ) {
		const uint16_t nlen = rd16(cd + p + 28);
		const uint16_t elen = rd16(cd + p + 30);
		const uint16_t clen = rd16(cd + p + 32);
		if ((uint64_t)p + 46 + nlen > cd_size) break;
		if (cartridge_name((const char*)(cd + p + 46), nlen)) {
			out->crc32 = rd32(cd + p + 16);
			out->compressed_size = rd32(cd + p + 20);
			out->uncompressed_size = rd32(cd + p + 24);
			out->method = rd16(cd + p + 10);
			out->local_header_offset = rd32(cd + p + 42);
			ok = true;
			break;
		}
		p += 46u + nlen + elen + clen;
	}

done:
	free(buf);
	free(cd);
	fclose(f);
	return ok;
}

bool romscan_zip_extract(const char* path, const RomScanEntry* entry,
                         void** out, size_t* out_len) {
	/* A Game Boy cartridge is at most 8MB; anything claiming more is not one,
	 * and inflating it would only be a way to exhaust memory. */
	if (!entry->uncompressed_size || entry->uncompressed_size > 8u * 1024u * 1024u)
		return false;
	FILE* f = fopen(path, "rb");
	if (!f) return false;

	bool ok = false;
	unsigned char* rom = NULL;
	unsigned char* packed = NULL;
	unsigned char local[30];

	if (fseek(f, (long)entry->local_header_offset, SEEK_SET)) goto done;
	if (fread(local, 1, sizeof(local), f) != sizeof(local)) goto done;
	if (rd32(local) != SIG_LOCAL) goto done;
	/* The local header repeats the name and extra lengths, and they may differ
	 * from the central directory's, so the data offset has to come from here. */
	if (fseek(f, (long)(rd16(local + 26) + rd16(local + 28)), SEEK_CUR)) goto done;

	rom = malloc(entry->uncompressed_size);
	if (!rom) goto done;

	if (entry->method == 0) {
		if (entry->compressed_size != entry->uncompressed_size) goto done;
		ok = fread(rom, 1, entry->uncompressed_size, f) == entry->uncompressed_size;
		goto done;
	}
	if (entry->method != 8) goto done;   /* deflate is the only thing in the wild */
	if (!entry->compressed_size || entry->compressed_size > 16u * 1024u * 1024u)
		goto done;

	packed = malloc(entry->compressed_size);
	if (!packed) goto done;
	if (fread(packed, 1, entry->compressed_size, f) != entry->compressed_size) goto done;

	{
		z_stream zs;
		memset(&zs, 0, sizeof(zs));
		/* Negative window bits: the zip member is a raw deflate stream with no
		 * zlib wrapper around it. */
		if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) goto done;
		zs.next_in = packed;
		zs.avail_in = entry->compressed_size;
		zs.next_out = rom;
		zs.avail_out = entry->uncompressed_size;
		const int rc = inflate(&zs, Z_FINISH);
		const uLong produced = zs.total_out;
		inflateEnd(&zs);
		ok = (rc == Z_STREAM_END) && produced == entry->uncompressed_size;
	}

done:
	free(packed);
	fclose(f);
	if (ok) {
		*out = rom;
		*out_len = entry->uncompressed_size;
	} else {
		free(rom);
	}
	return ok;
}

/*--------------------------------------------------------------------------
 * Cache
 *------------------------------------------------------------------------*/

/* Paths live in one packed pool rather than a fixed array per row. A row with
 * a 512-byte path field costs 4.6MB across a large library, nearly all of it
 * padding, and the doubling realloc that grows it needs old and new at once -
 * an unhelpful amount of memory to ask for on a handheld already running two
 * emulators. */
typedef struct {
	uint32_t path_off;       /* byte offset into the pool */
	uint32_t file_size;      /* of the archive, to notice a replaced file */
	uint32_t mtime;
	uint32_t uncompressed_size;
	uint32_t crc32;
	uint32_t local_header_offset;
	uint32_t compressed_size;
	uint16_t method;
	uint8_t  has_rom;        /* a zip with no cartridge is worth remembering */
} CacheRow;

/* Rows are fixed-size and the pool is append-only, so a row's path never
 * moves relative to the pool base - only the base itself can move. */

/* Open-addressed index over rows, rebuilt on load and maintained on insert.
 * The lookup used to be a linear scan, which is quadratic across a library:
 * eight thousand archives meant tens of millions of comparisons before the
 * search had read a single byte. */
struct RomScanCache {
	char path[512];
	CacheRow* rows;
	size_t count, cap;
	size_t added;
	bool dirty;
	uint32_t* index;      /* slot -> row + 1, zero for empty */
	size_t index_mask;
	char* pool;
	size_t pool_len, pool_cap;
};

static const char* row_path(const RomScanCache* c, const CacheRow* r) {
	return c->pool && r->path_off < c->pool_len ? c->pool + r->path_off : "";
}

/* Returns the offset, or SIZE_MAX if the pool could not grow. */
static size_t pool_put(RomScanCache* c, const char* path) {
	const size_t len = strlen(path) + 1;
	if (c->pool_len + len > c->pool_cap) {
		size_t cap = c->pool_cap ? c->pool_cap : 8192;
		while (cap < c->pool_len + len) cap *= 2;
		char* pool = realloc(c->pool, cap);
		if (!pool) return (size_t)-1;
		c->pool = pool;
		c->pool_cap = cap;
	}
	const size_t off = c->pool_len;
	memcpy(c->pool + off, path, len);
	c->pool_len += len;
	return off;
}

static uint32_t path_hash(const char* p) {
	uint32_t h = 2166136261u;
	for (; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
	return h ? h : 1u;
}

/* Bumped when the layout changed to a packed path pool. The cache is
 * disposable - a stale or unreadable one costs a rescan, nothing more - so
 * an old file is simply ignored rather than migrated. */
#define CACHE_MAGIC 0x324d4f52u  /* "ROM2" */

static void index_rebuild(RomScanCache* c) {
	free(c->index);
	c->index = NULL;
	c->index_mask = 0;
	size_t slots = 64;
	while (slots < (c->count + 1) * 2) slots *= 2;
	c->index = calloc(slots, sizeof(*c->index));
	if (!c->index) return;
	c->index_mask = slots - 1;
	for (size_t i = 0; i < c->count; i++) {
		size_t slot = path_hash(row_path(c, &c->rows[i])) & c->index_mask;
		while (c->index[slot]) slot = (slot + 1) & c->index_mask;
		c->index[slot] = (uint32_t)(i + 1);
	}
}

RomScanCache* romscan_cache_open(const char* path) {
	RomScanCache* c = calloc(1, sizeof(*c));
	if (!c) return NULL;
	snprintf(c->path, sizeof(c->path), "%s", path);

	FILE* f = fopen(path, "rb");
	if (!f) return c;
	uint32_t magic = 0, count = 0, pool_len = 0;
	if (fread(&magic, sizeof(magic), 1, f) == 1 && magic == CACHE_MAGIC &&
	    fread(&count, sizeof(count), 1, f) == 1 && count <= 200000u &&
	    fread(&pool_len, sizeof(pool_len), 1, f) == 1 &&
	    pool_len <= 64u * 1024u * 1024u) {
		c->rows = calloc(count ? count : 1, sizeof(*c->rows));
		c->pool = malloc(pool_len ? pool_len : 1);
		if (c->rows && c->pool) {
			c->cap = count;
			c->pool_cap = pool_len ? pool_len : 1;
			c->count = fread(c->rows, sizeof(*c->rows), count, f);
			c->pool_len = fread(c->pool, 1, pool_len, f);
			/* A truncated pool would leave rows pointing past its end. Reading
			 * fewer bytes than declared means the file is not trustworthy. */
			if (c->pool_len != pool_len || c->count != count) {
				c->count = 0;
				c->pool_len = 0;
			}
		} else {
			free(c->rows); c->rows = NULL;
			free(c->pool); c->pool = NULL;
		}
	}
	fclose(f);
	index_rebuild(c);
	return c;
}

void romscan_cache_close(RomScanCache* c) {
	if (!c) return;
	if (c->dirty && c->path[0]) {
		char tmp[600];
		snprintf(tmp, sizeof(tmp), "%s.tmp", c->path);
		FILE* f = fopen(tmp, "wb");
		if (f) {
			const uint32_t magic = CACHE_MAGIC;
			const uint32_t count = (uint32_t)c->count;
			const uint32_t pool_len = (uint32_t)c->pool_len;
			bool ok = fwrite(&magic, sizeof(magic), 1, f) == 1 &&
			          fwrite(&count, sizeof(count), 1, f) == 1 &&
			          fwrite(&pool_len, sizeof(pool_len), 1, f) == 1 &&
			          fwrite(c->rows, sizeof(*c->rows), c->count, f) == c->count &&
			          fwrite(c->pool, 1, c->pool_len, f) == c->pool_len;
			ok = (fclose(f) == 0) && ok;
			/* Rename only a complete file: a cache truncated by a battery pull
			 * would otherwise be read back as authoritative. */
			if (ok) rename(tmp, c->path); else remove(tmp);
		}
	}
	free(c->rows);
	free(c->index);
	free(c->pool);
	free(c);
}

size_t romscan_cache_count(const RomScanCache* c) { return c ? c->count : 0; }
size_t romscan_cache_added(const RomScanCache* c) { return c ? c->added : 0; }

static CacheRow* cache_lookup(RomScanCache* c, const char* path,
                              uint32_t file_size, uint32_t mtime) {
	if (!c->index) return NULL;
	size_t slot = path_hash(path) & c->index_mask;
	while (c->index[slot]) {
		CacheRow* r = &c->rows[c->index[slot] - 1];
		if (!strcmp(row_path(c, r), path)) {
			/* Same path, different size or mtime: the file was replaced, so the
			 * remembered answer is wrong and the caller must read it again. */
			return (r->file_size == file_size && r->mtime == mtime) ? r : NULL;
		}
		slot = (slot + 1) & c->index_mask;
	}
	return NULL;
}

static CacheRow* cache_add(RomScanCache* c, const char* path,
                           uint32_t file_size, uint32_t mtime,
                           const RomScanEntry* entry) {
	if (c->count == c->cap) {
		const size_t cap = c->cap ? c->cap * 2 : 256;
		CacheRow* rows = realloc(c->rows, cap * sizeof(*rows));
		if (!rows) return NULL;
		c->rows = rows;
		c->cap = cap;
	}
	const size_t row_index = c->count++;
	CacheRow* r = &c->rows[row_index];
	memset(r, 0, sizeof(*r));
	const size_t off = pool_put(c, path);
	if (off == (size_t)-1) { c->count--; return NULL; }
	r->path_off = (uint32_t)off;
	r->file_size = file_size;
	r->mtime = mtime;
	if (entry) {
		r->has_rom = 1;
		r->uncompressed_size = entry->uncompressed_size;
		r->crc32 = entry->crc32;
		r->local_header_offset = entry->local_header_offset;
		r->compressed_size = entry->compressed_size;
		r->method = entry->method;
	}
	c->added++;
	c->dirty = true;
	/* Keep the index usable as the table grows; a rebuild is cheap next to the
	 * directory read that produced this row. */
	if (!c->index || (c->count + 1) * 2 > c->index_mask + 1) {
		index_rebuild(c);
	} else {
		size_t slot = path_hash(row_path(c, r)) & c->index_mask;
		while (c->index[slot]) slot = (slot + 1) & c->index_mask;
		c->index[slot] = (uint32_t)(row_index + 1);
	}
	return r;
}

static uint64_t now_ms(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)tv.tv_usec / 1000ull;
}

typedef struct {
	RomScanCache* cache;
	const uint8_t* want_sha;
	uint32_t want_size;
	uint32_t want_crc;
	char* out_path;
	size_t out_len;
	bool (*sha256_of)(const void*, size_t, uint8_t[32]);
	RomScanEntry* out_entry;
	bool* out_zipped;
	bool found;
	uint64_t deadline_ms;
	bool expired;
	size_t examined;
} Search;

/* The clock is consulted every so many files rather than every file: on a
 * healthy card the check would cost more than the work it guards. */
#define DEADLINE_CHECK_EVERY 32

static void search_dir(Search* s, const char* dir, unsigned depth) {
	if (s->found || s->expired || depth > 4) return;
	DIR* d = opendir(dir);
	if (!d) return;
	struct dirent* e;
	while (!s->found && !s->expired && (e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		if (++s->examined % DEADLINE_CHECK_EVERY == 0 &&
		    s->deadline_ms && now_ms() > s->deadline_ms) {
			s->expired = true;
			break;
		}
		char path[512];
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= sizeof(path))
			continue;
		struct stat st;
		if (stat(path, &st)) continue;
		if (S_ISDIR(st.st_mode)) { search_dir(s, path, depth + 1); continue; }
		if (!S_ISREG(st.st_mode)) continue;

		RomScanEntry entry;
		bool have = false;

		if (has_suffix(e->d_name, ".zip")) {
			const uint32_t fsize = (uint32_t)st.st_size;
			const uint32_t mtime = (uint32_t)st.st_mtime;
			CacheRow* row = s->cache ? cache_lookup(s->cache, path, fsize, mtime) : NULL;
			if (!row) {
				/* New or changed since the cache was written: read it now and
				 * remember it, which is what keeps the next session cheap. */
				RomScanEntry fresh;
				const bool got = romscan_zip_entry(path, &fresh);
				if (s->cache) row = cache_add(s->cache, path, fsize, mtime,
				                              got ? &fresh : NULL);
				if (!got) continue;
				entry = fresh;
				have = true;
			} else if (row->has_rom) {
				entry.uncompressed_size = row->uncompressed_size;
				entry.crc32 = row->crc32;
				entry.local_header_offset = row->local_header_offset;
				entry.compressed_size = row->compressed_size;
				entry.method = row->method;
				have = true;
			}
			if (!have) continue;
			if (entry.uncompressed_size != s->want_size || entry.crc32 != s->want_crc)
				continue;

			/* Both cheap filters passed, so this is almost certainly it. Inflate
			 * once and let SHA-256 have the final word. */
			void* rom = NULL;
			size_t rom_len = 0;
			if (!romscan_zip_extract(path, &entry, &rom, &rom_len)) continue;
			uint8_t got[32];
			const bool match = s->sha256_of(rom, rom_len, got) &&
			                   !memcmp(got, s->want_sha, 32);
			free(rom);
			if (!match) continue;
			snprintf(s->out_path, s->out_len, "%s", path);
			if (s->out_entry) *s->out_entry = entry;
			if (s->out_zipped) *s->out_zipped = true;
			s->found = true;
			continue;
		}

		/* A bare cartridge is what it says it is. */
		if (!cartridge_name(e->d_name, strlen(e->d_name))) continue;
		if ((uint32_t)st.st_size != s->want_size) continue;
		FILE* f = fopen(path, "rb");
		if (!f) continue;
		void* rom = malloc(s->want_size);
		const bool read_ok = rom && fread(rom, 1, s->want_size, f) == s->want_size;
		fclose(f);
		if (read_ok) {
			uint8_t got[32];
			if (s->sha256_of(rom, s->want_size, got) &&
			    !memcmp(got, s->want_sha, 32)) {
				snprintf(s->out_path, s->out_len, "%s", path);
				if (s->out_zipped) *s->out_zipped = false;
				s->found = true;
			}
		}
		free(rom);
	}
	closedir(d);
}

RomScanResult romscan_find(RomScanCache* cache, const char* const* dirs,
                  const uint8_t sha256[32], uint32_t size, uint32_t crc32,
                  char* out_path, size_t out_len,
                  RomScanEntry* out_entry, bool* out_zipped,
                  unsigned budget_ms, size_t* out_examined,
                  bool (*sha256_of)(const void*, size_t, uint8_t[32])) {
	Search s;
	memset(&s, 0, sizeof(s));
	s.cache = cache;
	s.want_sha = sha256;
	s.want_size = size;
	s.want_crc = crc32;
	s.out_path = out_path;
	s.out_len = out_len;
	s.sha256_of = sha256_of;
	s.out_entry = out_entry;
	s.out_zipped = out_zipped;
	s.deadline_ms = budget_ms ? now_ms() + budget_ms : 0;
	for (size_t i = 0; dirs && dirs[i] && !s.found && !s.expired; i++)
		search_dir(&s, dirs[i], 0);
	if (out_examined) *out_examined = s.examined;
	if (s.found) return ROMSCAN_FOUND;
	return s.expired ? ROMSCAN_TIMED_OUT : ROMSCAN_MISSING;
}
