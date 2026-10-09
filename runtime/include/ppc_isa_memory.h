#pragma once

// Host memory seam for isa/ppc_isa_quantized.h. That header includes
// "ppc_isa_memory.h" by plain quoted name and expects whatever it resolves to
// to supply the Memory / MemoryInline APIs and the flat-guest-base macros its
// psq fast paths use. This file is this project's implementation of that
// contract; memory.h chains memory_access.h, which defines MemoryInline.

#include "memory.h" // chains memory_access.h, which supplies MemoryInline and
                    // includes guest_flat_memory.h for the flat-base macros
