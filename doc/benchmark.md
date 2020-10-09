# Benchmarks

## vtebench

All benchmarks are done using [vtebench](https://github.com/alacritty/vtebench):

```sh
vtebench -h $(tput lines) -w $(tput cols) -b 104857600 alt-screen-random-write > ~/alt-random
vtebench -c -h $(tput lines) -w $(tput cols) -b 104857600 alt-screen-random-write > ~/alt-random-colors
vtebench -h $(tput lines) -w $(tput cols) -b 10485760 scrolling > ~/scrolling
vtebench -h $(tput lines) -w $(tput cols) -b 104857600 scrolling --fill-lines > ~/scrolling-filled-lines
vtebench -h $(tput lines) -w $(tput cols) -b 10485760 unicode-random-write > ~/unicode-random
```

They were "executed" using [benchmark.py](../scripts/benchmark.py),
which will load each file into memory, and then print it to the
terminal. This is done **20** times for each test. Then it calculates
the _mean_ and _standard deviation_ for each test.


## 2020-10-09

### System

CPU: i9-9900

RAM: 64GB

Graphics: Radeon RX 5500XT


### Terminal configuration

Geometry: 2040x1884

Font: Fantasque Sans Mono 10.00pt/23px

Scrollback: 10000 lines


### Results


| Benchmark              | Foot (GCC+PGO) 1.5.0.r90 | Foot 1.5.0.r90 |    Alacritty 0.5.0 |     URxvt 9.22 |      XTerm 360 |
|------------------------|-------------------------:|---------------:|-------------------:|---------------:|---------------:|
| alt-random             |            0.353s ±0.007 |  0.685s ±0.005 | 0.903s ±0.011      |  1.102s ±0.004 | 12.886s ±0.064 |
| alt-random-colors      |            0.354s ±0.019 |  0.665s ±0.004 | 0.933s ±0.004      |  1.149s ±0.013 | 11.739s ±0.093 |
| scrolling              |            1.387s ±0.077 |  1.257s ±0.032 | 1.048s ±0.011      |  1.001s ±0.023 | 38.187s ±0.192 |
| scrolling-filled-lines |            0.607s ±0.008 |  0.834s ±0.038 | 1.246s ±0.020      |  1.224s ±0.008 |  6.619s ±0.166 |
| unicode-random         |            0.224s ±0.001 |  0.144s ±0.001 | 0.092s ±0.004 [^1] | 21.294s ±1.580 | 26.594s ±3.801 |


## 2020-07-25

### System

CPU: i5-8250U

RAM: 8GB

Graphics: Intel UHD Graphics 620


### Terminal configuration

Geometry: 945x1020

Font: Dina:pixelsize=12

Scrollback=10000 lines


### Results

| Benchmark              | Foot (GCC+PGO) 1.5.0.r27 |     Alacritty 0.5.0 |       URxvt 9.22 |      St 0.8.4 |       XTerm 360 |
|------------------------|-------------------------:|--------------------:|-----------------:|--------------:|----------------:|
| alt-random             |            0.741s ±0.067 |  1.472s ±0.126      |    1.717s ±0.141 | 1.910s ±0.030 |  37.832s ±0.193 |
| alt-random-colors      |            0.735s ±0.050 |  1.510s ±0.084      |    1.936s ±0.121 | 2.114s ±0.116 |  33.759s ±0.344 |
| scrolling              |            1.687s ±0.044 |  1.440s ±0.128      |    1.485s ±0.032 | 3.485s ±0.142 | 134.590s ±0.602 |
| scrolling-filled-lines |            1.331s ±0.041 |  2.072s ±0.073      |    2.031s ±0.087 | 2.658s ±0.084 |  20.508s ±0.063 |
| unicode-random         |            0.303s ±0.010 |  0.155s ±0.006 [^1] | 130.967s ±28.161 |       crashed | 170.444s ±7.798 |

[^1]: [Alacritty and "unicode-random"](#alacritty-and-unicode-random)


# Alacritty and "unicode-random"

Alacritty is actually **really** slow at rendering this (whether it is
fallback fonts in general, emojis, or something else, I don't know).

I believe the reason it finishes the benchmark so quickly is because
it reads from the PTY in a separate thread, into a larger receive
buffer which is then consumed by the main thread. This allows the
client program to write its output much faster since it is no longer
stalling on a blocked PTY.

This means Alacritty only needs to render a couple of frames since it
can reach the final VT state almost immediately.

On the other hand, `cat`:ing the `unicode-random` test file in an
endless loop, or just manually scrolling up after the benchmark is
done is **slow**, which besides being felt (input lag), can be seen by
setting `debug.render_timer = true` in `alacritty.yml`.
