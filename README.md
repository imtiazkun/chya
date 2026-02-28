# chya

Stop motion tool: **merge image sequences into video** and **turn video into stop motion** by reducing frames (e.g. keep every Nth frame or lower FPS). Built with CMake and Ninja.

## Requirements

- CMake 3.20+
- Ninja
- C++20-capable compiler (GCC, Clang, MSVC)

## Build

```bash
mkdir -p build && cd build
cmake -G Ninja ..
ninja
```

Binary: `build/chya`

## Run

```bash
./build/chya
```

## License

See [LICENSE](LICENSE).
