// symbol_lookup.h — runtime address -> recompiled-function-name lookup.
//
// Turns raw guest PCs in debug output (trace dumps, dispatch-miss reports,
// TCP) into `<FunctionName+0xoffset>`. Names come from the decomp symbols
// the recompiler seeds into generated code; the recompiler emits a sorted
// address->name table (symbol_map.cpp / bios_symbol_map.cpp) whose static
// initializer REGISTERS itself here at startup. Registration is used rather
// than weak externs because MinGW PE-COFF weak symbols don't reliably
// resolve from static archives (see runtime_arm.cpp header). A binary
// without a generated map simply never registers one and the lookup
// returns nullptr (no annotation) — graceful, and no link dependency.
#pragma once

#include <cstdint>

struct GbaSymbol { uint32_t addr; const char* name; };

extern "C" {

// Returns the name of the nearest recompiled function whose entry address
// is <= pc, setting *out_offset to (pc - entry_addr). Returns nullptr when
// no symbol map is registered, or when pc precedes the first known entry.
const char* gba_symbol_lookup(uint32_t pc, uint32_t* out_offset);

// Called by the generated symbol_map.cpp / bios_symbol_map.cpp static
// initializers. `tab` must be sorted ascending by addr and live for the
// program's lifetime (it is static data in the generated TU).
void gba_symbol_register_cart(const GbaSymbol* tab, unsigned count);
void gba_symbol_register_bios(const GbaSymbol* tab, unsigned count);

}  // extern "C"
