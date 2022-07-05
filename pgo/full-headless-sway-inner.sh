#!/bin/sh

set -ux

srcdir=$(realpath "${1}")
blddir=$(realpath "${2}")

"${srcdir}"/pgo/full-inner.sh "${srcdir}" "${blddir}"
swaymsg exit
