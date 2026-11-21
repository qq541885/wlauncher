/* main.c -- Wine 程序启动器（Switch homebrew，中文图形界面）
 *
 * 作用：在 Horizon 上给 Autorun(wine-nx) 提供一个中文图形前端。
 *   - 扫描 sdmc:/switch/wine2/drive_c/Programs 下的 *.exe / *.bat
 *   - 方向键选择，A 启动，B 退出，X 刷新
 *   - 启动方式是写 target.txt / args.txt，然后 envSetNextLoad 跳到
 *     wine-nx-runtime.nro（hbmenu 会接管这次链式加载）
 *
 * 中文渲染用 Switch 系统自带的 SharedFont（pl 服务），
 * 简体中文字形由系统提供，不往 nro 里塞字体文件。
 *
 * MIT license.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#define WINE_ROOT   "sdmc:/switch/wine2"
#define PROG_DIR    WINE_ROOT "/drive_c/Programs"
#define TARGET_TXT  WINE_ROOT "/target.txt"
#define ARGS_TXT    WINE_ROOT "/args.txt"
#define RUNTIME_NRO WINE_ROOT "/wine-nx-runtime.nro"

#define MAX_ITEMS   256
#define NAME_LEN    256

typedef struct {
  char name[NAME_LEN];   /* 显示用文件名 */
  char path[512];        /* 传给 wine 的 Windows 路径 */
  int  is_bat;
} Entry;

static Entry g_items[MAX_ITEMS];
static int   g_count = 0;
static int   g_sel   = 0;
static int   g_top   = 0;     /* 列表滚动起点 */

/* ------------------------------------------------------------------ */
/* 中文字体：从 Switch 系统 SharedFont 取                               */
/* ------------------------------------------------------------------ */

static void *g_font_data = NULL;
static size_t g_font_size = 0;

static int load_system_font(void) {
  /* 标准字体里已含简体中文字形；再显式请求一次中文集作为保险 */
  const u32 types = PlSharedFontType_Standard | PlSharedFontType_ChineseSimplified;

  if (R_FAILED(plInitialize(types))) {
    return -1;
  }

  PlFontData fd;
  if (R_FAILED(plGetSharedFontByType(&fd, PlSharedFontType_ChineseSimplified))) {
    if (R_FAILED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) {
      return -1;
    }
  }
  g_font_data = fd.address;
  g_font_size = fd.size;
  return 0;
}

/* ------------------------------------------------------------------ */
/* 扫描程序目录                                                         */
/* ------------------------------------------------------------------ */

static int is_exe_name(const char *n) {
  size_t l = strlen(n);
  if (l < 5) return 0;
  const char *e = n + l - 4;
  return (!strcasecmp(e, ".exe") || !strcasecmp(e, ".bat"));
}

static void scan_programs(void) {
  g_count = 0;
  g_sel = 0;
  g_top = 0;

  DIR *d = opendir(PROG_DIR);
  if (!d) return;

  struct dirent *de;
  while ((de = readdir(d)) != NULL && g_count < MAX_ITEMS) {
    if (de->d_name[0] == '.') continue;
    if (!is_exe_name(de->d_name)) continue;

    Entry *it = &g_items[g_count];
    snprintf(it->name, sizeof(it->name), "%s", de->d_name);
    /* 文件在 drive_c\Programs 下，对应 Windows 路径 C:\Programs\... */
    snprintf(it->path, sizeof(it->path), "C:\\Programs\\%s", de->d_name);
    it->is_bat = (strcasecmp(de->d_name + strlen(de->d_name) - 4, ".bat") == 0);
    g_count++;
  }
  closedir(d);

  /* 字母序 */
  for (int i = 0; i < g_count; i++)
    for (int j = i + 1; j < g_count; j++)
      if (strcasecmp(g_items[i].name, g_items[j].name) > 0) {
        Entry t = g_items[i]; g_items[i] = g_items[j]; g_items[j] = t;
      }
}

/* ------------------------------------------------------------------ */
/* 写 target.txt / args.txt                                             */
/* ------------------------------------------------------------------ */

static int write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  fwrite(text, 1, strlen(text), f);
  fclose(f);
  return 0;
}

static int prepare_launch(const Entry *e) {
  char args[512];

  if (e->is_bat) {
    /* bat 交给 cmd.exe；cmd 在 syswow64 里，wine 会按需加载 */
    if (write_file(TARGET_TXT, "C:\\windows\\syswow64\\cmd.exe") < 0) return -1;
    snprintf(args, sizeof(args), "/c %s", e->path);
    return write_file(ARGS_TXT, args);
  }

  if (write_file(TARGET_TXT, e->path) < 0) return -1;
  return write_file(ARGS_TXT, "");
}

/* 跳到 wine-nx-runtime.nro，由 hbmenu 完成链式加载 */
static void jump_to_runtime(void) {
  envSetNextLoad(RUNTIME_NRO, RUNTIME_NRO);
}

/* ------------------------------------------------------------------ */
/* 绘制                                                                */
/* ------------------------------------------------------------------ */

#define SCR_W 1280
#define SCR_H 720
#define PAD   40

static SDL_Window   *g_win  = NULL;
static SDL_Renderer *g_ren  = NULL;
static TTF_Font     *g_f24  = NULL;
static TTF_Font     *g_f20  = NULL;

static void draw_text(const char *s, int x, int y, SDL_Color c, TTF_Font *f) {
  if (!f || !s || !*s) return;
  SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, c);
  if (!surf) return;
  SDL_Texture *tex = SDL_CreateTextureFromSurface(g_ren, surf);
  if (tex) {
    SDL_Rect dst = { x, y, surf->w, surf->h };
    SDL_RenderCopy(g_ren, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
  }
  SDL_FreeSurface(surf);
}

