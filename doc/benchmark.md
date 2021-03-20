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


## 2021-03-20

### System

CPU: i5-8250U

RAM: 8GB

Graphics: Intel UHD Graphics 620


### Terminal configuration

Geometry: 945x1020

Font: Dina:pixelsize=12

Scrollback=10000 lines


### Results


| Benchmark              | Foot (GCC+PGO) 1.7.0.r2 | Foot (no PGO) 1.7.0.r2 |    Alacritty 0.7.2 |       URxvt 9.22 |      St 0.8.4 |       XTerm 366 |
|------------------------|------------------------:|-----------------------:|-------------------:|-----------------:|--------------:|----------------:|
| alt-random             |           0.714s ±0.047 |          0.900s ±0.041 | 1.586s ±0.045      |    1.684s ±0.034 | 2.054s ±0.121 |  37.205s ±0.252 |
| alt-random-colors      |           0.736s ±0.054 |          0.950s ±0.082 | 1.565s ±0.043      |    2.150s ±0.137 | 2.195s ±0.154 |  33.112s ±0.167 |
| scrolling              |           1.593s ±0.070 |          1.559s ±0.055 | 1.517s ±0.079      |    1.462s ±0.052 | 3.308s ±0.133 | 134.432s ±0.436 |
| scrolling-filled-lines |           1.178s ±0.044 |          1.309s ±0.045 | 2.281s ±0.086      |    2.044s ±0.060 | 2.732s ±0.056 |  20.753s ±0.067 |
| unicode-random         |           0.349s ±0.009 |          0.352s ±0.007 | 0.148s ±0.010 [^1] |   19.090s ±0.363 |       crashed |  15.579s ±0.093 |

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
