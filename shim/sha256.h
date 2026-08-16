#ifndef NETPLAY_SHA256_H
#define NETPLAY_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	uint32_t h[8];
	uint64_t bytes;
	uint8_t block[64];
	size_t used;
} Sha256;

void sha256_init(Sha256* ctx);
void sha256_update(Sha256* ctx, const void* data, size_t len);
void sha256_final(Sha256* ctx, uint8_t out[32]);

#endif
