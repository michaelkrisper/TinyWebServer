# TinyWebServer

![Build and Release](https://github.com/michaelkrisper/TinyWebServer/actions/workflows/release.yml/badge.svg)
![Build Verification](https://github.com/michaelkrisper/TinyWebServer/actions/workflows/test.yml/badge.svg)

A minimalist static file server written in C. Files are read from disk on every request — no caching, no global state.

## Features
- **Any file type**: Serves HTML, CSS, JS, images, fonts, video, audio, ZIP, and more (20+ MIME types)
- **CLI arguments**: Configurable port and serve directory
- **Thread-per-connection**: Each request is handled in a short-lived detached thread
- **Directory traversal protection**: `..` in paths returns 403
- **Small footprint**: ~12 KB binary (Windows)
- **Cross-platform**: Windows (MSVC/GCC/Clang) and Linux/macOS (GCC)

## Usage

```
server [port] [directory]
```

| Argument    | Default                     |
|-------------|-----------------------------|
| `port`      | `80`                        |
| `directory` | Directory of the executable |

### Windows
```bat
build.bat
.\server.exe 8080 C:\www
```

### Linux / macOS
```bash
make
./server 8080 /var/www
```

## Benchmarks

Load test using [Bombardier](https://github.com/codesenberg/bombardier) v1.2.6 — 100 concurrent connections, 10 seconds, Windows (v3.0 release binary, files read from disk):

| Endpoint      | Req/sec | Latency avg | p50     | p99     |
|---------------|---------|-------------|---------|---------|
| `/`           | ~327    | 320 ms      | 318 ms  | 384 ms  |
| `/index.html` | ~316    | 320 ms      | 318 ms  | 354 ms  |

*Performance is higher on Linux due to lower thread-creation overhead. The old cached version reached ~3,380 RPS; the tradeoff is simplicity and support for any file type.*

### Running the benchmark yourself
```bash
python tests/bench_bombardier.py [port] [tag]
```
Downloads the server binary from the GitHub release and bombardier automatically, starts the server, and runs the load test.

## License
MIT License. Attribution required (see `LICENSE`).
