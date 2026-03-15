# TinyWebServer

![Build and Release](https://github.com/michaelkrisper/TinyWebServer/actions/workflows/release.yml/badge.svg)
![Build Verification](https://github.com/michaelkrisper/TinyWebServer/actions/workflows/test.yml/badge.svg)

A minimalist static file server written in C.

## Features
- **Any file type**: Serves HTML, CSS, JS, images, fonts, video, audio, ZIP, and more (20+ MIME types)
- **HTTP keep-alive**: connections are reused across requests — no TCP handshake per request
- **mtime cache**: files cached in memory, reloaded only on change (up to 64 entries, 1s re-check interval)
- **Zero-copy cache hits**: serves directly from cached buffer under read lock — no malloc/memcpy per request
- **Thread pool**: 32 pre-created workers, ring-buffer queue (256 slots) — no per-request thread creation
- **CLI arguments**: configurable port and serve directory
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

Load test using [Bombardier](https://github.com/codesenberg/bombardier) v1.2.6 — 100 concurrent connections, 10 seconds, Windows (v3.3 release binary):

| Endpoint      | Req/sec  | Latency avg | p50      | p99     | Throughput  |
|---------------|----------|-------------|----------|---------|-------------|
| `/`           | ~40,700  | 2.5 ms      | 0.57 ms  | 5.9 ms  | 118 MB/s    |
| `/index.html` | ~42,700  | 2.4 ms      | 0.55 ms  | 6.2 ms  | 125 MB/s    |

**120× improvement over v3.2** (338 → 40,700 RPS). The bottleneck was TCP connection setup/teardown per request — keep-alive eliminated it. Zero-copy cache serving and skipping `stat()` on fresh entries removed the remaining per-request overhead.

### Running the benchmark yourself
```bash
python tests/bench_bombardier.py [port] [tag]
```
Downloads the server binary from the GitHub release and bombardier automatically, starts the server, and runs the load test.

## License
MIT License. Attribution required (see `LICENSE`).
