import os
import subprocess
import urllib.request
import sys
import time
import signal
import json

REPO             = "michaelkrisper/TinyWebServer"
BOMBARDIER_VERSION = "v1.2.6"
BOMBARDIER_URLS  = {
    "nt":    f"https://github.com/codesenberg/bombardier/releases/download/{BOMBARDIER_VERSION}/bombardier-windows-amd64.exe",
    "posix": f"https://github.com/codesenberg/bombardier/releases/download/{BOMBARDIER_VERSION}/bombardier-linux-amd64",
}
SERVER_NAMES = {
    "nt":    "server-windows.exe",
    "posix": "server-linux",
}

TESTS_DIR      = os.path.dirname(os.path.abspath(__file__))
BOMBARDIER_EXE = os.path.join(TESTS_DIR, "bombardier.exe" if os.name == "nt" else "bombardier")
SERVER_EXE     = os.path.join(TESTS_DIR, SERVER_NAMES.get(os.name, "server"))
CONCURRENCY    = 100
DURATION       = "10s"


def download(url, dest, executable=False):
    print(f"Downloading {os.path.basename(dest)}...")
    urllib.request.urlretrieve(url, dest)
    if executable and os.name == "posix":
        os.chmod(dest, 0o755)
    print("Done.")


def ensure_bombardier():
    if not os.path.exists(BOMBARDIER_EXE):
        url = BOMBARDIER_URLS.get(os.name)
        if not url:
            print(f"Unsupported platform. Download bombardier from:")
            print(f"  https://github.com/codesenberg/bombardier/releases/tag/{BOMBARDIER_VERSION}")
            sys.exit(1)
        download(url, BOMBARDIER_EXE, executable=True)


def latest_release_tag():
    api = f"https://api.github.com/repos/{REPO}/releases/latest"
    with urllib.request.urlopen(api) as r:
        return json.loads(r.read())["tag_name"]


def ensure_server(tag=None):
    if os.path.exists(SERVER_EXE):
        return
    tag = tag or latest_release_tag()
    name = SERVER_NAMES.get(os.name)
    if not name:
        print("Unsupported platform — build the server manually.")
        sys.exit(1)
    url = f"https://github.com/{REPO}/releases/download/{tag}/{name}"
    download(url, SERVER_EXE, executable=True)


def start_server(port):
    serve_dir = os.path.dirname(TESTS_DIR)  # repo root contains index.html
    proc = subprocess.Popen(
        [SERVER_EXE, str(port), serve_dir],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(0.5)
    if proc.poll() is not None:
        print("Server failed to start.")
        sys.exit(1)
    return proc


def stop_server(proc):
    if os.name == "nt":
        proc.terminate()
    else:
        proc.send_signal(signal.SIGTERM)
    proc.wait()


def run_benchmark(url, label=None):
    print(f"\n--- {label or url} ---")
    subprocess.run([BOMBARDIER_EXE, "-c", str(CONCURRENCY), "-d", DURATION, "--latencies", url])


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    tag  = sys.argv[2] if len(sys.argv) > 2 else None

    ensure_bombardier()
    ensure_server(tag)

    server = start_server(port)
    base   = f"http://localhost:{port}"

    print(f"Bombardier {BOMBARDIER_VERSION}  |  {CONCURRENCY} connections  |  {DURATION}")
    print(f"Server binary: {SERVER_EXE}")

    endpoints = [
        ("/",           "root (index.html)"),
        ("/index.html", "index.html (explicit)"),
    ]

    try:
        for path, label in endpoints:
            run_benchmark(base + path, label)
    except KeyboardInterrupt:
        print("\nBenchmark aborted.")
    finally:
        stop_server(server)
