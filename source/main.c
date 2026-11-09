#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/socket.h>
#include <sys/fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
#define SERVER_HOST   "your.server.example.com"   // <-- set your VPS host or IP
#define SERVER_PORT   26662

#define MSG_BUF       512
#define MAX_LINES     120
#define MAX_TOP_LINES 40

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
static char screen_log[MAX_LINES][MSG_BUF];
static char top_log[MAX_TOP_LINES][MSG_BUF];
static int  log_tail  = 0;
static int  log_count = 0;
static int  top_tail  = 0;
static int  top_count = 0;

static char room_name[16] = "main";
static char my_name[17]   = "guest";
static long long last_ping = 0;

static PrintConsole top_screen;
static PrintConsole bot_screen;

// ---------------------------------------------------------------------------
// Ring buffer log helpers
// ---------------------------------------------------------------------------
static void push_line(PrintConsole *which, char store[MAX_LINES][MSG_BUF],
                      int *tail, int *count, int cap, const char *line) {
  (void)which;
  snprintf(store[*tail], MSG_BUF, "%s", line);
  *tail = (*tail + 1) % cap;
  if (*count < cap) (*count)++;
}

static void log_msg(const char *line) {
  push_line(&bot_screen, screen_log, &log_tail, &log_count, MAX_LINES, line);
}

static void log_top(const char *line) {
  push_line(&top_screen, top_log, &top_tail, &top_count, MAX_TOP_LINES, line);
}

static void redraw_log(void) {
  consoleSelect(&bot_screen);
  consoleClear();
  int start = (log_tail - log_count + MAX_LINES) % MAX_LINES;
  for (int i = 0; i < log_count; i++) {
    printf("%s\n", screen_log[(start + i) % MAX_LINES]);
  }
  consoleSelect(&top_screen);
  consoleClear();
  printf("== \031 MAX \030 on 3DS ==\n");
  printf("Room: %s   You: %s\n\n", room_name, my_name);
  int tstart = (top_tail - top_count + MAX_TOP_LINES) % MAX_TOP_LINES;
  for (int i = 0; i < top_count; i++) {
    printf("%s\n", top_log[(tstart + i) % MAX_TOP_LINES]);
  }
}

// ---------------------------------------------------------------------------
// Networking (single blocking socket + non-blocking recv loop)
// ---------------------------------------------------------------------------
static int sockfd = -1;

static int net_connect(void) {
  struct hostent *he = gethostbyname(SERVER_HOST);
  if (!he) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons(SERVER_PORT);
  memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) return -1;

  if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(sockfd);
    sockfd = -1;
    return -1;
  }

  // Non-blocking so the UI never freezes on incoming data
  int flags = fcntl(sockfd, F_GETFL, 0);
  fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
  return 0;
}

static void net_send(const char *fmt, const char *arg) {
  char buf[MSG_BUF];
  snprintf(buf, sizeof(buf), fmt, arg);
  if (sockfd >= 0) send(sockfd, buf, strlen(buf), 0);
}

static void net_poll(void) {
  if (sockfd < 0) return;
  char buf[511];
  int n = recv(sockfd, buf, sizeof(buf) - 1, 0);
  if (n <= 0) { // 0 = closed, -1 w/ EAGAIN = nothing to read
    if (n == 0) { log_top("! disconnected"); close(sockfd); sockfd = -1; }
    return;
  }
  buf[n] = 0;

  char *line = strtok(buf, "\r\n");
  while (line) {
    if (strncmp(line, "#HELLO", 6) == 0) {
      char nick[24], room[24];
      int r = sscanf(line, "#HELLO %23s %23s", nick, room);
      if (r >= 2) { snprintf(my_name, sizeof(my_name), "%s", nick);
                    snprintf(room_name, sizeof(room_name), "%s", room); }
      log_top(line);
    } else if (strncmp(line, "#JOIN ", 6) == 0) {
      log_top(line + 6);
      log_top(">>> joined");
    } else if (strncmp(line, "#PART ", 6) == 0) {
      log_top(line + 6);
    } else if (strncmp(line, "#WHO ", 5) == 0) {
      log_top(line + 5);
    } else if (strncmp(line, "#ERR ", 5) == 0) {
      log_top(line + 5);
    } else if (line[0] == 'M' && line[1] == ' ') {
      log_msg(line + 2);
    } else if (line[0] == '#' || line[0] != 0) {
      log_top(line);
    }
    line = strtok(NULL, "\r\n");
  }
}

