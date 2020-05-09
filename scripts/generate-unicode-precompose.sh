#!/bin/sh

unicodedata_txt="${1}"
output="${2}"

cat <<EOF > "${output}"
#pragma once

#include <wchar.h>

static const struct {
    wchar_t replacement;
    wchar_t base;
    wchar_t comb;
} precompose_table[] = {
EOF

# extract canonical decomposition data from UnicodeData.txt,
# - pad hex values to 5 digits,
# - sort numerically on base character, then combining character,
# - then reduce to 4 digits again where possible
#
# "borrowed" from xterm/unicode/make-precompose.sh

cut -d ";" -f 1,6 "${unicodedata_txt}" |
    grep ";[0-9,A-F]" | grep " " |
    sed -e "s/ /, 0x/;s/^/{ 0x/;s/;/, 0x/;s/$/},/" |
    sed -e "s,0x\(....\)\([^0-9A-Fa-f]\),0x0\1\2,g" |
    (sort -k 3 || sort +2) |
    sed -e "s,0x0\(...[0-9A-Fa-f]\),0x\1,g" |
    sed 's/^/    /' >> "${output}"

echo "};" >> "${output}"
