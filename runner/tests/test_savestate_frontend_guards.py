#!/usr/bin/env python3
"""Structural guards for frontend savestate input handoff.

The slot unit test covers command/result plumbing without SDL. This catches the
frontend-only regression where a successful load republished keys but left a
held touchscreen press as whatever the historical state file contained until
the next mouse event.
"""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
FRONTEND = ROOT / "runner/src/frontend.cpp"
source = FRONTEND.read_text(encoding="utf-8", errors="replace")
failures = []


def check(name, condition, detail=""):
    if condition:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name}: {detail}")
        failures.append(name)


load_branch = re.search(
    r"if\s*\(\s*result\.success\s*&&\s*state_command\.action\s*==\s*"
    r"NdsSavestateSlotAction::Load\s*\)\s*\{(?P<body>.*?)"
    r"\n\s*\}\s*\n\s*\}\s*else\s+if\s*\(scancode == SDL_SCANCODE_ESCAPE\)",
    source,
    re.S,
)

check("successful load branch exists", load_branch is not None)
body = load_branch.group("body") if load_branch else ""
check("successful load invalidates blend cache", "blend_cache.valid = false;" in body)
check("successful load republishes host keys", "publish_keys();" in body)
check(
    "held host touch is reapplied",
    "if (mouse_down)" in body
    and "set_touch_from_mouse(" in body
    and "last_touch_event_x" in body
    and "last_touch_event_y" in body,
)
check(
    "inactive host touch is cleared",
    "else" in body and "nds_set_touch(0, 0, false);" in body,
)
check(
    "load branch does not only clear non-held touch",
    "if (!mouse_down) nds_set_touch(0, 0, false);" not in body,
)

sys.exit(1 if failures else 0)
