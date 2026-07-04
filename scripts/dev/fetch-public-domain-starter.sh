#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
manifest="$repo_root/starter-pack/public-domain/manifest.json"
target="${1:-${PAPERLIBRARY_STARTER_DIR:-$HOME/Projects/PaperLibrary/starter-public-domain}}"

python3 "$script_dir/fetch_public_domain_starter.py" "$manifest" "$target"
