# Agent instructions

## Responsibility

Maintain the user-space GPU stack: PanVK patches, the libdrm stand-in, the Metal plug-in and its
bridge. The kernel driver is [Mali](https://github.com/Ken-u/Mali); the board repository
(`UniXNU-evb10`) wires it in through boot arguments (`mali=8 mali-metal=1 ...`) and the display backend.

## Required reading

1. This README, `mesa-build/README.md` and `docs/notes.zh-CN.md`.
2. [GPU porting guide](https://unixnu.com/porting/gpu) and [Driver repositories](https://unixnu.com/drivers/repositories).

## Rules

- `darwin-drm` and Mali's `mali_uapi.h` are one contract. Change both together; the build reads `MALI_DIR` (default `../Mali`).
- Never put machine paths, hosts, accounts or passwords in a script or document. They live in `.env` (git-ignored, template `.env.example`).
- Never commit build output, SDKs, firmware, Apple content or logs.
- The Mesa tree is never modified in place: patches go to `mesa-patches/` and apply with `mesa-build/prepare-mesa-src.sh`.
- Inline assembly for Darwin uses `"\n\t"` between instructions, never `;`.
- Keep measured hardware results apart from host or build results when reporting.
