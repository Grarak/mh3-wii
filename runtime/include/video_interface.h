#pragma once
#include <cstdint>

void ResetVideoInterface();
bool ReadVideoRegister16(uint32_t address, uint16_t& value);
bool WriteVideoRegister16(uint32_t address, uint16_t value);
uint64_t VideoRetracePeriodNs();
void BeginVideoRetrace();
void EndVideoRetrace();

struct CpuContext;
void ServiceVideoRetrace(CpuContext* cpu);
void RunVideoTimingSelfTests();
