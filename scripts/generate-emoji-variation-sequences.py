#!/usr/bin/env python3

import argparse
import sys


class Codepoint:
    def __init__(self, start: int, end: None|int = None):
        self.start = start
        self.end = start if end is None else end
        self.vs15 = False
        self.vs16 = False

    def __repr__(self) -> str:
        return f'{self.start:x}-{self.end:x}, vs15={self.vs15}, vs16={self.vs16}'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('input', type=argparse.FileType('r'))
    parser.add_argument('output', type=argparse.FileType('w'))
    opts = parser.parse_args()

    codepoints: dict[int, Codepoint] = {}

    for line in opts.input:
        line = line.rstrip()
        if not line:
            continue
        if line[0] == '#':
            continue

        # Example: "0023 FE0E  ; text style;  # (1.1) NUMBER SIGN"
        cps, _ = line.split(';', maxsplit=1)  # cps = "0023 FE0F  "
        cps = cps.strip().split(' ')          # cps = ["0023", "FE0F"]

        if len(cps) != 2:
            raise NotImplementedError(f'emoji variation sequences with more than one base codepoint: {cps}')

        cp, vs = cps       # cp = "0023", vs = "FE0F"
        cp = int(cp, 16)   # cp = 0x23
        vs = int(vs, 16)   # vs = 0xfe0f

        assert vs in [0xfe0e, 0xfe0f]

        if cp not in codepoints:
            codepoints[cp] = Codepoint(cp)

        assert codepoints[cp].start == cp

        if vs == 0xfe0e:
            codepoints[cp].vs15 = True
        else:
            codepoints[cp].vs16 = True

    sorted_list = sorted(codepoints.values(), key=lambda cp: cp.start)

    compacted: list[Codepoint] = []
    for i, cp in enumerate(sorted_list):
        assert cp.end == cp.start

        if i == 0:
            compacted.append(cp)
            continue

        last_cp = compacted[-1]
        if last_cp.end == cp.start - 1 and last_cp.vs15 == cp.vs15 and last_cp.vs16 == cp.vs16:
            compacted[-1].end = cp.start
        else:
            compacted.append(cp)

    opts.output.write('#pragma once\n')
    opts.output.write('#include <stdint.h>\n')
    opts.output.write('#include <stdbool.h>\n')
    opts.output.write('\n')
    opts.output.write('struct emoji_vs {\n')
    opts.output.write('    uint32_t start:21;\n')
    opts.output.write('    uint32_t end:21;\n')
    opts.output.write('    bool vs15:1;\n')
    opts.output.write('    bool vs16:1;\n')
    opts.output.write('} __attribute__((packed));\n')
    opts.output.write('_Static_assert(sizeof(struct emoji_vs) == 6, "unexpected struct size");\n')
    opts.output.write('\n')
    opts.output.write('#if defined(FOOT_GRAPHEME_CLUSTERING)\n')
    opts.output.write('\n')

    opts.output.write(f'static const struct emoji_vs emoji_vs[{len(compacted)}] = {{\n')

    for cp in compacted:
        opts.output.write('    {\n')
        opts.output.write(f'        .start = 0x{cp.start:X},\n')
        opts.output.write(f'        .end = 0x{cp.end:x},\n')
        opts.output.write(f'        .vs15 = {"true" if cp.vs15 else "false"},\n')
        opts.output.write(f'        .vs16 = {"true" if cp.vs16 else "false"},\n')
        opts.output.write('    },\n')

    opts.output.write('};\n')
    opts.output.write('\n')
    opts.output.write('#endif  /* FOOT_GRAPHEME_CLUSTERING */\n')


if __name__ == '__main__':
    sys.exit(main())
