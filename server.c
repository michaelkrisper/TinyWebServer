#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DEFAULT_PORT  80
#define MAX_REQ       2048
#define MAX_FILE_SIZE (10 * 1024 * 1024)
#define CACHE_CAP            64
#define MTIME_CHECK_INTERVAL  1   /* seconds between mtime re-checks */
#define POOL_SIZE     128
#define QUEUE_CAP     512

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#define os_chdir    _chdir
#define os_stat_t   struct _stat
#define os_stat     _stat

void get_exe_dir(const char *argv0, char *dir, size_t size) {
  (void)argv0;
  GetModuleFileNameA(NULL, dir, (DWORD)size);
  char *sep = strrchr(dir, '\\');
  if (!sep) sep = strrchr(dir, '/');
  if (sep) *sep = '\0';
  else { dir[0] = '.'; dir[1] = '\0'; }
}

static SRWLOCK g_lock;
#define cache_lock_init() InitializeSRWLock(&g_lock)
#define cache_rlock()     AcquireSRWLockShared(&g_lock)
#define cache_runlock()   ReleaseSRWLockShared(&g_lock)
#define cache_wlock()     AcquireSRWLockExclusive(&g_lock)
#define cache_wunlock()   ReleaseSRWLockExclusive(&g_lock)

static CRITICAL_SECTION  g_queue_mutex;
static CONDITION_VARIABLE g_queue_cond;
#define queue_mutex_init() InitializeCriticalSection(&g_queue_mutex)
#define queue_lock()       EnterCriticalSection(&g_queue_mutex)
#define queue_unlock()     LeaveCriticalSection(&g_queue_mutex)
#define queue_wait()       SleepConditionVariableCS(&g_queue_cond, &g_queue_mutex, INFINITE)
#define queue_signal()     WakeConditionVariable(&g_queue_cond)
#define THREAD_RET         DWORD WINAPI
#define THREAD_RET_VAL     0

typedef volatile LONG64 atomic_time_t;
static inline void atomic_time_store(atomic_time_t *p, time_t v) {
  InterlockedExchange64((LONGLONG volatile *)p, (LONGLONG)v);
}
static inline time_t atomic_time_load(const atomic_time_t *p) { return (time_t)*p; }

#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#define SOCKET         int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR   (-1)
#define closesocket(s) close(s)
#define os_chdir       chdir
#define os_stat_t      struct stat
#define os_stat        stat

void get_exe_dir(const char *argv0, char *dir, size_t size) {
  strncpy(dir, argv0, size - 1);
  dir[size - 1] = '\0';
  char *sep = strrchr(dir, '/');
  if (sep) *sep = '\0';
  else { dir[0] = '.'; dir[1] = '\0'; }
}

static int fopen_s(FILE **f, const char *name, const char *mode) {
  *f = fopen(name, mode);
  return *f ? 0 : -1;
}

static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;
#define cache_lock_init() ((void)0)
#define cache_rlock()     pthread_rwlock_rdlock(&g_lock)
#define cache_runlock()   pthread_rwlock_unlock(&g_lock)
#define cache_wlock()     pthread_rwlock_wrlock(&g_lock)
#define cache_wunlock()   pthread_rwlock_unlock(&g_lock)

/* Queue mutex/cond only needed on non-Linux (Linux uses SO_REUSEPORT, no queue) */
#ifndef __linux__
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_queue_cond  = PTHREAD_COND_INITIALIZER;
#define queue_mutex_init() ((void)0)
#define queue_lock()       pthread_mutex_lock(&g_queue_mutex)
#define queue_unlock()     pthread_mutex_unlock(&g_queue_mutex)
#define queue_wait()       pthread_cond_wait(&g_queue_cond, &g_queue_mutex)
#define queue_signal()     pthread_cond_signal(&g_queue_cond)
#endif

#define THREAD_RET         void *
#define THREAD_RET_VAL     NULL

typedef _Atomic time_t atomic_time_t;
static inline void atomic_time_store(atomic_time_t *p, time_t v) {
  atomic_store_explicit(p, v, memory_order_relaxed);
}
static inline time_t atomic_time_load(const atomic_time_t *p) {
  return atomic_load_explicit(p, memory_order_relaxed);
}
#endif

/* --- Shared clock: background thread updates g_now every second ---------- */
static volatile time_t g_now;

