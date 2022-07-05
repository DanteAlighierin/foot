# Benchmarks

## vtebench

All benchmarks are done using [vtebench](https://github.com/alacritty/vtebench):

```sh
./target/release/vtebench -b ./benchmarks --dat /tmp/<terminal>
```

## 2022-05-12

### System

CPU: i9-9900

RAM: 64GB

Graphics: Radeon RX 5500XT


### Terminal configuration

Geometry: 2040x1884

Font: Fantasque Sans Mono 10.00pt/23px

Scrollback: 10000 lines


### Results

| Benchmark (times in ms)       | Foot (GCC+PGO) 1.12.1 | Foot 1.12.1 | Alacritty 0.10.1 | URxvt 9.26 | XTerm 372 |
|-------------------------------|----------------------:|------------:|-----------------:|-----------:|----------:|
| cursor motion                 |                 10.40 |       14.07 |            24.97 |      23.38 |   1622.86 |
| dense cells                   |                 29.58 |       45.46 |            97.45 |   10828.00 |   2323.00 |
| light cells                   |                  4.34 |        4.40 |            12.84 |      12.17 |     49.81 |
| scrollling                    |                135.31 |      116.35 |           121.69 |     108.30 |   4041.33 |
| scrolling bottom region       |                118.19 |      109.70 |           105.26 |     118.80 |   3875.00 |
| scrolling bottom small region |                132.41 |      122.11 |           122.83 |     151.30 |   3839.67 |
| scrolling fullscreen          |                  5.70 |        5.66 |            10.92 |      12.09 |    124.25 |
| scrolling top region          |                144.19 |      121.78 |           135.81 |     159.24 |   3858.33 |
| scrolling top small region    |                135.95 |      119.01 |           115.46 |     216.55 |   3872.67 |
| unicode                       |                 11.56 |       10.92 |            15.94 |    1012.27 |   4779.33 |


## 2022-05-12

### System

CPU: i5-8250U

RAM: 8GB

Graphics: Intel UHD Graphics 620


### Terminal configuration

Geometry: 945x1020

Font: Dina:pixelsize=12

Scrollback=10000 lines


### Results


| Benchmark (times in ms)       | Foot (GCC+PGO) 1.12.1 | Foot 1.12.1 | Alacritty 0.10.1 | URxvt 9.26 | XTerm 372 |
|-------------------------------|----------------------:|------------:|-----------------:|-----------:|----------:|
| cursor motion                 |                 15.03 |       16.74 |            23.22 |      24.14 |   1381.63 |
| dense cells                   |                 43.56 |       54.10 |            89.43 |    1807.17 |   1945.50 |
| light cells                   |                  7.96 |        9.66 |            20.19 |      21.31 |    122.44 |
| scrollling                    |                146.02 |      150.47 |           129.22 |     129.84 |  10140.00 |
| scrolling bottom region       |                138.36 |      137.42 |           117.06 |     141.87 |  10136.00 |
| scrolling bottom small region |                137.40 |      134.66 |           128.97 |     208.77 |   9930.00 |
| scrolling fullscreen          |                 11.66 |       12.02 |            19.69 |      21.96 |    315.80 |
| scrolling top region          |                143.81 |      133.47 |           132.51 |     475.81 |  10267.00 |
| scrolling top small region    |                133.72 |      135.32 |           145.10 |     314.13 |  10074.00 |
| unicode                       |                 20.89 |       21.78 |            26.11 |    5687.00 |  15740.00 |
