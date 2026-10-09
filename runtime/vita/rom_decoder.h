#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef intptr_t (*MH3RomReadAt)(void*,uint64_t,uint8_t*,size_t);
void* mh3_rom_open(void* context,uint64_t physical_size,MH3RomReadAt read_at,uint8_t* error,size_t capacity);
uint64_t mh3_rom_size(void* reader,int flat_partition);
int mh3_rom_read(void* reader,int flat_partition,uint64_t offset,uint8_t* bytes,size_t count,uint8_t* error,size_t capacity);
void mh3_rom_close(void* reader);
#ifdef __cplusplus
}
#endif
