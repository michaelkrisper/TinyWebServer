#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DEFAULT_PORT  80
#define MAX_REQ       2048
#define MAX_FILE_SIZE (10 * 1024 * 1024)
#define CACHE_CAP            64
#define MTIME_CHECK_INTERVAL  10   /* seconds between mtime re-checks */
#define POOL_SIZE     128
#define QUEUE_CAP     512

/* =========================================================================
   Platform: Windows
   ========================================================================= */
#ifdef _WIN32
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

#define THREAD_RET         DWORD WINAPI
#define THREAD_RET_VAL     0

typedef volatile LONG64 atomic_time_t;
static inline void atomic_time_store(atomic_time_t *p, time_t v) {
  InterlockedExchange64((LONGLONG volatile *)p, (LONGLONG)v);
}
static inline time_t atomic_time_load(const atomic_time_t *p) { return (time_t)*p; }

/* =========================================================================
   Platform: POSIX
   ========================================================================= */
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/socket.h>
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

#define THREAD_RET         void *
#define THREAD_RET_VAL     NULL

typedef _Atomic time_t atomic_time_t;
static inline void atomic_time_store(atomic_time_t *p, time_t v) {
  atomic_store_explicit(p, v, memory_order_relaxed);
}
static inline time_t atomic_time_load(const atomic_time_t *p) {
  return atomic_load_explicit(p, memory_order_relaxed);
}
#endif /* _WIN32 */

/* =========================================================================
   io_uring (Linux)
   ========================================================================= */
#if defined(__linux__)
#  if __has_include(<liburing.h>)
#    include <liburing.h>
#    define USE_IOURING 1
#  else
#    define USE_IOURING 0
#  endif
#endif

/* =========================================================================
   kqueue (macOS / BSD)
   ========================================================================= */
#if defined(__APPLE__) || defined(__FreeBSD__)
#  include <fcntl.h>
#  include <sys/event.h>
#  define USE_KQUEUE 1
#endif

/* =========================================================================
   Shared clock + Date header
   ========================================================================= */
static volatile time_t g_now;
static char            g_date_hdr[48]; /* "Date: Mon, 16 Mar 2026 12:00:01 GMT\r\n\r\n" */
static int             g_date_hdr_len;

static void gmtime_safe(time_t t, struct tm *out) {
#ifdef _WIN32
  gmtime_s(out, &t);
#else
  gmtime_r(&t, out);
#endif
}

static void update_date_hdr(time_t t) {
  struct tm tm_buf;
  gmtime_safe(t, &tm_buf);
  char tmp[48];
  int len = (int)strftime(tmp, sizeof(tmp),
                           "Date: %a, %d %b %Y %H:%M:%S GMT\r\n\r\n", &tm_buf);
  memcpy(g_date_hdr, tmp, (size_t)len + 1);
  g_date_hdr_len = len;
}

