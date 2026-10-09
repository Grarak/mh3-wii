#pragma once

#include "../../internal.hpp"
#include "../../gx/gx.hpp"

// Defined once in GXAurora.cpp. The FATAL/CHECK/TRY macros resolve `Log` unqualified, so it stays
// at global scope like the rest of this header's using-declarations.
extern aurora::Module Log;

using aurora::gx::g_gxState;
