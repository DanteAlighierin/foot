#!/bin/sh

set -eux

srcdir=$(realpath "${1}")
blddir=$(realpath "${2}")

runtime_dir=$(mktemp -d)
trap "rm -rf '${runtime_dir}'" EXIT INT HUP TERM

XDG_RUNTIME_DIR="${runtime_dir}" WLR_RENDERER=pixman WLR_BACKENDS=headless cage "${srcdir}"/pgo/full-inner.sh "${srcdir}" "${blddir}"

# Cage’s exit code doesn’t reflect our script’s exit code
[ -f "${blddir}"/pgo-ok ] || exit 1
