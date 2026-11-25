# Brief

This a pure C++ 20 implementation for serval concurrent containers: forward list, queue and stack with different approaches, for personal study

## Benchmakr

### Producer-Consumer

<p align="center">
  <img src="benchmark/bench-logs/main-261125/producer_consumer_clang-main-261125.png" alt="Producer-Consumer Clang" width="400">
  <img src="benchmark/bench-logs/main-261125/producer_consumer_gcc-main-261125.png" alt="Producer-Consumer GCC" width="400">
  
</p>

<p align="center">
  <img src="benchmark/bench-logs/main-261125/05_slot_sensitivity-main-261125.png" alt="Producer-Consumer MSVC" width="600">
</p>

### ElimiBackoffStack

<p align="center">
  <img src="benchmark/bench-logs/main-261125/05_slot_sensitivity-main-261125.png" alt="Slot Sensitivity" width="600">
</p>

### PGO

<p align="center">
  <img src="benchmark/bench-logs/main-261125/01_scalability_pgo-main-261125.png" alt="Scalablity PGO" width="600">
</p>

<p align="center">
  <img src="benchmark/bench-logs/main-261125/02_pgo_lift_heatmap-main-261125.png" alt="PGO Lift Heatmap" width="600">
</p>

# TODO

## Core

- [x] Concurrent stack
- [ ] Concurrent forward list
- [ ] Concurrent queue
- [ ] Add module functionality for GNU GCC and Clang complier
- [x] Compatibility with different compliers:
  - [x] UCRT Clang
  - [x] MSVC
  - [x] UCRT GCC
- [x] Compatibility with different build systems:CMake, Meson, Bazel
- [x] Compatibility with IDEs:
  - [ ] Visual Studio(temporarily MSVC x86/x64-debug/release configures only)
  - [x] Clion
- [x] Smoke tests(google/googletest)
- [x] Benchmark(google/benchmark)
- [x] Increment Build Cache:
  - [x] ccache
  - [x] sccache
- [x] PGO support for(CMake only)
  - [x] Clang
  - [x] GCC
  - [x] MSVC
- [ ] no-ninja generator support?(Maybe for Visual Studio Generator)

## Other

- [x] CI/CD
- [ ] More descriptions
  
## Temporay
- [ ] bazel and meson need updated to match CMake

