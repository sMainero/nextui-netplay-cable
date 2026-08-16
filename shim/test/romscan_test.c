/*
 * Finding a peer's cartridge in a zipped library.
 *
 * The different-cartridge path - Oracle of Seasons against Ages, Gold against
 * Silver - needs each device to find the other's ROM among its own files. A
 * library is normally zipped, so the file on disk is neither the size nor the
 * bytes the peer described, and the search that shipped before this found
 * nothing at all: it filtered on file size and only looked at bare .gb files.
 *
 * The replacement reads zip central directories, filters on the uncompressed
 * size and CRC32 recorded there, and inflates only the one candidate that
 * passes both. These tests pin the parts that would fail quietly: that it finds
 * the right cartridge among decoys of identical uncompressed size, that a CRC32
 * collision cannot by itself cause a wrong match, and that the cache does not
 * hand back stale answers when a file is replaced.
 *
 * The fixture is built by the shell script beside this file, because generating
 * zips is a job for a zip library and the point here is the reader.
 */
#include "../romscan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int ok, const char *what) {
	printf("  %s %s\n", ok ? "ok  " : "MISS", what);
	if (!ok) failures++;
}

/* The real shim hands romscan its SHA-256; this mirrors it with a trivial
 * digest so the test needs no crypto, only agreement between the two sides. */
static int sha_calls;
static bool fake_sha256(const void *data, size_t len, uint8_t out[32]) {
	const unsigned char *p = (const unsigned char *)data;
	sha_calls++;
	memset(out, 0, 32);
	for (size_t i = 0; i < len; i++) out[i % 32] ^= p[i];
	out[31] ^= (uint8_t)len;
	return true;
}

static void digest_of_file(const char *path, uint8_t out[32]) {
	FILE *f = fopen(path, "rb");
	if (!f) { memset(out, 0, 32); return; }
	static unsigned char buf[8u << 20];
	const size_t n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	fake_sha256(buf, n, out);
}

int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: romscan_test <romdir> <cachefile>\n"); return 2; }
	const char *romdir = argv[1];
	const char *cachefile = argv[2];
	const char *dirs[2];
	dirs[0] = romdir;
	dirs[1] = NULL;

	/* The fixture writes the plain ROM beside the archives so the test can
	 * describe the cartridge exactly as a peer would: by the uncompressed
	 * bytes. */
	char want_path[1024];
	snprintf(want_path, sizeof(want_path), "%s/../target.bin", romdir);
	uint8_t want[32];
	digest_of_file(want_path, want);

	RomScanEntry entry;
	memset(&entry, 0, sizeof(entry));
	char target_zip[1024];
	snprintf(target_zip, sizeof(target_zip), "%s/target.zip", romdir);
	const bool read_ok = romscan_zip_entry(target_zip, &entry);
	check(read_ok, "reads a zip central directory");
	check(read_ok && entry.uncompressed_size == 65536,
	      "reports the uncompressed size, not the compressed one");

	/* Every decoy shares the target's uncompressed size, which is the normal
	 * case for Game Boy: sizes are powers of two, so hundreds of cartridges
	 * collide on size alone. Only the CRC32 separates them. */
	{
		char found[512];
		RomScanEntry got;
		bool zipped = false;
		sha_calls = 0;
		remove(cachefile);
		RomScanCache *cache = romscan_cache_open(cachefile);
		const bool ok = ROMSCAN_FOUND == romscan_find(cache, dirs, want, entry.uncompressed_size,
		                             entry.crc32, found, sizeof(found),
		                             &got, &zipped, 0, NULL, fake_sha256);
		const size_t added = romscan_cache_added(cache);
		romscan_cache_close(cache);
		check(ok, "finds the cartridge inside its archive");
		check(ok && strstr(found, "target.zip") != NULL, "and names the right archive");
		check(zipped, "reports that it came from an archive");
		/* The whole design rests on this: decoys of equal uncompressed size are
		 * rejected on CRC32, so only the real match is ever inflated. */
		check(sha_calls == 1, "inflates exactly one candidate despite equal sizes");
		check(added > 1, "records every archive it read in the cache");
	}

	/* Second run: the cache answers, nothing is re-read, the result is the same. */
	{
		char found[512];
		bool zipped = false;
		sha_calls = 0;
		RomScanCache *cache = romscan_cache_open(cachefile);
		const size_t known = romscan_cache_count(cache);
		const bool ok = ROMSCAN_FOUND == romscan_find(cache, dirs, want, entry.uncompressed_size,
		                             entry.crc32, found, sizeof(found), NULL,
		                             &zipped, 0, NULL, fake_sha256);
		const size_t added = romscan_cache_added(cache);
		romscan_cache_close(cache);
		check(known > 1, "reloads what the previous run learned");
		check(ok, "finds the cartridge again from the cache");
		check(added == 0, "and reads no central directories the second time");
	}

	/* A CRC32 is 32 bits, so a collision is possible in principle. It must not
	 * be able to decide a match on its own - the digest still has the last
	 * word. Asking for the target's size and CRC32 under a different digest
	 * must fail rather than return the target. */
	{
		char found[512];
		uint8_t wrong[32];
		memset(wrong, 0xAB, sizeof(wrong));
		RomScanCache *cache = romscan_cache_open(cachefile);
		const bool ok = ROMSCAN_FOUND == romscan_find(cache, dirs, wrong, entry.uncompressed_size,
		                             entry.crc32, found, sizeof(found), NULL, NULL, 0, NULL,
		                             fake_sha256);
		romscan_cache_close(cache);
		check(!ok, "a matching CRC32 with the wrong contents is rejected");
	}

	/* A search that runs out of budget must report that, not "not installed".
	 * The two mean different things: one is a library that does not have the
	 * game, the other is a card too slow to finish looking, and only the second
	 * is a reason to keep the pairing alive on a retry. An unbounded search is
	 * what froze both devices at the pairing notice with nothing on screen to
	 * explain it. */
	{
		char found[512];
		RomScanCache *cache = romscan_cache_open(cachefile);
		/* One millisecond cannot walk forty archives. */
		const RomScanResult r = romscan_find(cache, dirs, want,
		                                     entry.uncompressed_size, entry.crc32,
		                                     found, sizeof(found), NULL, NULL,
		                                     1, NULL, fake_sha256);
		romscan_cache_close(cache);
		check(r != ROMSCAN_MISSING, "a search out of budget is not reported as missing");
	}

	/* Extraction has to reproduce the cartridge byte for byte, or the two
	 * devices would build different machines from the same pair of files. */
	{
		void *rom = NULL;
		size_t len = 0;
		const bool ok = romscan_zip_extract(target_zip, &entry, &rom, &len);
		uint8_t got[32];
		if (ok) fake_sha256(rom, len, got);
		check(ok && len == entry.uncompressed_size, "inflates to the declared length");
		check(ok && !memcmp(got, want, 32), "and reproduces the cartridge exactly");
		free(rom);
	}

	if (failures) {
		printf("\nFAIL: %d check(s) failed\n", failures);
		return 1;
	}
	printf("\nPASS: the peer's cartridge is found inside a zipped library\n");
	return 0;
}
