#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DEFAULT_PORT  80
#define MAX_REQ       2048
#define MAX_FILE_SIZE (10 * 1024 * 1024)
#define CACHE_CAP     64
#define POOL_SIZE     32
#define QUEUE_CAP     256

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

#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
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

static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_queue_cond  = PTHREAD_COND_INITIALIZER;
#define queue_mutex_init() ((void)0)
#define queue_lock()       pthread_mutex_lock(&g_queue_mutex)
#define queue_unlock()     pthread_mutex_unlock(&g_queue_mutex)
#define queue_wait()       pthread_cond_wait(&g_queue_cond, &g_queue_mutex)
#define queue_signal()     pthread_cond_signal(&g_queue_cond)
#define THREAD_RET         void *
#define THREAD_RET_VAL     NULL
#endif

typedef struct {
  char   path[256];
  char  *data;
  size_t size;
  time_t mtime;
} CacheEntry;

static CacheEntry g_cache[CACHE_CAP];
static int        g_cache_n;

/* Returns a malloc'd copy of the file data, or NULL on error. */
static char *cache_get(const char *path, size_t *out_size) {
  os_stat_t st;
  if (os_stat(path, &st) != 0) return NULL;
  if (st.st_size <= 0 || st.st_size > MAX_FILE_SIZE) return NULL;
  time_t mtime = st.st_mtime;

  cache_rlock();
  for (int i = 0; i < g_cache_n; i++) {
    if (strcmp(g_cache[i].path, path) == 0 && g_cache[i].mtime == mtime) {
      char *copy = malloc(g_cache[i].size);
      if (copy) { memcpy(copy, g_cache[i].data, g_cache[i].size); *out_size = g_cache[i].size; }
      cache_runlock();
      return copy;
    }
  }
  cache_runlock();

  FILE *f = NULL;
  if (fopen_s(&f, path, "rb") != 0) return NULL;
  char *data = malloc((size_t)st.st_size);
  if (!data) { fclose(f); return NULL; }
  size_t n = fread(data, 1, (size_t)st.st_size, f);
  fclose(f);

  cache_wlock();
  int slot = g_cache_n < CACHE_CAP ? g_cache_n++ : 0; /* evict slot 0 when full */
  for (int i = 0; i < g_cache_n; i++)
    if (strcmp(g_cache[i].path, path) == 0) { slot = i; break; }
  free(g_cache[slot].data);
  strncpy(g_cache[slot].path, path, sizeof(g_cache[slot].path) - 1);
  g_cache[slot].data  = data;
  g_cache[slot].size  = n;
  g_cache[slot].mtime = mtime;
  cache_wunlock();

  char *copy = malloc(n);
  if (copy) { memcpy(copy, data, n); *out_size = n; }
  return copy;
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

#define RESPOND(c, msg) do { send(c, msg, (int)strlen(msg), 0); closesocket(c); return; } while(0)

void handle_client(SOCKET client) {
#ifdef _WIN32
  DWORD tv = 2000;
#else
  struct timeval tv = {2, 0};
#endif
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

  char buffer[MAX_REQ] = {0};
  int received = recv(client, buffer, sizeof(buffer) - 1, 0);
  if (received <= 0) { closesocket(client); return; }
  buffer[received] = '\0';

  if (strncmp(buffer, "GET ", 4) != 0)
    RESPOND(client, "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\nMethod Not Allowed");

  char path[512] = {0};
  char *path_end = strchr(buffer + 4, ' ');
  if (!path_end) { closesocket(client); return; }
  size_t path_len = (size_t)(path_end - (buffer + 4));
  if (path_len >= sizeof(path)) path_len = sizeof(path) - 1;
  strncpy(path, buffer + 4, path_len);

  char *q = strchr(path, '?');
  if (q) *q = '\0';

  if (strstr(path, ".."))
    RESPOND(client, "HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\nForbidden");

  const char *local_path = (strcmp(path, "/") == 0) ? "index.html" : path + 1;

  size_t bytes_read = 0;
  char *data = cache_get(local_path, &bytes_read);
  if (!data)
    RESPOND(client, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNot Found");

  char header[256];
  snprintf(header, sizeof(header),
           "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
           mime_type(local_path), (unsigned long)bytes_read);
  send(client, header, (int)strlen(header), 0);
  size_t sent = 0;
  while (sent < bytes_read) {
    int n = send(client, data + sent, (int)(bytes_read - sent), 0);
    if (n <= 0) break;
    sent += (size_t)n;
  }
  free(data);
  closesocket(client);
}

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

static void start_pool(void) {
  queue_mutex_init();
  for (int i = 0; i < POOL_SIZE; i++) {
#ifdef _WIN32
    CloseHandle(CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)worker_thread, NULL, 0, NULL));
#else
    pthread_t t; pthread_create(&t, NULL, worker_thread, NULL); pthread_detach(t);
#endif
  }
}

static void enqueue_client(SOCKET s) {
  queue_lock();
  if (g_queue_len < QUEUE_CAP) {
    g_queue[g_queue_tail] = s;
    g_queue_tail = (g_queue_tail + 1) % QUEUE_CAP;
    g_queue_len++;
    queue_signal();
  } else {
    closesocket(s); /* queue full, reject */
  }
  queue_unlock();
}

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
  start_pool();

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

  SOCKET server = socket(AF_INET, SOCK_STREAM, 0);
  if (server == INVALID_SOCKET) return 1;

  int opt = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons((unsigned short)port);

  if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
    printf("Bind failed. Port %d maybe in use?\n", port);
    return 1;
  }

  if (listen(server, SOMAXCONN) == SOCKET_ERROR) return 1;

  printf("Serving files from: %s\n", serve_dir);
  printf("Tiny C Web Server listening on http://localhost:%d\n", port);
  printf("Press Ctrl+C to exit.\n");

  while (1) {
    SOCKET client = accept(server, NULL, NULL);
    if (client != INVALID_SOCKET)
      enqueue_client(client);
  }
}
