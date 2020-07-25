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



## 2020-05-31

### System

CPU: i5-8250U

RAM: 8GB

Graphics: Intel UHD Graphcis 620


### Terminal configuration

Geometry: 953x1023

Font: Dina:pixelsize=12

Scrollback=10000 lines


### Results


| Benchmark              | Foot (GCC+PGO) 1.3.0.r59 | Alacritty 0.4.2      |     URxvt 9.22 |      St 0.8.3 |      XTerm 356 |
|------------------------|-------------------------:|---------------------:|---------------:|--------------:|---------------:|
| alt-random             |            0.791s ±0.080 |   1.558s ±0.038      |  1.746s ±0.065 | 2.628s ±0.085 |  1.706s ±0.064 |
| alt-random-colors      |            0.830s ±0.076 |   1.587s ±0.041      |  2.049s ±0.118 | 3.033s ±0.129 |  2.109s ±0.131 |
| scrolling              |            1.603s ±0.070 |   1.464s ±0.098      |  1.439s ±0.035 | 3.760s ±0.113 |  1.459s ±0.036 |
| scrolling-filled-lines |            1.888s ±0.021 |   2.334s ±0.078      |  2.145s ±0.074 | 3.372s ±0.078 |  2.144s ±0.091 |
| unicode-random         |            1.545s ±0.229 |   0.164s ±0.012 [^1] | 11.180s ±0.342 |       crashed | 11.389s ±0.269 |

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
