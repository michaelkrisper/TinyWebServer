import os
import subprocess
import urllib.request
import sys

BOMBARDIER_VERSION = "v1.2.6"
BOMBARDIER_URLS = {
    "nt":      f"https://github.com/codesenberg/bombardier/releases/download/{BOMBARDIER_VERSION}/bombardier-windows-amd64.exe",
    "posix":   f"https://github.com/codesenberg/bombardier/releases/download/{BOMBARDIER_VERSION}/bombardier-linux-amd64",
}
BOMBARDIER_EXE = os.path.join(
    os.path.dirname(__file__),
    "bombardier.exe" if os.name == "nt" else "bombardier"
)
CONCURRENCY = 100
DURATION = "10s"

def ensure_bombardier():
    if os.path.exists(BOMBARDIER_EXE):
        return
    url = BOMBARDIER_URLS.get(os.name)
    if not url:
        print(f"Unsupported platform: {os.name}. Download bombardier manually from:")
        print(f"  https://github.com/codesenberg/bombardier/releases/tag/{BOMBARDIER_VERSION}")
        sys.exit(1)
    print(f"Downloading bombardier {BOMBARDIER_VERSION}...")
    urllib.request.urlretrieve(url, BOMBARDIER_EXE)
    if os.name == "posix":
        os.chmod(BOMBARDIER_EXE, 0o755)
    print("Done.")

def run_benchmark(url, label=None):
    label = label or url
    print(f"\n--- {label} ---")
    subprocess.run([BOMBARDIER_EXE, "-c", str(CONCURRENCY), "-d", DURATION, "--latency", url])

if __name__ == "__main__":
    base_url = os.environ.get("SERVER_URL", sys.argv[1] if len(sys.argv) > 1 else "http://localhost:80")
    base_url = base_url.rstrip("/")

    ensure_bombardier()
    print(f"Bombardier {BOMBARDIER_VERSION}  |  {CONCURRENCY} connections  |  {DURATION}")

    endpoints = [
        ("/",           "root (index.html)"),
        ("/index.html", "index.html"),
    ]
    # Add extra endpoints from remaining CLI args: e.g. /style.css "/logo.png"
    for extra in sys.argv[2:]:
        endpoints.append((extra, extra))

    try:
        for path, label in endpoints:
            run_benchmark(base_url + path, label)
    except KeyboardInterrupt:
        print("\nBenchmark aborted.")
