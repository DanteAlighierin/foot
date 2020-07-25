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


## 2020-07-25

### System

CPU: i9-9900

RAM: 64GB

Graphics: Radeon RX 5500XT


### Terminal configuration

Geometry: 2040x1884

Font: Fantasque Sans Mono 10.00pt/23px

Scrollback: 10000 lines


### Results

| Benchmark              | Foot (GCC+PGO) 1.4.2.r14 |      Alacritty 0.4.3 |     URxvt 9.22 |      XTerm 358 |
|------------------------|-------------------------:|---------------------:|---------------:|---------------:|
| alt-random             |            0.423s ±0.014 |   0.904s ±0.006      |  1.111s ±0.003 | 12.851s ±0.087 |
| alt-random-colors      |            0.382s ±0.005 |   0.935s ±0.005      |  1.146s ±0.007 | 11.816s ±0.088 |
| scrolling              |            1.380s ±0.048 |   1.011s ±0.012      |  1.021s ±0.016 | 38.483s ±0.122 |
| scrolling-filled-lines |            0.826s ±0.020 |   1.307s ±0.008      |  1.213s ±0.015 |  6.725s ±0.016 |
| unicode-random         |            0.243s ±0.006 |   0.091s ±0.003 [^1] | 24.507s ±3.264 | 26.127s ±3.891 |



## 2020-07-25

### System

CPU: i5-8250U

RAM: 8GB

Graphics: Intel UHD Graphcis 620


### Terminal configuration

Geometry: 953x1023

Font: Dina:pixelsize=12

Scrollback=10000 lines


### Results


| Benchmark              | Foot (GCC+PGO) 1.4.2.r9 | Alacritty 0.4.3      |     URxvt 9.22 |      St 0.8.4 |       XTerm 358 |
|------------------------|------------------------:|---------------------:|---------------:|--------------:|----------------:|
| alt-random             |           0.784s ±0.074 |   1.568s ±0.094      |  1.600s ±0.052 | 1.917s ±0.054 |  34.487s ±0.118 |
| alt-random-colors      |           0.823s ±0.067 |   1.627s ±0.107      |  1.932s ±0.073 | 2.111s ±0.163 |  30.676s ±0.127 |
| scrolling              |           1.612s ±0.092 |   1.492s ±0.051      |  1.504s ±0.033 | 3.767s ±0.140 | 125.202s ±0.383 |
| scrolling-filled-lines |           1.874s ±0.039 |   2.423s ±0.083      |  1.994s ±0.037 | 2.751s ±0.076 |  19.608s ±0.056 |
| unicode-random         |           0.458s ±0.026 |   0.159s ±0.007 [^1] | 12.416s ±0.223 |       crashed |  16.336s ±0.410 |

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
