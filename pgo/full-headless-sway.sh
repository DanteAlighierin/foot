#!/bin/sh

set -eux

srcdir=$(realpath "${1}")
blddir=$(realpath "${2}")

runtime_dir=$(mktemp -d)
sway_conf=$(mktemp)

cleanup() {
    rm -f "${sway_conf}"
    rm -rf "${runtime_dir}"
}
trap cleanup EXIT INT HUP TERM

# Generate a custom config that executes our generate-pgo-data script
> "${sway_conf}" echo "exec '${srcdir}'/pgo/full-headless-sway-inner.sh '${srcdir}' '${blddir}'"

# Run Sway. full-headless-sway-inner.sh ends with a ‘swaymsg exit’
XDG_RUNTIME_DIR="${runtime_dir}" WLR_RENDERER=pixman WLR_BACKENDS=headless sway -c "${sway_conf}"

# Sway’s exit code doesn’t reflect our script’s exit code
[ -f "${blddir}"/pgo-ok ] || exit 1
