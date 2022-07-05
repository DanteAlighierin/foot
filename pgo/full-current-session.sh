#!/bin/sh

set -eux

srcdir=$(realpath "${1}")
blddir=$(realpath "${2}")

"${srcdir}"/pgo/full-inner.sh "${srcdir}" "${blddir}"
