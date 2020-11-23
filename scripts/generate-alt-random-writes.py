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
    CUBE = enum.auto()
    RGB = enum.auto()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        'out', type=argparse.FileType(mode='w'), nargs='?', help='name of output file')
    parser.add_argument('--cols', type=int)
    parser.add_argument('--rows', type=int)
    parser.add_argument('--colors-regular', action='store_true')
    parser.add_argument('--colors-bright', action='store_true')
    parser.add_argument('--colors-256', action='store_true')
    parser.add_argument('--colors-rgb', action='store_true')
    parser.add_argument('--scroll', action='store_true')
    parser.add_argument('--scroll-region', action='store_true')
    parser.add_argument('--attr-bold', action='store_true')
    parser.add_argument('--attr-italic', action='store_true')
    parser.add_argument('--attr-underline', action='store_true')
    parser.add_argument('--sixel', action='store_true')

    opts = parser.parse_args()
    out = opts.out if opts.out is not None else sys.stdout

    lines, cols, height, width = struct.unpack(
        'HHHH',
        fcntl.ioctl(sys.stdout.fileno(),
                    termios.TIOCGWINSZ,
                    struct.pack('HHHH', 0, 0, 0, 0)))

    if opts.rows is not None:
        lines = opts.rows
    if opts.cols is not None:
        cols = opts.cols

    # Number of characters to write to screen
    count = 256 * 1024**1

    # Characters to choose from
    alphabet = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRTSTUVWXYZ0123456789 öäå 👨👩🧒'

    color_variants = ([ColorVariant.NONE] +
                      ([ColorVariant.REGULAR] if opts.colors_regular else []) +
                      ([ColorVariant.BRIGHT] if opts.colors_bright else []) +
                      ([ColorVariant.CUBE] if opts.colors_256 else []) +
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
                if not rev and rand.read(1)[0] % 2:
                    out.write(f'\033[{lines};{cols}H')
                    out.write('\n' * lines_to_scroll)
                else:
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

            elif color_variant == ColorVariant.CUBE:
                do_bg = rand.read(1)[0] % 2
                base = 48 if do_bg else 38

                idx = rand.read(1)[0] % 256
                if rand.read(1)[0] % 2:
                    # Old-style
                    out.write(f'\033[{base};5;{idx}m')
                else:
                    # New-style (sub-parameter based)
                    out.write(f'\033[{base}:2:5:{idx}m')

            elif color_variant == ColorVariant.RGB:
                do_bg = rand.read(1)[0] % 2
                base = 48 if do_bg else 38
                rgb = rand.read(3)

                if rand.read(1)[0] % 2:
                    # Old-style
                    out.write(f'\033[{base};2;{rgb[0]};{rgb[1]};{rgb[2]}m')
                else:
                    # New-style (sub-parameter based)
                    out.write(f'\033[{base}:2::{rgb[0]}:{rgb[1]}:{rgb[2]}m')

            if opts.attr_bold and rand.read(1)[0] % 5 == 0:
                out.write('\033[1m')
            if opts.attr_italic and rand.read(1)[0] % 5 == 0:
                out.write('\033[3m')
            if opts.attr_underline and rand.read(1)[0] % 5 == 0:
                out.write('\033[4m')

            out.write(c * repeat)

            do_sgr_reset = rand.read(1)[0] % 2
            if do_sgr_reset:
                reset_actions = ['\033[m', '\033[39m', '\033[49m']
                idx = rand.read(1)[0] % len(reset_actions)
                out.write(reset_actions[idx])

    # Leave alt screen
    out.write('\033[m\033[r\033[?1049l')

    with open('/dev/urandom', 'rb') as rand:
        if opts.sixel:
            # The sixel 'alphabet'
            sixels = '?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~'

            for _ in range(200):
                # Offset image
                out.write(' ' * (rand.read(1)[0] % (cols // 2)))

                # Begin sixel
                out.write('\033Pq')

                # Set up 256 random colors
                for idx in range(256):
                    # param 2: 1=HLS, 2=RGB.
                    # param 3/4/5: HLS/RGB values in range 0-100
                    #              (except 'hue' which is 0..360)
                    out.write(f'#{idx};2;{rand.read(1)[0] % 101};{rand.read(1)[0] % 101};{rand.read(1)[0] % 101}')

                # Randomize image width/height
                six_height = struct.unpack('@H', rand.read(2))[0] % (height // 2)
                six_width = struct.unpack('@H', rand.read(2))[0] % (width // 2)

                # Sixel size. Without this, sixels will be
                # auto-resized on cell-boundaries. We expect programs
                # to emit this sequence since otherwise you cannot get
                # correctly sized images.
                out.write(f'"0;0;{six_width};{six_height}')

                for row in range(six_height // 6):  # Each sixel is 6 pixels
                    # Choose a random color
                    out.write(f'#{rand.read(1)[0] % 256}')

                    if rand.read(1)[0] == 999999999999:
                        assert False
                        out.write(f'!{six_width}{sixels[rand.read(1)[0] % len(sixels)]}')
                    else:
                        for col in range(six_width):
                            out.write(f'{sixels[rand.read(1)[0] % len(sixels)]}')

                    # Next line
                    out.write('-')

                # End sixel
                out.write('\033\\')


if __name__ == '__main__':
    sys.exit(main())