static void draw_rect(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
  SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(g_ren, r, g, b, a);
  SDL_Rect rc = { x, y, w, h };
  SDL_RenderFillRect(g_ren, &rc);
}

static void render(void) {
  SDL_SetRenderDrawColor(g_ren, 18, 20, 26, 255);
  SDL_RenderClear(g_ren);

  SDL_Color white = { 235, 238, 245, 255 };
  SDL_Color grey  = { 150, 158, 172, 255 };
  SDL_Color accent= { 120, 190, 255, 255 };
  SDL_Color dim   = {  90,  96, 110, 255 };

  /* 标题栏 */
  draw_rect(0, 0, SCR_W, 84, 30, 34, 44, 255);
  draw_text("Wine 程序启动器", PAD, 24, white, g_f24);
  char sub[128];
  snprintf(sub, sizeof(sub), "共 %d 个程序", g_count);
  draw_text(sub, PAD + 300, 32, grey, g_f20);

  /* 列表 */
  const int row_h = 52;
  const int list_y = 104;
  const int max_rows = (SCR_H - list_y - 96) / row_h;

  if (g_count == 0) {
    draw_text("Programs 目录里没有 exe 或 bat", PAD, list_y + 20, grey, g_f24);
    snprintf(sub, sizeof(sub), "请把程序放进 %s", PROG_DIR);
    draw_text(sub, PAD, list_y + 70, dim, g_f20);
  } else {
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + max_rows) g_top = g_sel - max_rows + 1;
    if (g_top < 0) g_top = 0;

    for (int i = 0; i < max_rows && g_top + i < g_count; i++) {
      int idx = g_top + i;
      int y = list_y + i * row_h;
      int selected = (idx == g_sel);

      if (selected) {
        draw_rect(PAD - 12, y - 6, SCR_W - 2 * PAD + 24, row_h - 8, 60, 110, 175, 255);
      }
      draw_text(g_items[idx].name, PAD, y + 10, selected ? white : grey, g_f24);
      if (selected) {
        draw_text("← 按 A 启动", SCR_W - PAD - 190, y + 14, accent, g_f20);
      }
    }

    /* 滚动指示 */
    if (g_count > max_rows) {
      char pos[64];
      snprintf(pos, sizeof(pos), "%d / %d", g_sel + 1, g_count);
      draw_text(pos, SCR_W - PAD - 90, 32, grey, g_f20);
    }
  }

  /* 底部提示 */
  draw_rect(0, SCR_H - 72, SCR_W, 72, 26, 30, 38, 255);
  draw_text("方向键 选择     A 启动     X 刷新     B 退出", PAD, SCR_H - 50, grey, g_f20);

  SDL_RenderPresent(g_ren);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
  (void)argc; (void)argv;

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
    consoleInit(NULL);
    printf("SDL_Init 失败: %s\n按 + 退出\n", SDL_GetError());
    consoleUpdate(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad; padInitializeDefault(&pad);
    while (appletMainLoop()) {
      padUpdate(&pad);
      if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
      consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 1;
  }

  g_win = SDL_CreateWindow("Wine 程序启动器", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           SCR_W, SCR_H, SDL_WINDOW_SHOWN);
  g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!g_win || !g_ren) {
    SDL_Quit();
    return 1;
  }

  if (TTF_Init() != 0) {
    SDL_DestroyRenderer(g_ren);
    SDL_DestroyWindow(g_win);
    SDL_Quit();
    return 1;
  }

  if (load_system_font() == 0 && g_font_data && g_font_size) {
    SDL_RWops *rw = SDL_RWFromConstMem(g_font_data, (int)g_font_size);
    if (rw) {
      /* 24px 与 20px 两档；TTF 从系统字体内存直接加载 */
      g_f24 = TTF_OpenFontRW(rw, 1, 24);
      if (g_f24) g_f20 = TTF_OpenFontRW(SDL_RWFromConstMem(g_font_data, (int)g_font_size), 1, 20);
    }
  }

  scan_programs();

  int running = 1;
  while (running && appletMainLoop()) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) { running = 0; break; }
      if (ev.type == SDL_KEYDOWN) {
        switch (ev.key.keysym.sym) {
          case SDLK_UP:    if (g_sel > 0) g_sel--; break;
          case SDLK_DOWN:  if (g_sel + 1 < g_count) g_sel++; break;
          case SDLK_x:     scan_programs(); break;
          case SDLK_b:
          case SDLK_ESCAPE: running = 0; break;
          case SDLK_a:
            if (g_count > 0 && prepare_launch(&g_items[g_sel]) == 0) {
              jump_to_runtime();
              running = 0;
            }
            break;
          default: break;
        }
      }
      if (ev.type == SDL_JOYBUTTONDOWN) {
        switch (ev.jbutton.button) {
          case 0:  /* A */
            if (g_count > 0 && prepare_launch(&g_items[g_sel]) == 0) {
              jump_to_runtime();
              running = 0;
            }
            break;
          case 1:  /* B */
            running = 0;
            break;
          case 2:  /* X */
            scan_programs();
            break;
          default: break;
        }
      }
      if (ev.type == SDL_JOYHATMOTION) {
        if (ev.jhat.value & SDL_HAT_UP)   { if (g_sel > 0) g_sel--; }
        if (ev.jhat.value & SDL_HAT_DOWN) { if (g_sel + 1 < g_count) g_sel++; }
      }
    }
    render();
    SDL_Delay(16);
  }

  if (g_f24) TTF_CloseFont(g_f24);
  if (g_f20) TTF_CloseFont(g_f20);
  TTF_Quit();
  plExit();
  SDL_DestroyRenderer(g_ren);
  SDL_DestroyWindow(g_win);
  SDL_Quit();
  return 0;
}