static void format_http_date(time_t t, char *buf, size_t size) {
  struct tm tm_buf;
  gmtime_safe(t, &tm_buf);
  strftime(buf, size, "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);
}

static THREAD_RET clock_thread(void *_) {
  (void)_;
  for (;;) {
#ifdef _WIN32
    Sleep(1000);
#else
    sleep(1);
#endif
    time_t now = time(NULL);
    g_now = now;
    update_date_hdr(now);
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

/* =========================================================================
   Cache
   ========================================================================= */
typedef struct {
  char   path[256];
  char  *data;
  size_t size;
  time_t mtime;
  atomic_time_t last_checked;
  char   header_ka[320];  /* pre-built keep-alive header (no final blank line) */
  int    header_ka_len;
  char   header_cl[320];  /* pre-built close header (no final blank line) */
  int    header_cl_len;
} CacheEntry;

static CacheEntry g_cache[CACHE_CAP];
static int        g_cache_n;

const char *mime_type(const char *path);

/* Error/304 response headers (no trailing blank line — g_date_hdr provides it) */
static const char hdr_304_ka[] = "HTTP/1.1 304 Not Modified\r\nConnection: keep-alive\r\n";
static const char hdr_304_cl[] = "HTTP/1.1 304 Not Modified\r\nConnection: close\r\n";
static const char hdr_403[]    = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n";
static const char hdr_404[]    = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n";
static const char hdr_405[]    = "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n";

/* Build a flat malloc'd buffer [header][g_date_hdr][body].
   Returns NULL on file-not-found or OOM. Caller frees. */
static char *build_response(const char *path, int keep_alive,
                              time_t ims, size_t *resp_len) {
  time_t now = g_now;

  /* Fast path: fresh cache hit */
  cache_rlock();
  for (int i = 0; i < g_cache_n; i++) {
    CacheEntry *e = &g_cache[i];
    if (strcmp(e->path, path) == 0 &&
        now - atomic_time_load(&e->last_checked) < MTIME_CHECK_INTERVAL) {
      const char *hdr;  int hdr_len;
      const char *body = NULL;  size_t body_sz = 0;
      if (ims && ims >= e->mtime) {
        hdr = keep_alive ? hdr_304_ka : hdr_304_cl;
        hdr_len = keep_alive ? (int)(sizeof(hdr_304_ka)-1) : (int)(sizeof(hdr_304_cl)-1);
      } else {
        hdr = keep_alive ? e->header_ka : e->header_cl;
        hdr_len = keep_alive ? e->header_ka_len : e->header_cl_len;
        body = e->data;  body_sz = e->size;
      }
      size_t total = (size_t)hdr_len + (size_t)g_date_hdr_len + body_sz;
      char *buf = malloc(total);
      if (buf) {
        memcpy(buf,                            hdr,        (size_t)hdr_len);
        memcpy(buf + hdr_len,                  g_date_hdr, (size_t)g_date_hdr_len);
        if (body_sz) memcpy(buf + hdr_len + g_date_hdr_len, body, body_sz);
        *resp_len = total;
      }
      cache_runlock();
      return buf;
    }
  }
  cache_runlock();

  /* Slow path: stat to detect changes */
  os_stat_t st;
  if (os_stat(path, &st) != 0) return NULL;
  if (st.st_size <= 0 || st.st_size > MAX_FILE_SIZE) return NULL;

  cache_rlock();
  for (int i = 0; i < g_cache_n; i++) {
    CacheEntry *e = &g_cache[i];
    if (strcmp(e->path, path) == 0 && e->mtime == st.st_mtime) {
      const char *hdr;  int hdr_len;
      const char *body = NULL;  size_t body_sz = 0;
      if (ims && ims >= e->mtime) {
        hdr = keep_alive ? hdr_304_ka : hdr_304_cl;
        hdr_len = keep_alive ? (int)(sizeof(hdr_304_ka)-1) : (int)(sizeof(hdr_304_cl)-1);
      } else {
        hdr = keep_alive ? e->header_ka : e->header_cl;
        hdr_len = keep_alive ? e->header_ka_len : e->header_cl_len;
        body = e->data;  body_sz = e->size;
      }
      atomic_time_store(&e->last_checked, now);
      size_t total = (size_t)hdr_len + (size_t)g_date_hdr_len + body_sz;
      char *buf = malloc(total);
      if (buf) {
        memcpy(buf,                            hdr,        (size_t)hdr_len);
        memcpy(buf + hdr_len,                  g_date_hdr, (size_t)g_date_hdr_len);
        if (body_sz) memcpy(buf + hdr_len + g_date_hdr_len, body, body_sz);
        *resp_len = total;
      }
      cache_runlock();
      return buf;
    }
  }
  cache_runlock();

  /* Cache miss: load from disk */
  FILE *f = NULL;
  if (fopen_s(&f, path, "rb") != 0) return NULL;
  char *data = malloc((size_t)st.st_size);
  if (!data) { fclose(f); return NULL; }
  size_t n = fread(data, 1, (size_t)st.st_size, f);
  fclose(f);

  char lm[32];
  format_http_date(st.st_mtime, lm, sizeof(lm));

  char hdr_ka[320], hdr_cl[320];
  int hdr_ka_len = snprintf(hdr_ka, sizeof(hdr_ka),
    "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\nLast-Modified: %s\r\nConnection: keep-alive\r\n",
    mime_type(path), (unsigned long)n, lm);
  int hdr_cl_len = snprintf(hdr_cl, sizeof(hdr_cl),
    "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\nLast-Modified: %s\r\nConnection: close\r\n",
    mime_type(path), (unsigned long)n, lm);

  /* Store in cache */
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

  /* Build flat response buffer */
  const char *hdr;  int hdr_len;
  const char *body = NULL;  size_t body_sz = 0;
  if (ims && ims >= st.st_mtime) {
    hdr = keep_alive ? hdr_304_ka : hdr_304_cl;
    hdr_len = keep_alive ? (int)(sizeof(hdr_304_ka)-1) : (int)(sizeof(hdr_304_cl)-1);
  } else {
    hdr = keep_alive ? hdr_ka : hdr_cl;
    hdr_len = keep_alive ? hdr_ka_len : hdr_cl_len;
    body = data;  body_sz = n;
  }
  size_t total = (size_t)hdr_len + (size_t)g_date_hdr_len + body_sz;
  char *buf = malloc(total);
  if (buf) {
    memcpy(buf,                            hdr,        (size_t)hdr_len);
    memcpy(buf + hdr_len,                  g_date_hdr, (size_t)g_date_hdr_len);
    if (body_sz) memcpy(buf + hdr_len + g_date_hdr_len, body, body_sz);
    *resp_len = total;
  }
  return buf;
}

/* Build a flat malloc'd error response buffer [hdr][g_date_hdr][body]. */
static char *build_error(const char *hdr, int hdr_len,
                          const char *body, int body_len,
                          size_t *resp_len) {
  size_t total = (size_t)hdr_len + (size_t)g_date_hdr_len + (size_t)body_len;
  char *buf = malloc(total);
  if (!buf) return NULL;
  memcpy(buf,                            hdr,        (size_t)hdr_len);
  memcpy(buf + hdr_len,                  g_date_hdr, (size_t)g_date_hdr_len);
  memcpy(buf + hdr_len + g_date_hdr_len, body,       (size_t)body_len);
  *resp_len = total;
  return buf;
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

/* =========================================================================
   HTTP date parser (RFC 7231)
   ========================================================================= */
static time_t parse_http_date(const char *s) {
  struct tm tm = {0};
  char month[4] = {0};
  if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d GMT",
             &tm.tm_mday, month, &tm.tm_year,
             &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) return 0;
  static const char *months[] = {
    "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
  };
  tm.tm_mon = -1;
  for (int i = 0; i < 12; i++)
    if (strncmp(month, months[i], 3) == 0) { tm.tm_mon = i; break; }
  if (tm.tm_mon < 0) return 0;
  tm.tm_year -= 1900;
#ifdef _WIN32
  return _mkgmtime(&tm);
#else
  return timegm(&tm);
#endif
}

/* =========================================================================
   Single-pass request parser
   Returns 0=ok, -403=forbidden, -405=method not allowed.
   ========================================================================= */
static int parse_request(const char *buf, char *path, size_t path_cap,
                          int *keep_alive, time_t *ims) {
  if (strncmp(buf, "GET ", 4) != 0) return -405;

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

  while (*p && *p != ' ') p++;
  if (*p == ' ') p++;
  int http10 = (strncmp(p, "HTTP/1.0", 8) == 0);
  while (*p && *p != '\n') p++;
  if (*p) p++;

  int conn_close = 0, conn_ka = 0;
  *ims = 0;
  while (*p && !(*p == '\r' || *p == '\n')) {
    if (strncmp(p, "Connection: ", 12) == 0) {
      p += 12;
      if      (strncmp(p, "keep-alive", 10) == 0) conn_ka    = 1;
      else if (strncmp(p, "close",       5) == 0) conn_close = 1;
    } else if (strncmp(p, "If-Modified-Since: ", 19) == 0) {
      *ims = parse_http_date(p + 19);
    }
    while (*p && *p != '\n') p++;
    if (*p) p++;
  }

  *keep_alive = !conn_close && (!http10 || conn_ka);
  return 0;
}

/* =========================================================================
   ConnCtx — per-connection state
   ========================================================================= */
#define CONN_OP_RECV 1
#define CONN_OP_SEND 2

#ifdef _WIN32
typedef struct {
  OVERLAPPED ovl;   /* MUST be first — GQCS casts OVERLAPPED* → ConnCtx* */
  SOCKET     sock;
  int        op, keep_alive;
  char       req[MAX_REQ];
  char      *resp;
  size_t     resp_len, resp_off;
} ConnCtx;
#define conn_fd(c) ((c)->sock)
#else
typedef struct {
  int    fd, op, keep_alive;
  char   req[MAX_REQ];
  char  *resp;
  size_t resp_len, resp_off;
} ConnCtx;
#define conn_fd(c) ((c)->fd)
#endif

static void free_conn(ConnCtx *ctx) {
  closesocket(conn_fd(ctx));
  free(ctx->resp);
  free(ctx);
}

/* Parse ctx->req, build ctx->resp. Returns 0 on success, -1 on OOM. */
static int dispatch_request(ConnCtx *ctx) {
  char   path[512];
  int    keep_alive;
  time_t ims;
  int    status = parse_request(ctx->req, path, sizeof(path), &keep_alive, &ims);

  free(ctx->resp);
  ctx->resp = NULL;

  if (status == -405) {
    ctx->keep_alive = 0;
    ctx->resp = build_error(hdr_405, (int)(sizeof(hdr_405)-1),
                            "Method Not Allowed", 18, &ctx->resp_len);
    return ctx->resp ? 0 : -1;
  }
  if (status == -403) {
    ctx->keep_alive = 0;
    ctx->resp = build_error(hdr_403, (int)(sizeof(hdr_403)-1),
                            "Forbidden", 9, &ctx->resp_len);
    return ctx->resp ? 0 : -1;
  }

  const char *local_path = (path[0] == '/' && path[1] == '\0') ? "index.html" : path + 1;
  ctx->keep_alive = keep_alive;
  ctx->resp = build_response(local_path, keep_alive, ims, &ctx->resp_len);
  if (!ctx->resp) {
    ctx->keep_alive = 0;
    ctx->resp = build_error(hdr_404, (int)(sizeof(hdr_404)-1),
                            "Not Found", 9, &ctx->resp_len);
    return ctx->resp ? 0 : -1;
  }
  return 0;
}

/* =========================================================================
   WINDOWS — IOCP
   ========================================================================= */
#ifdef _WIN32

static HANDLE g_iocp;

static void post_recv(ConnCtx *ctx) {
  memset(&ctx->ovl, 0, sizeof ctx->ovl);   /* MUST zero before every reuse */
  WSABUF wb = { MAX_REQ - 1, ctx->req };
  DWORD  flags = 0;
  if (WSARecv(ctx->sock, &wb, 1, NULL, &flags, &ctx->ovl, NULL) == SOCKET_ERROR
      && WSAGetLastError() != WSA_IO_PENDING)
    free_conn(ctx);
}

static void post_send(ConnCtx *ctx) {
  memset(&ctx->ovl, 0, sizeof ctx->ovl);
  WSABUF wb = { (ULONG)(ctx->resp_len - ctx->resp_off),
                ctx->resp + ctx->resp_off };
  if (WSASend(ctx->sock, &wb, 1, NULL, 0, &ctx->ovl, NULL) == SOCKET_ERROR
      && WSAGetLastError() != WSA_IO_PENDING)
    free_conn(ctx);
}

static THREAD_RET iocp_worker(void *_) {
  (void)_;
  for (;;) {
    DWORD      bytes;
    ULONG_PTR  key;
    OVERLAPPED *ovl;
    if (!GetQueuedCompletionStatus(g_iocp, &bytes, &key, &ovl, INFINITE) || !ovl) {
      if (ovl) free_conn((ConnCtx *)ovl);
      continue;
    }
    ConnCtx *ctx = (ConnCtx *)ovl;

    if (ctx->op == CONN_OP_RECV) {
      if (bytes == 0) { free_conn(ctx); continue; }
      ctx->req[bytes] = '\0';
      if (dispatch_request(ctx) < 0) { free_conn(ctx); continue; }
      ctx->op = CONN_OP_SEND;
      ctx->resp_off = 0;
      post_send(ctx);
    } else { /* CONN_OP_SEND */
      ctx->resp_off += bytes;
      if (ctx->resp_off < ctx->resp_len) {
        post_send(ctx);
      } else {
        free(ctx->resp);  ctx->resp = NULL;
        if (ctx->keep_alive) {
          ctx->op = CONN_OP_RECV;
          post_recv(ctx);
        } else {
          free_conn(ctx);
        }
      }
    }
  }
  return THREAD_RET_VAL;
}

static void start_pool(int port) {
  (void)port;
  g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
  for (int i = 0; i < POOL_SIZE; i++)
    CloseHandle(CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)iocp_worker, NULL, 0, NULL));
}

/* =========================================================================
   LINUX — io_uring (with blocking fallback when liburing is absent)
   ========================================================================= */
#elif defined(__linux__)

#if USE_IOURING

#define URING_CAP    512
#define ACCEPT_COOKIE ((uint64_t)1)  /* sentinel; pointers are ≥ 8-byte aligned */

static void submit_recv(struct io_uring *r, ConnCtx *ctx) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(r);
  if (!sqe) return;
  io_uring_prep_recv(sqe, ctx->fd, ctx->req, MAX_REQ - 1, 0);
  io_uring_sqe_set_data(sqe, ctx);
  io_uring_submit(r);
}

