# MVision

Experimental native runtime for a classic PlayStation title on Apple Vision Pro (immersive, 6DOF).

This repository contains **no game data and no copyrighted code**. You must supply your own legally obtained disc image; everything game-specific is read from it at runtime or at build time on your machine/CI.

## Layout

- `tools/disc/` — BIN/CUE (MODE2/2352) reader: ISO9660 listing, extraction, manifests for diffs.
- `.github/workflows/` — CI (manual dispatch or `[build]` in the commit message only).

## Credits

Written by Claude, building on the great work of the projects credited here; the repository owner contributed ideas on how some things could be adapted.