static THREAD_RET clock_thread(void *_) {
  (void)_;
  for (;;) {
#ifdef _WIN32
    Sleep(1000);
#else
    sleep(1);
#endif
    g_now = time(NULL);
  }
  return THREAD_RET_VAL;
}

static void start_clock(void) {
#ifdef _WIN32
  CloseHandle(CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)clock_thread, NULL, 0, NULL));
#else
  pthread_t t; pthread_create(&t, NULL, clock_thread, NULL); pthread_detach(t);
#endif
}

/* --- Cache --------------------------------------------------------------- */
typedef struct {
  char   path[256];
  char  *data;
  size_t size;
  time_t mtime;
  atomic_time_t last_checked;
  char   header_ka[256];  /* pre-built "Connection: keep-alive" header */
  int    header_ka_len;
  char   header_cl[256];  /* pre-built "Connection: close" header */
  int    header_cl_len;
} CacheEntry;

static CacheEntry g_cache[CACHE_CAP];
static int        g_cache_n;

const char *mime_type(const char *path);

/* Scatter-gather send: header + body in one syscall */
static void send_response(SOCKET client, const char *header, int header_len,
                           const char *data, size_t size) {
#ifdef _WIN32
  WSABUF bufs[2];
  bufs[0].buf = (CHAR *)header; bufs[0].len = (ULONG)header_len;
  bufs[1].buf = (CHAR *)data;   bufs[1].len = (ULONG)size;
  DWORD sent;
  WSASend(client, bufs, 2, &sent, 0, NULL, NULL);
#else
  struct iovec iov[2];
  iov[0].iov_base = (void *)header; iov[0].iov_len = (size_t)header_len;
  iov[1].iov_base = (void *)data;   iov[1].iov_len = size;
  writev(client, iov, 2);
#endif
}

/* Serves path to client directly from cache. Returns 1 on success, 0 on error. */
static int cache_serve(const char *path, SOCKET client, int keep_alive) {
  time_t now = g_now;  /* shared clock — no syscall */

  /* Fast path: entry is fresh — skip stat, send under read lock (no copy) */
  cache_rlock();
  for (int i = 0; i < g_cache_n; i++) {
    CacheEntry *e = &g_cache[i];
    if (strcmp(e->path, path) == 0 &&
        now - atomic_time_load(&e->last_checked) < MTIME_CHECK_INTERVAL) {
      send_response(client,
                    keep_alive ? e->header_ka : e->header_cl,
                    keep_alive ? e->header_ka_len : e->header_cl_len,
                    e->data, e->size);
      cache_runlock();
      return 1;
    }
  }
  cache_runlock();

  /* Slow path: stat to detect changes */
  os_stat_t st;
  if (os_stat(path, &st) != 0) return 0;
  if (st.st_size <= 0 || st.st_size > MAX_FILE_SIZE) return 0;

  /* Cached entry still valid (same mtime)? Send and refresh last_checked atomically. */
  cache_rlock();
  for (int i = 0; i < g_cache_n; i++) {
    CacheEntry *e = &g_cache[i];
    if (strcmp(e->path, path) == 0 && e->mtime == st.st_mtime) {
      send_response(client,
                    keep_alive ? e->header_ka : e->header_cl,
                    keep_alive ? e->header_ka_len : e->header_cl_len,
                    e->data, e->size);
      atomic_time_store(&e->last_checked, now);
      cache_runlock();
      return 1;
    }
  }
  cache_runlock();

  /* Miss or stale: load from disk */
  FILE *f = NULL;
  if (fopen_s(&f, path, "rb") != 0) return 0;
  char *data = malloc((size_t)st.st_size);
  if (!data) { fclose(f); return 0; }
  size_t n = fread(data, 1, (size_t)st.st_size, f);
  fclose(f);

  char hdr_ka[256], hdr_cl[256];
  int hdr_ka_len = snprintf(hdr_ka, sizeof(hdr_ka),
    "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: keep-alive\r\n\r\n",
    mime_type(path), (unsigned long)n);
  int hdr_cl_len = snprintf(hdr_cl, sizeof(hdr_cl),
    "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
    mime_type(path), (unsigned long)n);

  send_response(client, keep_alive ? hdr_ka : hdr_cl,
                         keep_alive ? hdr_ka_len : hdr_cl_len, data, n);

  cache_wlock();
  int slot = g_cache_n < CACHE_CAP ? g_cache_n++ : 0;
  for (int i = 0; i < g_cache_n; i++)
    if (strcmp(g_cache[i].path, path) == 0) { slot = i; break; }
  free(g_cache[slot].data);
  strncpy(g_cache[slot].path, path, sizeof(g_cache[slot].path) - 1);
  g_cache[slot].path[sizeof(g_cache[slot].path) - 1] = '\0';
  g_cache[slot].data         = data;
  g_cache[slot].size         = n;
  g_cache[slot].mtime        = st.st_mtime;
  atomic_time_store(&g_cache[slot].last_checked, now);
  memcpy(g_cache[slot].header_ka, hdr_ka, (size_t)hdr_ka_len);
  g_cache[slot].header_ka_len = hdr_ka_len;
  memcpy(g_cache[slot].header_cl, hdr_cl, (size_t)hdr_cl_len);
  g_cache[slot].header_cl_len = hdr_cl_len;
  cache_wunlock();
  return 1;
}