static void submit_send(struct io_uring *r, ConnCtx *ctx) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(r);
  if (!sqe) return;
  io_uring_prep_send(sqe, ctx->fd,
                     ctx->resp + ctx->resp_off,
                     ctx->resp_len - ctx->resp_off, 0);
  io_uring_sqe_set_data(sqe, ctx);
  io_uring_submit(r);
}

static void submit_accept(struct io_uring *r, int srv) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(r);
  if (!sqe) return;
#ifdef IORING_ACCEPT_MULTISHOT
  io_uring_prep_multishot_accept(sqe, srv, NULL, NULL, 0);
#else
  io_uring_prep_accept(sqe, srv, NULL, NULL, 0);
#endif
  io_uring_sqe_set_data64(sqe, ACCEPT_COOKIE);
  io_uring_submit(r);
}

static THREAD_RET uring_worker(void *arg) {
  int port = (int)(intptr_t)arg;

  /* Per-thread listen socket (SO_REUSEPORT — kernel balances connections) */
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) return THREAD_RET_VAL;
  int opt = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
  setsockopt(srv, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof opt);
#ifdef TCP_DEFER_ACCEPT
  setsockopt(srv, IPPROTO_TCP, TCP_DEFER_ACCEPT, &opt, sizeof opt);
#endif
  struct sockaddr_in addr = {0};
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port        = htons((unsigned short)port);
  if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0 ||
      listen(srv, SOMAXCONN) != 0) {
    close(srv);
    return THREAD_RET_VAL;
  }

  /* Ring init: try Ubuntu 24.04 / kernel 6.1+ flags, fall back on older kernels */
  struct io_uring ring;
