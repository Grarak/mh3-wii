#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef intptr_t (*MH3RomReadAt)(void*, uint64_t, uint8_t*, size_t);
void* mh3_rom_open(void* context, uint64_t physical_size, MH3RomReadAt read_at, uint8_t* error, size_t capacity);
uint64_t mh3_rom_size(void* reader, int flat_partition);
int mh3_rom_read(void* reader, int flat_partition, uint64_t offset, uint8_t* bytes, size_t count, uint8_t* error, size_t capacity);
/* Starts a worker thread that decodes the group a read needs next while the
 * reading thread decodes the current one (WIA/RVZ); thread_start, if set, runs
 * first on it. Returns 1 when it started. */
int mh3_rom_read_ahead(void* reader, void (*thread_start)(void));
void mh3_rom_close(void* reader);
/* SHA-1 of size bytes at data into digest (20 bytes). */
void mh3_sha1(const void* data, size_t size, uint8_t* digest);
#ifdef __cplusplus
}
#endif
