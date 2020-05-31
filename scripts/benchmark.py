#!/usr/bin/env -S python3 -u

import argparse
import fcntl
import os
import statistics
import struct
import sys
import termios

from datetime import datetime


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('files', type=argparse.FileType('rb'), nargs='+')
    parser.add_argument('--iterations', type=int, default=20)

    args = parser.parse_args()

    lines, cols, height, width = struct.unpack(
        'HHHH',
        fcntl.ioctl(sys.stdout.fileno(),
                    termios.TIOCGWINSZ,
                    struct.pack('HHHH', 0, 0, 0, 0)))

    times = {name: [] for name in [f.name for f in args.files]}

    for f in args.files:
        bench_bytes = f.read()

        for i in range(args.iterations):
            start = datetime.now()
            sys.stdout.buffer.write(bench_bytes)
            stop = datetime.now()

            times[f.name].append((stop - start).total_seconds())

        del bench_bytes

    print('\033[J')
    print(times)
    print(f'cols={cols}, lines={lines}, width={width}px, height={height}px')
    for f in args.files:
        print(f'{os.path.basename(f.name)}: '
              f'{statistics.mean(times[f.name]):.3f}s '
              f'±{statistics.stdev(times[f.name]):.3f}')


if __name__ == '__main__':
    sys.exit(main())
