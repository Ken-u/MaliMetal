#!/bin/bash
# Sourced by the scripts of this repository.
# Machine-specific paths and secrets live in ./.env (git-ignored, see .env.example); nothing here is personal.
_here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export MALIMETAL_ROOT="$_here"
if [ -f "$_here/.env" ]; then set -a; . "$_here/.env"; set +a; fi
# The Mali kernel-driver checkout (mali_uapi.h, gpu_files.inc); defaults to the sibling directory.
export MALI_DIR="${MALI_DIR:-$_here/../Mali}"