#if defined(IORING_SETUP_SINGLE_ISSUER) && defined(IORING_SETUP_DEFER_TASKRUN)
  struct io_uring_params params = {0};
  params.flags = IORING_SETUP_SINGLE_ISSUER
               | IORING_SETUP_COOP_TASKRUN
               | IORING_SETUP_DEFER_TASKRUN;
  if (io_uring_queue_init_params(URING_CAP, &ring, &params) < 0) {
    if (io_uring_queue_init(URING_CAP, &ring, 0) < 0) {
      close(srv);
      return THREAD_RET_VAL;
    }
  }
#else
  if (io_uring_queue_init(URING_CAP, &ring, 0) < 0) {
    close(srv);
    return THREAD_RET_VAL;
  }
#endif

  submit_accept(&ring, srv);

  for (;;) {
    struct io_uring_cqe *cqe;
    if (io_uring_wait_cqe(&ring, &cqe) < 0) continue;

    if (cqe->user_data == ACCEPT_COOKIE) {
      int fd = cqe->res;
      if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        ConnCtx *ctx = calloc(1, sizeof *ctx);
        if (ctx) {
          ctx->fd = fd;
          ctx->op = CONN_OP_RECV;
          submit_recv(&ring, ctx);
        } else {
          close(fd);
        }
      }
      /* Re-arm only if multishot accept has terminated */
#ifdef IORING_ACCEPT_MULTISHOT
      if (!(cqe->flags & IORING_CQE_F_MORE))
        submit_accept(&ring, srv);
#else
      submit_accept(&ring, srv);
#endif

    } else {
      ConnCtx *ctx = (ConnCtx *)(uintptr_t)cqe->user_data;
      int res = cqe->res;

      if (ctx->op == CONN_OP_RECV) {
        if (res <= 0) { free_conn(ctx); goto seen; }
        ctx->req[res] = '\0';
        if (dispatch_request(ctx) < 0) { free_conn(ctx); goto seen; }
        ctx->op = CONN_OP_SEND;
        ctx->resp_off = 0;
        submit_send(&ring, ctx);

      } else { /* CONN_OP_SEND */
        if (res < 0) { free_conn(ctx); goto seen; }
        ctx->resp_off += (size_t)res;
        if (ctx->resp_off < ctx->resp_len) {
          submit_send(&ring, ctx);
        } else {
          free(ctx->resp);  ctx->resp = NULL;
          if (ctx->keep_alive) {
            ctx->op = CONN_OP_RECV;
            submit_recv(&ring, ctx);
          } else {
            free_conn(ctx);
          }
        }
      }
    }
