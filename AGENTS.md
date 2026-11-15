# Agent notes

- Prefer **/ponytail** (minimal diff, reuse existing patterns).
- After kernel changes, run **`make C=2`** — module build with kernel **sparse** (`C=2`).
- After `ddc.c` / DDC logic changes, also run **`make check`**.