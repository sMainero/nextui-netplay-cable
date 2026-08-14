#ifndef NETPLAY_ROMSCAN_H
#define NETPLAY_ROMSCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Finding the peer's cartridge in a local library.
 *
 * Two devices playing linked but different games - Oracle of Seasons against
 * Ages, Gold against Silver - each have to load the other's cartridge locally,
 * because the pair is mirrored on both devices and neither ships a ROM over
 * the wire. So each side must find, among its own files, the one the peer
 * described.
 *
 * The peer describes it by the SHA-256 of the *uncompressed* ROM, because that
 * is what the frontend hands the core and therefore what both sides can agree
 * on. A library is usually zipped, so the file on disk is neither that size nor
 * those bytes, and a search that compares file sizes finds nothing at all.
 *
 * Decompressing candidates to check is not affordable: Game Boy ROM sizes are
 * powers of two, so hundreds of cartridges share an uncompressed size. On one
 * real library that would have meant inflating well over a hundred 2MB ROMs per
 * session.
 *
 * A zip's central directory already records the uncompressed size and a CRC32
 * of the uncompressed data, and reading it decompresses nothing. Filtering on
 * both leaves, in practice, exactly one candidate - measured across 2078 Game
 * Boy and Game Boy Color cartridges, no two shared a (size, CRC32) pair. That
 * one candidate is inflated and its SHA-256 checked, so the CRC32 is only ever
 * a filter and never the decision.
 */

typedef struct {
	uint32_t uncompressed_size;
	uint32_t crc32;
	/* Where inside the archive, so the extractor need not search again. */
	uint32_t local_header_offset;
	uint16_t method;              /* 0 = stored, 8 = deflate */
	uint32_t compressed_size;
} RomScanEntry;

/* Reads a zip's central directory and returns the first entry whose name looks
 * like a Game Boy cartridge. Decompresses nothing. False if the file is not a
 * zip, holds no cartridge, or cannot be read. */
bool romscan_zip_entry(const char* path, RomScanEntry* out);

/* Inflates that entry. Caller frees *out. */
bool romscan_zip_extract(const char* path, const RomScanEntry* entry,
                         void** out, size_t* out_len);

/* The cache. Reading a central directory costs about 0.8ms per archive, which
 * is 1.6s over a couple of thousand cartridges on a cold page cache - fine
 * once, wasteful every session. Entries are keyed by path, size and mtime, so
 * a replaced or edited file is re-read rather than trusted. */
typedef struct RomScanCache RomScanCache;

RomScanCache* romscan_cache_open(const char* path);
/* Writes only if anything changed. */
void romscan_cache_close(RomScanCache* cache);
size_t romscan_cache_count(const RomScanCache* cache);
size_t romscan_cache_added(const RomScanCache* cache);

typedef enum {
	ROMSCAN_FOUND = 0,
	ROMSCAN_MISSING,     /* searched everything asked of it; not here */
	ROMSCAN_TIMED_OUT,   /* ran out of budget - says nothing about presence */
} RomScanResult;

/* Looks up a cartridge by what the peer told us about it. `dirs` is a
 * NULL-terminated list searched in order, so the caller can try the Game Boy
 * folders before the whole library.
 *
 * `budget_ms` bounds the whole search. It has to: this runs inside a bootstrap
 * the peer is waiting on, so a library on a slow or damaged card would
 * otherwise stall both devices with no way to tell a long search from a hang.
 * Running out of budget is reported distinctly from "not installed", because
 * the two mean different things to a player. */
RomScanResult romscan_find(RomScanCache* cache, const char* const* dirs,
                  const uint8_t sha256[32], uint32_t size, uint32_t crc32,
                  char* out_path, size_t out_len,
                  RomScanEntry* out_entry, bool* out_zipped,
                  unsigned budget_ms, size_t* out_examined,
                  bool (*sha256_of)(const void* data, size_t len, uint8_t out[32]));

#endif