seen:
    io_uring_cqe_seen(&ring, cqe);
  }
  return THREAD_RET_VAL;
}

#else /* !USE_IOURING — blocking SO_REUSEPORT fallback */

static THREAD_RET uring_worker(void *arg) {
  int port = (int)(intptr_t)arg;

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) return THREAD_RET_VAL;
  int opt = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
  setsockopt(srv, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof opt);
#ifdef TCP_DEFER_ACCEPT
  setsockopt(srv, IPPROTO_TCP, TCP_DEFER_ACCEPT, &opt, sizeof opt);
#endif
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons((unsigned short)port);
  if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0 ||
      listen(srv, SOMAXCONN) != 0) { close(srv); return THREAD_RET_VAL; }

  for (;;) {
    int fd = accept(srv, NULL, NULL);
    if (fd < 0) continue;

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    ConnCtx ctx = {0};
    ctx.fd = fd;
    for (;;) {
      int nr = recv(fd, ctx.req, MAX_REQ - 1, 0);
      if (nr <= 0) break;
      ctx.req[nr] = '\0';
      if (dispatch_request(&ctx) < 0) break;
      size_t off = 0;
      while (off < ctx.resp_len) {
        ssize_t ns = send(fd, ctx.resp + off, ctx.resp_len - off, 0);
        if (ns <= 0) { free(ctx.resp); ctx.resp = NULL; goto close_fd; }
        off += (size_t)ns;
      }
      free(ctx.resp);  ctx.resp = NULL;
      if (!ctx.keep_alive) break;
    }
close_fd:
    free(ctx.resp);
    close(fd);
  }
  return THREAD_RET_VAL;
}

