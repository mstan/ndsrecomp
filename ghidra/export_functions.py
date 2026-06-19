# export_functions.py — Ghidra headless postScript.
# Emits the analyzed program's functions as a JSON seed list for the
# ndsrecomp discovery pass. Runs under Ghidra's Jython (Python 2.7).
#
# Invoked by import_bios.sh as:
#   -postScript export_functions.py <output.json>
#
# Output: JSON array of {"addr","name","mode"} where mode is "thumb" if
# the function entry is in Thumb (TMode register = 1), else "arm".

import json

# getScriptArgs / currentProgram are injected by the Ghidra script env.
args = getScriptArgs()              # noqa: F821
out_path = args[0] if args else "ghidra_functions.json"

fm = currentProgram.getFunctionManager()   # noqa: F821
listing = currentProgram.getListing()       # noqa: F821
tmode = currentProgram.getProgramContext().getRegister("TMode")  # noqa: F821

records = []
for fn in fm.getFunctions(True):            # True = forward order
    entry = fn.getEntryPoint()
    mode = "arm"
    if tmode is not None:
        val = currentProgram.getProgramContext().getRegisterValue(  # noqa: F821
            tmode, entry)
        if val is not None and val.hasValue() and val.getUnsignedValueIgnoreMask():
            mode = "thumb"
    records.append({
        "addr": "0x%08X" % entry.getOffset(),
        "name": fn.getName(),
        "mode": mode,
    })

with open(out_path, "w") as f:
    json.dump(records, f, indent=1)

print("export_functions.py: wrote %d functions to %s" % (len(records), out_path))