const char *mime_type(const char *path) {
  static const struct { const char *ext, *type; } map[] = {
    {".html","text/html; charset=utf-8"}, {".htm","text/html; charset=utf-8"},
    {".css", "text/css"},
    {".js",  "application/javascript"},  {".json","application/json"},
    {".txt", "text/plain; charset=utf-8"},{".xml", "application/xml"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},              {".jpeg","image/jpeg"},
    {".gif", "image/gif"},               {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},            {".webp","image/webp"},
    {".pdf", "application/pdf"},
    {".woff","font/woff"},               {".woff2","font/woff2"}, {".ttf","font/ttf"},
    {".mp3", "audio/mpeg"},
    {".mp4", "video/mp4"},               {".webm","video/webm"},
    {".zip", "application/zip"},
  };
  const char *ext = strrchr(path, '.');
  if (ext)
    for (size_t i = 0; i < sizeof(map) / sizeof(*map); i++)
      if (strcmp(ext, map[i].ext) == 0) return map[i].type;
  return "application/octet-stream";
}

/* --- Single-pass request parser ----------------------------------------- */
/* Returns 0=ok, -403=forbidden, -405=method not allowed.
   Fills path[] and *keep_alive on success. */
static int parse_request(const char *buf, char *path, size_t path_cap, int *keep_alive) {
  if (strncmp(buf, "GET ", 4) != 0) return -405;

  /* Extract path; detect ".." in one scan */
  const char *p = buf + 4;
  size_t plen = 0;
  int dotdot = 0;
  while (*p && *p != ' ' && *p != '?') {
    if (*p == '.' && p[1] == '.') dotdot = 1;
    if (plen < path_cap - 1) path[plen++] = *p;
    p++;
  }
  path[plen] = '\0';
  if (dotdot) return -403;

  /* Skip query string to reach the HTTP version token */
  while (*p && *p != ' ') p++;
  if (*p == ' ') p++;
  int http10 = (strncmp(p, "HTTP/1.0", 8) == 0);

  /* Skip to end of request line */
  while (*p && *p != '\n') p++;
  if (*p) p++;

  /* Scan headers for Connection: (stop at blank line) */
  int conn_close = 0, conn_ka = 0;
  while (*p && !(*p == '\r' || *p == '\n')) {
    if (strncmp(p, "Connection: ", 12) == 0) {
      p += 12;
      if      (strncmp(p, "keep-alive", 10) == 0) conn_ka    = 1;
      else if (strncmp(p, "close",       5) == 0) conn_close = 1;
    }
    while (*p && *p != '\n') p++;
    if (*p) p++;
  }

  *keep_alive = !conn_close && (!http10 || conn_ka);
  return 0;
}