#endif /* USE_IOURING */

static void start_pool(int port) {
  for (int i = 0; i < POOL_SIZE; i++) {
    pthread_t t;
    pthread_create(&t, NULL, uring_worker, (void *)(intptr_t)port);
    pthread_detach(t);
  }
}

/* =========================================================================
   macOS / BSD — kqueue
   ========================================================================= */
#elif defined(USE_KQUEUE)

#define ACCEPT_COOKIE ((uintptr_t)1)
#define KEV_BATCH     64

static THREAD_RET kqueue_worker(void *arg) {
  int port = (int)(intptr_t)arg;

  /* Per-thread listen socket (SO_REUSEPORT) */
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) return THREAD_RET_VAL;
  int opt = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
  setsockopt(srv, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof opt);
  fcntl(srv, F_SETFL, O_NONBLOCK);
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons((unsigned short)port);
  if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0 ||
      listen(srv, SOMAXCONN) != 0) { close(srv); return THREAD_RET_VAL; }

  int kq = kqueue();
  if (kq < 0) { close(srv); return THREAD_RET_VAL; }

  struct kevent ev;
  EV_SET(&ev, (uintptr_t)srv, EVFILT_READ, EV_ADD, 0, 0, (void *)ACCEPT_COOKIE);
  kevent(kq, &ev, 1, NULL, 0, NULL);

  struct kevent events[KEV_BATCH];
  for (;;) {
    int n = kevent(kq, NULL, 0, events, KEV_BATCH, NULL);
    for (int i = 0; i < n; i++) {
      if (events[i].udata == (void *)ACCEPT_COOKIE) {
        /* Accept all pending connections */
        int fd;
        while ((fd = accept(srv, NULL, NULL)) >= 0) {
          fcntl(fd, F_SETFL, O_NONBLOCK);
          int one = 1;
          setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
          ConnCtx *ctx = calloc(1, sizeof *ctx);
          if (!ctx) { close(fd); continue; }
          ctx->fd = fd;  ctx->op = CONN_OP_RECV;
          EV_SET(&ev, (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, ctx);
          kevent(kq, &ev, 1, NULL, 0, NULL);
        }
      } else {
        ConnCtx *ctx = (ConnCtx *)events[i].udata;

        if (ctx->op == CONN_OP_RECV) {
          ssize_t nr = recv(ctx->fd, ctx->req, MAX_REQ - 1, 0);
          if (nr <= 0) { free_conn(ctx); continue; }
          ctx->req[nr] = '\0';
          if (dispatch_request(ctx) < 0) { free_conn(ctx); continue; }
          ctx->op = CONN_OP_SEND;
          ctx->resp_off = 0;
          EV_SET(&ev, (uintptr_t)ctx->fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, ctx);
          kevent(kq, &ev, 1, NULL, 0, NULL);

        } else { /* CONN_OP_SEND */
          ssize_t ns = send(ctx->fd,
                            ctx->resp + ctx->resp_off,
                            ctx->resp_len - ctx->resp_off, 0);
          if (ns < 0) { free_conn(ctx); continue; }
          ctx->resp_off += (size_t)ns;
          if (ctx->resp_off < ctx->resp_len) {
            EV_SET(&ev, (uintptr_t)ctx->fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT, 0, 0, ctx);
            kevent(kq, &ev, 1, NULL, 0, NULL);
          } else {
            free(ctx->resp);  ctx->resp = NULL;
            if (ctx->keep_alive) {
              ctx->op = CONN_OP_RECV;
              EV_SET(&ev, (uintptr_t)ctx->fd, EVFILT_READ, EV_ADD | EV_ONESHOT, 0, 0, ctx);
              kevent(kq, &ev, 1, NULL, 0, NULL);
            } else {
              free_conn(ctx);
            }
          }
        }
      }
    }
  }
  return THREAD_RET_VAL;
}