// ---------------------------------------------------------------------------
// Text input via software keyboard
// ---------------------------------------------------------------------------
static void input_text(const char *hint, int max, char *out) {
  SwkbdState swkbd;
  SwkbdButton button;
  char16_t input[SWKBD_MAX_LENGTH + 1];
  swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, max);
  swkbdSetHintText(&swkbd, hint);
  button = swkbdInputText(&swkbd, input, SWKBD_MAX_LENGTH + 1);
  if (button != SWKBD_BUTTON_CONFIRM) { out[0] = 0; return; }
  for (int i = 0; i < max && input[i] != 0; i++) out[i] = (char)input[i];
  out[max] = 0;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(void) {
  gfxInitDefault();
  consoleInit(GFX_TOP, &top_screen);
  consoleInit(GFX_BOTTOM, &bot_screen);

  log_msg("MAX-3DS chat client");
  log_msg("B = connect, A = msg, X = room, Y = nick");
  log_msg("D-pad up/down = scroll will be added");
  redraw_log();

  // Init network stack
  static u32 __attribute__((aligned(0x1000))) soc_buffer[0x2000];
  Result r = socInit(soc_buffer, sizeof(soc_buffer));
  if (R_FAILED(r)) {
    log_msg("socInit failed!");
    while (aptMainLoop()) {
      hidScanInput();
      if (hidKeysDown() & KEY_START) break;
      gfxFlushBuffers();
      gfxSwapBuffers();
      gspWaitForVBlank();
    }
    gfxExit();
    return 1;
  }

  int connected = 0;
  u64 lastConnect = 0;

  while (aptMainLoop()) {
    hidScanInput();
    u32 kDown = hidKeysDown();
    u32 kHeld = hidKeysHeld();

    // Keep-alive every 5 s
    u64 now = svcGetSystemTick();
    if (now - last_ping > 5 * 268123480ULL) {
      last_ping = now;
      if (connected) net_send("PING\n", "");
    }

    // Connection dlg
    if (kDown & KEY_B) {
      if (!connected) {
        log_top("connecting to " SERVER_HOST "...");
        redraw_log();
        if (net_connect() == 0) {
          connected = 1;
          log_top("connected");
          char buf[32];
          snprintf(buf, sizeof(buf), "NICK %s\n", my_name);
          net_send("%s", buf);
          net_send("JOIN %s\n", room_name);
        } else {
          log_top("connect failed");
        }
      } else {
        close(sockfd); sockfd = -1;
        connected = 0;
        log_top("disconnected");
      }
      redraw_log();
    }

    if (kDown & KEY_A) {
      if (!connected) { log_top("connect first (B)"); redraw_log(); }
      else {
        char text[400];
        input_text("Message", 400, text);
        if (text[0]) {
          char line[MSG_BUF];
          snprintf(line, sizeof(line), "MSG %s\n", text);
          send(sockfd, line, strlen(line), 0);
        }
      }
    }

    if (kDown & KEY_X) {
      char nb[16];
      input_text("Room name", 15, nb);
      if (nb[0]) {
        snprintf(room_name, sizeof(room_name), "%s", nb);
        char line[32];
        snprintf(line, sizeof(line), "JOIN %s\n", nb);
        net_send("%s", line);
        log_top("switching room...");
      }
      redraw_log();
    }

    if (kDown & KEY_Y) {
      char nick[17];
      input_text("Nickname", 16, nick);
      if (nick[0]) {
        snprintf(my_name, sizeof(my_name), "%s", nick);
        char line[32];
        snprintf(line, sizeof(line), "NICK %s\n", nick);
        net_send("%s", line);
        log_top("nick set");
      }
      redraw_log();
    }

    // Process incoming
    net_poll();
    if (kDown & (KEY_L | KEY_R)) redraw_log();

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();

    if (kDown & KEY_START) break;
  }

  if (sockfd >= 0) close(sockfd);
  socExit();
  gfxExit();
  return 0;
}