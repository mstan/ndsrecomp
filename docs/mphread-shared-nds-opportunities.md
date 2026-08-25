# MphRead Shared NDS Opportunities

Reference: https://github.com/NoneGiven/MphRead

MphRead is game-specific, but some of its extraction and validation ideas should
inform shared ndsrecomp tooling. The goal is to avoid putting generic Nintendo
DS infrastructure inside MetroidPrimeHuntersRecomp just because MPH is the
first released title.

## Shared Candidates

- NDS ROM filesystem extraction helpers for FNT/FAT walking and stable
  extracted-root layouts.
- Compression/archive plumbing that can be parameterized by title, including
  LZ10 handling and simple archive validators.
- File-selection and extracted-root configuration patterns that work across
  Windows, Linux, and AppImage builds.
- Validation tools that compare native framebuffer output, direct OpenGL
  presentation, and software fallback output.
- Renderer diagnostics that identify whether a visual failure happens before
  or after final presentation.

## Title-Owned Candidates

- MPH `SNDFILE` archive naming conventions and AMHE0 archive inventory.
- MPH room metadata, model/material meaning, entity structs, collision
  semantics, node data, save data, and gameplay behavior.
- MPH adaptive widescreen, portal/frustum, room culling, HUD anchoring, and
  scene-specific fallback decisions.
- Any comparison against MphRead's reconstructed gameplay or renderer output.

## Licensing

MphRead is MIT licensed. Shared ndsrecomp code can adapt MIT-licensed portions,
but copied/adapted code must carry the upstream copyright and permission notice
in `THIRD_PARTY_ATTRIBUTION.md` or an equivalent local notice. Reference-only
use should be documented in `docs/references.md` or title docs rather than in
vendored-source attribution.

Beads tracker: `beads-yjp.38`.
