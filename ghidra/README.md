# ghidra/ — disassembly projects for the DS BIOSes (and later the menu)

Ghidra is **reference only** — symbols, function boundaries, jump-table
shapes, and seeds for the recompiler's discovery. It is **never** an
execution oracle (that's melonDS). Mirrors `psxrecomp`'s `PSXBIOS.gpr`
+ `ghidra_function_starts.json` flow.

## Scope

- **Now (Phase 0):** the two BIOSes — they are uncompressed and directly
  loadable.
  - `biosnds7.rom` → ARM7TDMI, language `ARM:LE:32:v4t`, base `0x00000000`
  - `biosnds9.rom` → ARM946E-S, language `ARM:LE:32:v5t`, base `0xFFFF0000`
- **Later (Phase 3):** the firmware ARM9 menu + ARM7 parts, after the
  recompiled BIOS (or `fw_inspect`-side decoder) yields the
  **decompressed** binaries at their RAM destinations
  (`0x021F0000` ARM9, `0x0380CC00` ARM7 — see `../docs/firmware_boot.md`).
  Load each at its RAM base with the matching language.

## Run (local Ghidra)

```
bash ghidra/import_bios.sh /c/path/to/ghidra
```

This drives `analyzeHeadless` to create `ghidra/NDSBIOS.gpr`, import both
BIOSes with the right language + base, auto-analyze, and run
`export_functions.py` to write seed lists to
`generated/ghidra_function_starts_arm9.json` and `_arm7.json`.

## Run (Ghidra MCP — the primary path for this project)

This project drives Ghidra through an **SSE MCP server on port 2222**,
matching the sibling pattern (nesrecomp uses 3333, snesrecomp 8078):

- `../.mcp.json` → `mcpServers.ghidra = { type: "sse",
  url: "http://localhost:2222/sse" }`
- `../.claude/settings.json` → `enabledMcpjsonServers: ["ghidra"]`

The in-Ghidra extension serves SSE on 2222; it only exposes once at
least one program is open in the CodeBrowser. The MCP can address
multiple programs in the project, so once one is open both BIOSes are
drivable. Local-Ghidra install for reference:
`F:\Software\Ghidra\ghidra_12.0.3_PUBLIC_20260210\ghidra_12.0.3_PUBLIC`.

The `analyzeHeadless` script below remains the portable fallback.

## Resume plan (after the session restart that brings the MCP up)

1. Verify MCP connectivity: read program metadata / list functions.
2. Fix/reimport `biosnds7.rom` if the manual import used wrong settings
   (must be `ARM:LE:32:v4t`, base `0x00000000`).
3. Analyze ARM7 BIOS: label the vector table (reset 0x0, SWI 0x8, IRQ
   0x18), the firmware-header parse, the part decompressor/copy, SWI
   dispatch. Export → `generated/ghidra_function_starts_arm7.json`.
4. Import + analyze `biosnds9.rom` (`ARM:LE:32:v5t`, base `0xFFFF0000`).
   Export → `generated/ghidra_function_starts_arm9.json`.

## Output contract

`export_functions.py` emits, per program, a JSON array of
`{"addr": "0xXXXXXXXX", "name": "...", "mode": "arm|thumb"}`. The
recompiler consumes these as discovery seeds (`PRINCIPLES.md` "Hints are
not correctness" — seeds bootstrap discovery, they don't replace it).
