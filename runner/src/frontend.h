#pragma once

// Run the native, human-facing firmware preview. The emulation remains on the
// same scheduler/device path used by the deterministic debug verifier; SDL is
// only the host presentation and input/audio transport.
int nds_run_interactive_frontend();