static void start_pool(int port) {
  for (int i = 0; i < POOL_SIZE; i++) {
    pthread_t t;
    pthread_create(&t, NULL, kqueue_worker, (void *)(intptr_t)port);
    pthread_detach(t);
  }
}

/* =========================================================================
   Generic POSIX fallback (queue-based)
   ========================================================================= */
#else

static int g_queue_fds[QUEUE_CAP];
static int g_queue_head, g_queue_tail, g_queue_len;
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_queue_cond  = PTHREAD_COND_INITIALIZER;

static THREAD_RET worker_thread(void *_) {
  (void)_;
  for (;;) {
    pthread_mutex_lock(&g_queue_mutex);
    while (g_queue_len == 0) pthread_cond_wait(&g_queue_cond, &g_queue_mutex);
    int fd = g_queue_fds[g_queue_head];
    g_queue_head = (g_queue_head + 1) % QUEUE_CAP;
    g_queue_len--;
    pthread_mutex_unlock(&g_queue_mutex);

    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    ConnCtx ctx = {0};
    ctx.fd = fd;
    for (;;) {
      int nr = recv(fd, ctx.req, MAX_REQ - 1, 0);
      if (nr <= 0) break;
      ctx.req[nr] = '\0';
      if (dispatch_request(&ctx) < 0) break;
      size_t off = 0;
      while (off < ctx.resp_len) {
        ssize_t ns = send(fd, ctx.resp + off, ctx.resp_len - off, 0);
        if (ns <= 0) { free(ctx.resp); ctx.resp = NULL; goto close_fd; }
        off += (size_t)ns;
      }
      free(ctx.resp);  ctx.resp = NULL;
      if (!ctx.keep_alive) break;
    }
close_fd:
    free(ctx.resp);
    close(fd);
  }
  return THREAD_RET_VAL;
}

