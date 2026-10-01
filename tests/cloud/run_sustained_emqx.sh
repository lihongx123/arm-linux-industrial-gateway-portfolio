#!/usr/bin/env bash
set -euo pipefail

private_env="${MQMGATEWAY_EMQX_ENV:-${XDG_CONFIG_HOME:-$HOME/.config}/mqmgateway/emqx.env}"
[[ -f "$private_env" ]] || { echo "private EMQX environment file is missing" >&2; exit 1; }
[[ $(stat -c %a "$private_env") == 600 ]] || { echo "private EMQX environment file must have 600 permissions" >&2; exit 1; }
set -a
source "$private_env"
set +a
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$script_dir/sustained_emqx.py" "$@"
