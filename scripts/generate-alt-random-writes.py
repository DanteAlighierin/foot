#!/usr/bin/env python3
import argparse
import enum
import fcntl
import struct
import sys
import termios


class ColorVariant(enum.IntEnum):
    NONE = enum.auto()
    REGULAR = enum.auto()
    BRIGHT = enum.auto()
    RGB = enum.auto()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        'out', type=argparse.FileType(mode='w'), nargs='?', help='name of output file')
    parser.add_argument('--colors-regular', action='store_true')
    parser.add_argument('--colors-bright', action='store_true')
    parser.add_argument('--colors-rgb', action='store_true')
    parser.add_argument('--scroll', action='store_true')
    parser.add_argument('--scroll-region', action='store_true')

    opts = parser.parse_args()
    out = opts.out if opts.out is not None else sys.stdout

    lines, cols, _, _ = struct.unpack(
        'HHHH',
        fcntl.ioctl(sys.stdout.fileno(),
                    termios.TIOCGWINSZ,
                    struct.pack('HHHH', 0, 0, 0, 0)))

    # Number of characters to write to screen
    count = 256 * 1024**1

    # Characters to choose from
    alphabet = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRTSTUVWXYZ0123456789 '

    color_variants = ([ColorVariant.NONE] +
                      ([ColorVariant.REGULAR] if opts.colors_regular else []) +
                      ([ColorVariant.BRIGHT] if opts.colors_bright else []) +
                      ([ColorVariant.RGB] if opts.colors_rgb else []))

    # Enter alt screen
    out.write('\033[?1049h')

    with open('/dev/urandom', 'rb') as rand:
        for _ in range(count):
            if opts.scroll and rand.read(1)[0] == 0:
                out.write('\033[m')

                if opts.scroll_region and rand.read(1)[0] == 0:
                    top = rand.read(1)[0] % 3
                    bottom = rand.read(1)[0] % 3
                    out.write(f'\033[{top};{lines - bottom}r')

                lines_to_scroll = rand.read(1)[0] % (lines - 1)
                rev = rand.read(1)[0] % 2
                out.write(f'\033[{lines_to_scroll + 1}{"T" if rev == 1 else "S"}')
                continue

            # Generate a random location and a random character
            row = rand.read(1)[0] % lines
            col = rand.read(1)[0] % cols
            c = alphabet[rand.read(1)[0] % len(alphabet)]

            repeat = rand.read(1)[0] % (cols - col) + 1
            assert col + repeat <= cols

            color_variant = color_variants[rand.read(1)[0] % len(color_variants)]

            # Position cursor
            out.write(f'\033[{row + 1};{col + 1}H')

            if color_variant in [ColorVariant.REGULAR, ColorVariant.BRIGHT]:
                do_bg = rand.read(1)[0] % 2
                base = 40 if do_bg else 30
                base += 60 if color_variant == ColorVariant.BRIGHT else 0

                idx = rand.read(1)[0] % 8
                out.write(f'\033[{base + idx}m')

            elif color_variant == ColorVariant.RGB:
                do_bg = rand.read(1)[0] % 2
                rgb = rand.read(3)
                out.write(f'\033[{48 if do_bg else 38}:2::{rgb[0]}:{rgb[1]}:{rgb[2]}m')

            out.write(c * repeat)

            if color_variant != ColorVariant.NONE:
                do_sgr_reset = rand.read(1)[0] % 2
                if do_sgr_reset:
                    out.write('\033[m')

    # Leave alt screen
    out.write('\033[m\033[r\033[?1049l')


if __name__ == '__main__':
    sys.exit(main())