static void enqueue_fd(int fd) {
  pthread_mutex_lock(&g_queue_mutex);
  if (g_queue_len < QUEUE_CAP) {
    g_queue_fds[g_queue_tail] = fd;
    g_queue_tail = (g_queue_tail + 1) % QUEUE_CAP;
    g_queue_len++;
    pthread_cond_signal(&g_queue_cond);
  } else {
    close(fd);
  }
  pthread_mutex_unlock(&g_queue_mutex);
}

static void start_pool(int port) {
  (void)port;
  for (int i = 0; i < POOL_SIZE; i++) {
    pthread_t t;
    pthread_create(&t, NULL, worker_thread, NULL);
    pthread_detach(t);
  }
}

#endif /* platform */

/* =========================================================================
   main
   ========================================================================= */
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
  time_t now = time(NULL);
  g_now = now;
  update_date_hdr(now);
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
  printf("Tiny Web Server listening on http://localhost:%d\n", port);
  printf("Press Ctrl+C to exit.\n");

#ifdef _WIN32
  /* IOCP: main thread accepts + attaches each socket to the completion port */
  SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
  if (server == INVALID_SOCKET) return 1;
  int opt = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof opt);
  struct sockaddr_in addr = {0};
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port        = htons((unsigned short)port);
  if (bind(server, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR) {
    printf("Bind failed. Port %d maybe in use?\n", port);
    return 1;
  }
  if (listen(server, SOMAXCONN) == SOCKET_ERROR) return 1;
  for (;;) {
    SOCKET client = accept(server, NULL, NULL);
    if (client == INVALID_SOCKET) continue;
    int one = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    CreateIoCompletionPort((HANDLE)client, g_iocp, 0, 0);
    ConnCtx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) { closesocket(client); continue; }
    ctx->sock = client;
    ctx->op   = CONN_OP_RECV;
    post_recv(ctx);
  }

#elif defined(__linux__) || defined(USE_KQUEUE)
  /* Workers own their sockets via SO_REUSEPORT — main just waits */
  for (;;) pause();

#else
  /* Generic POSIX: main accepts and enqueues to the thread pool */
  SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
  if (server == INVALID_SOCKET) return 1;
  int opt = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof opt);
  struct sockaddr_in addr = {0};
  addr.sin_family      = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port        = htons((unsigned short)port);
  if (bind(server, (struct sockaddr *)&addr, sizeof addr) == SOCKET_ERROR) {
    printf("Bind failed. Port %d maybe in use?\n", port);
    return 1;
  }
  if (listen(server, SOMAXCONN) == SOCKET_ERROR) return 1;
  for (;;) {
    int client = accept(server, NULL, NULL);
    if (client >= 0) enqueue_fd(client);
  }
#endif
}