/* --- Connection handler -------------------------------------------------- */
void handle_client(SOCKET client) {
#ifdef _WIN32
  DWORD tv = 5000;
#else
  struct timeval tv = {5, 0};
#endif
  setsockopt(client, SOL_SOCKET,  SO_RCVTIMEO,  (const char *)&tv,  sizeof(tv));
  int nodelay = 1;
  setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));

  char buffer[MAX_REQ];
  for (;;) {
    int nr = recv(client, buffer, sizeof(buffer) - 1, 0);
    if (nr <= 0) break;
    buffer[nr] = '\0';

    char path[512];
    int  keep_alive;
    int  status = parse_request(buffer, path, sizeof(path), &keep_alive);

    if (status == -405) {
      send(client, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\nMethod Not Allowed", 73, 0);
      break;
    }
    if (status == -403) {
      send(client, "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\nForbidden", 55, 0);
      break;
    }

    const char *local_path = (path[0] == '/' && path[1] == '\0') ? "index.html" : path + 1;

    if (!cache_serve(local_path, client, keep_alive)) {
      send(client, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNot Found", 55, 0);
      break;
    }

    if (!keep_alive) break;
  }
  closesocket(client);
}

/* --- Thread pool --------------------------------------------------------- */
#ifdef __linux__

/* On Linux: SO_REUSEPORT — each worker owns its socket, no shared queue.
   The kernel distributes accepted connections across all worker sockets. */
static THREAD_RET worker_thread(void *arg) {
  int port = (int)(intptr_t)arg;

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) return THREAD_RET_VAL;

  int opt = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(srv, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

  struct sockaddr_in addr = {0};
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port        = htons((unsigned short)port);

  if (bind(srv,  (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(srv, SOMAXCONN) != 0) {
    close(srv);
    return THREAD_RET_VAL;
  }

  for (;;) {
    int client = accept(srv, NULL, NULL);
    if (client >= 0) handle_client(client);
  }
  return THREAD_RET_VAL;
}

static void start_pool(int port) {
  for (int i = 0; i < POOL_SIZE; i++) {
    pthread_t t;
    pthread_create(&t, NULL, worker_thread, (void *)(intptr_t)port);
    pthread_detach(t);
  }
}

#else

/* Windows / macOS: queue-based pool with shared accept loop in main() */
static SOCKET g_queue[QUEUE_CAP];
static int    g_queue_head, g_queue_tail, g_queue_len;

static THREAD_RET worker_thread(void *_) {
  (void)_;
  for (;;) {
    queue_lock();
    while (g_queue_len == 0) queue_wait();
    SOCKET s = g_queue[g_queue_head];
    g_queue_head = (g_queue_head + 1) % QUEUE_CAP;
    g_queue_len--;
    queue_unlock();
    handle_client(s);
  }
  return THREAD_RET_VAL;
}

static void enqueue_client(SOCKET s) {
  queue_lock();
  if (g_queue_len < QUEUE_CAP) {
    g_queue[g_queue_tail] = s;
    g_queue_tail = (g_queue_tail + 1) % QUEUE_CAP;
    g_queue_len++;
    queue_signal();
  } else {
    closesocket(s);
  }
  queue_unlock();
}

static void start_pool(int port) {
  (void)port;
  queue_mutex_init();
  for (int i = 0; i < POOL_SIZE; i++) {
#ifdef _WIN32
    CloseHandle(CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)worker_thread, NULL, 0, NULL));
#else
    pthread_t t; pthread_create(&t, NULL, worker_thread, NULL); pthread_detach(t);
#endif
  }
}

#endif  /* __linux__ */

/* --- main --------------------------------------------------------------- */
int main(int argc, char *argv[]) {
  int port = DEFAULT_PORT;
  char serve_dir[512];
  get_exe_dir(argv[0], serve_dir, sizeof(serve_dir));

  if (argc >= 2) {
    port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
      printf("Invalid port: %s\n", argv[1]);
      return 1;
    }
  }
  if (argc >= 3) {
    strncpy(serve_dir, argv[2], sizeof(serve_dir) - 1);
    serve_dir[sizeof(serve_dir) - 1] = '\0';
  }

  cache_lock_init();
  g_now = time(NULL);
  start_clock();

  if (os_chdir(serve_dir) != 0) {
    printf("Failed to change to directory: %s\n", serve_dir);
    return 1;
  }

#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#else
  signal(SIGPIPE, SIG_IGN);
#endif

  start_pool(port);

  printf("Serving files from: %s\n", serve_dir);
  printf("Tiny C Web Server listening on http://localhost:%d\n", port);
  printf("Press Ctrl+C to exit.\n");

#ifdef __linux__
  /* Workers own their sockets; main thread just waits for a signal */
  for (;;) pause();
#else
  SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
  if (server == INVALID_SOCKET) return 1;

  int opt = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

  struct sockaddr_in addr = {0};
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port        = htons((unsigned short)port);

  if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
    printf("Bind failed. Port %d maybe in use?\n", port);
    return 1;
  }
  if (listen(server, SOMAXCONN) == SOCKET_ERROR) return 1;

  while (1) {
    SOCKET client = accept(server, NULL, NULL);
    if (client != INVALID_SOCKET)
      enqueue_client(client);
  }
#endif
}
