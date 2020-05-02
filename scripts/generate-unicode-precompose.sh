#!/usr/bin/sh

cut - -d ";" -f 1,6 |
    grep ";[0-9,A-F]" | grep " " |
    sed -e "s/ /, 0x/;s/^/{ 0x/;s/;/, 0x/;s/$/},/" |
    sed -e "s,0x\(....\)\([^0-9A-Fa-f]\),0x0\1\2,g" |
    (sort -k 3 || sort +2) |
    sed -e "s,0x0\(...[0-9A-Fa-f]\),0x\1,g"
