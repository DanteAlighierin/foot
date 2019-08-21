#!/usr/bin/env python3
import random
import shutil
import sys


def main():
    term_size = shutil.get_terminal_size()
    lines = term_size.lines
    cols = term_size.columns

    # Number of characters to write to screen
    count = 1 * 1024**2

    # Characters to choose from
    alphabet = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRTSTUVWXYZ0123456789 '

    # Enter alt screen
    sys.stdout.write('\033[?1049h')

    for _ in range(count):
        # Generate a random location and a random character
        pos = (random.randint(0, cols), random.randint(0, lines))
        c = random.choice(alphabet)

        # Write character
        sys.stdout.write(f'\033[{pos[1] + 1};{pos[0] + 1}H')
        sys.stdout.write(c)

    # Leave alt screen
    sys.stdout.write('\033[?1049l')


if __name__ == '__main__':
    sys.exit(main())
