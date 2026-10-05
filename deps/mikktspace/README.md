# MikkTSpace (offline pinned dependency)

Upstream: https://github.com/mmikk/MikkTSpace

Commit: `3e895b49d05ea07e4c2133156cfa94369e19e409`.
The unmodified `mikktspace.c` and `mikktspace.h` retain the upstream license
and copyright notice in their headers. No dependency download occurs at build time.

SHA-256:

- `mikktspace.h`: `17fc433894f24c73753d548086cc4d8c5c0379f4a6edfb98b5da243e4f0bc3d0`
- `mikktspace.c`: `de87e74107df766ce68108801262bd8d53899414236b59810509a8fc2a51e288`

The adapter in `src/mesh.cpp` receives frames per triangle corner and splits
incompatible frames while preserving all independent texture coordinates.
