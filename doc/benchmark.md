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


## 2020-05-31

### System

CPU: i5-8250U 
RAM: 8GB RAM 
Graphics: Intel UHD Graphcis 620


### Terminal configuration

Geometry: 953x1023
Font: Dina:pixelsize=12
scrollback=10000


| Benchmark              | Foot (GCC+PGO) | Alacritty     | URxvt          | St            | XTerm         |
|------------------------|----------------|---------------|⨪---------------|---------------|---------------|
| alt-random             |  0.791s ±0.080 | 1.558s ±0.038 | 1.746s ±0.065  | 2.628s ±0.085 | 1.706s ±0.064 |
| alt-random-colors      |  0.830s ±0.076 | 1.587s ±0.041 | 2.049s ±0.118  | 3.033s ±0.129 | 2.109s ±0.131 |
| scrolling              |  1.603s ±0.070 | 1.464s ±0.098 | 1.439s ±0.035  | 3.760s ±0.113 | 1.459s ±0.036 |
| scrolling-filled-lines |  1.888s ±0.021 | 2.334s ±0.078 | 2.145s ±0.074  | 3.372s ±0.078 | 2.144s ±0.091 |
| unicode-random         |  1.545s ±0.229 | 0.164s ±0.012 | 11.180s ±0.342 |       crashed |11.389s ±0.269 |
