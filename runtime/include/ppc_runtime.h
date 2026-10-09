// The PowerPC ISA package (runtime/include/isa/), re-exported under the single
// header name the generated code and the runtime include. The package has two
// host seams this project satisfies elsewhere: "ppc_isa_memory.h" (pulled by
// the quantized tier) and the ShowRuntimeFatalPopup implementation.

#pragma once

#include "isa/ppc_isa_config.h"
#include "isa/big_endian.h"
#include "isa/ppc_isa_fpenv.h"
#include "isa/ppc_isa_context.h"
#include "isa/ppc_isa_int.h"
#include "isa/ppc_isa_float.h"
#include "isa/ppc_isa_quantized.h"

// Guest lwz instructions listed in translation.load_hooks_path load through
// the runtime (step_counter.cpp): base + offset is the guest address, pc the
// instruction.
extern "C" uint32_t MH3_TickLoad(uint32_t value, uint32_t pc);
extern "C" double MH3_EaseDivisor(double d);
extern "C" double MH3_PerStep(double amount);
extern "C" double MH3_Decay(double factor);
extern "C" uint32_t MH3_HookedLoad32(uint32_t base, int32_t offset, uint32_t pc);
