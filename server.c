#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PORT 80
#define MAX_REQ 2048
#define MAX_FILE_SIZE (10 * 1024 * 1024)

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <windows.h>
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#define os_chdir _chdir

void get_exe_dir(const char *argv0, char *dir, size_t size) {
  (void)argv0;
  GetModuleFileNameA(NULL, dir, (DWORD)size);
  char *sep = strrchr(dir, '\\');
  if (!sep) sep = strrchr(dir, '/');
  if (sep) *sep = '\0';
  else { dir[0] = '.'; dir[1] = '\0'; }
}

#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket(s) close(s)
#define os_chdir chdir

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
#endif

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

  FILE *f = NULL;
  if (fopen_s(&f, local_path, "rb") != 0)
    RESPOND(client, "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\nNot Found");

  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  rewind(f);

  if (file_size <= 0 || file_size > MAX_FILE_SIZE) {
    fclose(f);
    RESPOND(client, "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nFile too large");
  }

  char *data = malloc((size_t)file_size);
  if (!data) {
    fclose(f);
    RESPOND(client, "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\nOut of memory");
  }

  size_t bytes_read = fread(data, 1, (size_t)file_size, f);
  fclose(f);

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

#ifdef _WIN32
DWORD WINAPI thread_entry(LPVOID arg) { handle_client((SOCKET)(intptr_t)arg); return 0; }
void spawn_thread(SOCKET client) {
  HANDLE t = CreateThread(NULL, 0, thread_entry, (void *)(intptr_t)client, 0, NULL);
  if (t) CloseHandle(t);
  else closesocket(client);
}
#else
void *thread_entry(void *arg) { handle_client((SOCKET)(intptr_t)arg); return NULL; }
void spawn_thread(SOCKET client) {
  pthread_t t;
  if (pthread_create(&t, NULL, thread_entry, (void *)(intptr_t)client) == 0)
    pthread_detach(t);
  else
    closesocket(client);
}
#endif

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
      spawn_thread(client);
  }
}
