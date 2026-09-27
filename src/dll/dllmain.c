#include "dllmain.h"
#pragma comment(lib, "user32.lib")
#include "../../external/inih/ini.c"
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct {
  float fps;
  int UseCustomScreenDimensions;
  int ScreenWidth;
  int ScreenHeight;
  int EnableCursorClip;
  int CursorClipHotkey;
  int EnableLogging;
  int EnableBorderless;
  int FixSprintSlowdown;
} config;
config configFile;

static FILE *logFile = NULL;
static uintptr_t g_flipper_slot = 0;
static int g_flipper_logged = 0;
static volatile float *g_speed_cave = NULL;
static float g_speed_cap = 60.f;
static LONG g_retry_started = 0;



void containCursor(void *args) {
  while (TRUE) {
    if (GetAsyncKeyState(configFile.CursorClipHotkey)) {
      if (GetForegroundWindow() == args)
        SetCapture(args);
      ClipCursor(&final);
    }
  }
}

static int handler(void *Settings, const char *section, const char *name, const char *value) {
  config *modconfig = (config *)Settings;
  if (MATCH("Settings", "fps")) {
    modconfig->fps = (float)atof(value);
  } else if (MATCH("Settings", "UseCustomScreenDimensions")) {
    modconfig->UseCustomScreenDimensions = atoi(value);
  } else if (MATCH("Settings", "ScreenWidth")) {
    modconfig->ScreenWidth = atoi(value);
  } else if (MATCH("Settings", "ScreenHeight")) {
    modconfig->ScreenHeight = atoi(value);
  } else if (MATCH("Settings", "EnableCursorClip")) {
    modconfig->EnableCursorClip = atoi(value);
  } else if (MATCH("Settings", "CursorClipHotkey")) {
    modconfig->CursorClipHotkey = (int)strtol(value, NULL, 16);
  } else if (MATCH("Settings", "EnableLogging")) {
    modconfig->EnableLogging = atoi(value);
  } else if (MATCH("Settings", "EnableBorderless")) {
    modconfig->EnableBorderless = atoi(value);
  } else if (MATCH("Settings", "FixSprintSlowdown")) {
    modconfig->FixSprintSlowdown = atoi(value);
  } else {
    return 1;
  }
  return 1;
}

float readFile() {
  configFile.EnableLogging = 0;
  configFile.fps = 1000.f;
  configFile.EnableBorderless = 1;
  configFile.FixSprintSlowdown = 1;
  configFile.UseCustomScreenDimensions = 0;
  configFile.EnableCursorClip = 0;
  configFile.CursorClipHotkey = 0x7A;
  configFile.ScreenWidth = 1920;
  configFile.ScreenHeight = 1080;

  ini_parse("FPSconfig.ini", handler, &configFile);
  return configFile.fps;
}

void log_init() {
  if (!configFile.EnableLogging) return;
  logFile = fopen("DS3DebugFPS_log.txt", "w");
  if (logFile) {
    fprintf(logFile, "[INFO] DS3DebugFPS sprint-fix build\n");
    fflush(logFile);
  }
  if (logFile) fclose(logFile);
  logFile = fopen("DS3DebugFPS_log.txt", "a");
}

void log_close() {
  if (!configFile.EnableLogging) return;
  if (logFile) fclose(logFile);
  logFile = NULL;
}

void log_print(const char *fmt, ...) {
  if (!configFile.EnableLogging || !logFile) return;
  va_list args;
  va_start(args, fmt);
  vfprintf(logFile, fmt, args);
  fprintf(logFile, "\n");
  fflush(logFile);
  va_end(args);
}

typedef struct {
  BYTE *base;
  DWORD size;
} GameModule;

static int protect_is_readable(DWORD protect) {
  if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
  protect &= 0xFF;
  return protect == PAGE_READONLY || protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
         protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

static int region_covers(const void *addr, size_t nbytes, int writable) {
  const BYTE *p = (const BYTE *)addr;
  const BYTE *end = p + nbytes;
  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT || !protect_is_readable(mbi.Protect)) return 0;
    if (writable) {
      DWORD prot = mbi.Protect & 0xFF;
      if (prot != PAGE_READWRITE && prot != PAGE_WRITECOPY && prot != PAGE_EXECUTE_READWRITE &&
          prot != PAGE_EXECUTE_WRITECOPY)
        return 0;
    }
    BYTE *region_end = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
    if (region_end <= p) return 0;
    p = region_end;
  }
  return 1;
}

static int get_game_module(GameModule *m) {
  HMODULE module = GetModuleHandleA("darksoulsiii.exe");
  if (!module) module = GetModuleHandleA(NULL);
  if (!module) return 0;
  BYTE *base = (BYTE *)module;
  if (!region_covers(base, sizeof(IMAGE_DOS_HEADER), 0)) return 0;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return 0;
  IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
  if (!region_covers(nt, sizeof(IMAGE_NT_HEADERS64), 0)) return 0;
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
  m->base = base;
  m->size = nt->OptionalHeader.SizeOfImage;
  return m->size > 0x1000;
}

static BYTE *find_masked(BYTE *start, size_t size, const BYTE *pat, const char *mask) {
  size_t n = strlen(mask);
  if (size < n) return NULL;
  for (size_t i = 0; i + n <= size; ++i) {
    int ok = 1;
    for (size_t j = 0; j < n; ++j) {
      if (mask[j] == 'x' && start[i + j] != pat[j]) {
        ok = 0;
        break;
      }
    }
    if (ok) return start + i;
  }
  return NULL;
}

/* Scan only committed, readable pages so a guard gap (common under Wine) cannot fault the process. */
static BYTE *scan_module(const GameModule *m, const BYTE *pat, const char *mask, BYTE *after) {
  size_t n = strlen(mask);
  BYTE *end = m->base + m->size;
  BYTE *p = after ? after : m->base;
  if (p < m->base) p = m->base;
  while (p < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
    BYTE *region_end = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
    if (region_end <= p) break;
    BYTE *from = p;
    BYTE *to = region_end < end ? region_end : end;
    if ((mbi.State == MEM_COMMIT) && protect_is_readable(mbi.Protect) && (size_t)(to - from) >= n) {
      BYTE *search = from;
      size_t search_size = (size_t)(to - from);
      if (after && after > from && after < to) {
        search = after;
        search_size = (size_t)(to - after);
      }
      BYTE *hit = find_masked(search, search_size, pat, mask);
      if (hit) return hit;
    }
    p = to;
  }
  return NULL;
}

static int inside_module(const GameModule *m, uintptr_t addr) {
  return addr >= (uintptr_t)m->base && addr < (uintptr_t)m->base + m->size;
}

/* mov ecx, 0x368; call ... later mov [rip+disp], rax. Size 0x368 is the flipper object.
   Both vanilla lazy-inits use this and store the pointer with 48 89 05. */
static uintptr_t slot_from_ctor(BYTE *hit) {
  if (!region_covers(hit, 64, 0)) return 0;
  for (int i = 0; i <= 64 - 7; ++i) {
    if (hit[i] == 0x48 && hit[i + 1] == 0x89 && hit[i + 2] == 0x05) {
      int32_t disp;
      memcpy(&disp, hit + i + 3, 4);
      return (uintptr_t)(hit + i + 7 + disp);
    }
  }
  return 0;
}

static int object_ok(uintptr_t obj, const GameModule *m) {
  uintptr_t vtable;
  if (obj < 0x10000 || (obj & 7)) return 0;
  if (!region_covers((void *)obj, FLIPPER_OBJECT_SIZE, 1)) return 0;
  memcpy(&vtable, (void *)obj, sizeof vtable);
  return inside_module(m, vtable);
}

static int g_code_searched = 0;

static uintptr_t find_code_slot(const GameModule *m) {
  static const BYTE ctor_pat[] = {0xB9, 0x68, 0x03, 0x00, 0x00, 0xE8};
  uintptr_t slots[4];
  int nslots = 0;
  BYTE *cursor = NULL;

  for (;;) {
    uintptr_t slot;
    cursor = scan_module(m, ctor_pat, "xxxxxx", cursor);
    if (!cursor) break;
    slot = slot_from_ctor(cursor);
    if (slot && inside_module(m, slot)) {
      int seen = 0;
      for (int i = 0; i < nslots; ++i)
        if (slots[i] == slot) seen = 1;
      if (!seen && nslots < 4) slots[nslots++] = slot;
    }
    cursor += 1;
  }
  if (nslots == 0) return 0;
  if (nslots > 1) {
    for (int i = 1; i < nslots; ++i)
      if (slots[i] != slots[0]) {
        for (int j = 0; j < nslots; ++j) {
          uintptr_t obj = 0;
          if (region_covers((void *)slots[j], sizeof obj, 0)) memcpy(&obj, (void *)slots[j], sizeof obj);
          if (object_ok(obj, m)) return slots[j];
        }
        break;
      }
  }
  return slots[0];
}

/* June 2025 matched a stored pointer ending in 0x6050 followed by a null qword, but only
   if the address was in the 0x7FF3/0x7FF4 range. Proton does not put the heap there, so
   that scan missed the unmodified game. Keep the low-16-bit shape, drop the Windows-only
   high bytes, and require the object's vtable to land in this module. */
static uintptr_t find_value_slot(const GameModule *m) {
  static const BYTE ptr_pat[] = {0x50, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  BYTE *cursor = NULL;
  while ((cursor = scan_module(m, ptr_pat, "xx????xxxxxxxxxx", cursor)) != NULL) {
    uintptr_t obj = 0;
    memcpy(&obj, cursor, sizeof obj);
    if (object_ok(obj, m)) return (uintptr_t)cursor;
    cursor += 1;
  }
  return 0;
}

static int write_fps(uintptr_t slot, float fps, const GameModule *game, const char *how) {
  uintptr_t obj = 0;
  if (!slot || !region_covers((void *)slot, sizeof obj, 0)) return 0;
  memcpy(&obj, (void *)slot, sizeof obj);
  if (!object_ok(obj, game)) return 0;
  if (!g_flipper_logged) {
    log_print("[FPS] Flipper slot via %s at 0x%llX (module %p).", how, (unsigned long long)slot, game->base);
    g_flipper_logged = 1;
  }
  *(float *)(obj + FLIPPER_FPS_OFFSET) = fps;
  *(char *)(obj + FLIPPER_USE_OFFSET) = 1;
  log_print("[FPS] Cap set to %g at object 0x%llX.", fps, (unsigned long long)obj);
  return 1;
}

static int apply_fps_value(float fps) {
  GameModule game;
  uintptr_t vanilla;
  uintptr_t value_slot;

  if (!get_game_module(&game)) {
    log_print("[FPS] darksoulsiii.exe is not loaded yet.");
    return 0;
  }
  if (!g_code_searched) {
    g_flipper_slot = find_code_slot(&game);
    g_code_searched = 1;
    if (g_flipper_slot)
      log_print("[FPS] Code pattern slot 0x%llX.", (unsigned long long)g_flipper_slot);
    else
      log_print("[FPS] Constructor pattern was not found.");
  }
  if (g_flipper_slot) return write_fps(g_flipper_slot, fps, &game, "code pattern");

  value_slot = find_value_slot(&game);
  if (write_fps(value_slot, fps, &game, "pointer value")) {
    g_flipper_slot = value_slot;
    return 1;
  }

  vanilla = (uintptr_t)game.base + FLIPPER_SLOT_RVA;
  if (inside_module(&game, vanilla) && write_fps(vanilla, fps, &game, "vanilla rva")) {
    g_flipper_slot = vanilla;
    return 1;
  }
  return 0;
}

static void *alloc_near(void *site, const GameModule *game) {
  SYSTEM_INFO info;
  uintptr_t gran;
  uintptr_t start;
  GetSystemInfo(&info);
  gran = info.dwAllocationGranularity ? info.dwAllocationGranularity : 0x10000;
  start = ((uintptr_t)game->base + game->size + gran - 1) & ~(gran - 1);
  for (int i = 0; i < 256; ++i) {
    uintptr_t hint = start + (uintptr_t)i * gran;
    intptr_t delta = (intptr_t)hint - (intptr_t)site;
    void *page;
    if (delta > 0x70000000 || delta < -0x70000000) break;
    page = VirtualAlloc((void *)hint, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (page) return page;
  }
  for (int i = 1; i < 256; ++i) {
    uintptr_t base = (uintptr_t)game->base & ~(gran - 1);
    uintptr_t hint;
    intptr_t delta;
    void *page;
    if (base < (uintptr_t)i * gran + 0x10000) break;
    hint = base - (uintptr_t)i * gran;
    delta = (intptr_t)hint - (intptr_t)site;
    if (delta > 0x70000000 || delta < -0x70000000) break;
    page = VirtualAlloc((void *)hint, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (page) return page;
  }
  return NULL;
}

static int write_disp32(BYTE *disp_addr, BYTE *next_ip, void *target) {
  intptr_t rel = (BYTE *)target - next_ip;
  int32_t disp;
  DWORD old = 0;
  if (rel > 2147483647LL || rel < -2147483647LL - 1) return 0;
  disp = (int32_t)rel;
  if (!VirtualProtect(disp_addr, 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
  memcpy(disp_addr, &disp, 4);
  VirtualProtect(disp_addr, 4, old, &old);
  FlushInstructionCache(GetCurrentProcess(), disp_addr, 4);
  return 1;
}

/* The stuck test is length(position delta this frame) * 30 < 1. 30 is half of 60,
   so the same test at any rate is length * (0.5 / frame_time). The flipper keeps a
   smoothed frame time at +0x26C, which already follows spikes instead of the cap.
   A gentle graze sits just under the line; 20% of slack above 90 FPS lets that
   through and still catches a real stop. Recovery is quicker than the game's *1.2
   so leaving the wall does not play a visible slow sprint on a high refresh rate. */
#define SPEED_FRAME_DT_OFFSET 0x26C
#define SPEED_LENIENCY 1.2f
#define SPEED_RECOVER_BASE 1.5f

static void write_speed_factors(float dt) {
  float leniency = (g_speed_cap >= 90.f) ? SPEED_LENIENCY : 1.f;
  float recover_base = (g_speed_cap >= 90.f) ? SPEED_RECOVER_BASE : 1.2f;
  if (dt < 1.f / 480.f) dt = 1.f / 480.f;
  if (dt > 0.05f) dt = 1.f / g_speed_cap;
  g_speed_cave[0] = (0.5f / dt) * leniency;
  g_speed_cave[1] = expf(logf(0.8f) * dt * 60.f);
  g_speed_cave[2] = expf(logf(recover_base) * dt * 60.f);
}

void UpdateSpeedFactors(void) {
  static uintptr_t flipper_obj = 0;
  static int logged = 0;
  float dt;
  if (!g_speed_cave) return;
  dt = 1.f / g_speed_cap;
  if (!flipper_obj && g_flipper_slot && region_covers((void *)g_flipper_slot, sizeof(uintptr_t), 0)) {
    uintptr_t obj = *(uintptr_t *)g_flipper_slot;
    if (obj > 0x10000 && (obj & 7) == 0 && region_covers((void *)(obj + SPEED_FRAME_DT_OFFSET), sizeof(float), 0))
      flipper_obj = obj;
  }
  if (flipper_obj) {
    float measured = *(volatile float *)(flipper_obj + SPEED_FRAME_DT_OFFSET);
    if (measured >= 1.f / 480.f && measured <= 0.05f) dt = measured;
  }
  write_speed_factors(dt);
  if (!logged && dt < 0.02f) {
    log_print("[SPEED] Using frame time %g ms, distance scale %g.", dt * 1000.f, g_speed_cave[0]);
    logged = 1;
  }
}

static int install_speed_hook(BYTE *hit, BYTE *page) {
  static const BYTE prologue[] = {0x4C, 0x8B, 0xDC, 0x57, 0x48, 0x83, 0xEC, 0x70};
  static const BYTE head[] = {0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xEC, 0x28, 0x48, 0xB8};
  static const BYTE mid[] = {0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,
                              0x4C, 0x8B, 0xDC, 0x57, 0x48, 0x83, 0xEC, 0x70, 0xE9};
  BYTE *fn = NULL;
  BYTE *stub;
  BYTE *cursor;
  BYTE patch[8];
  int32_t rel;
  DWORD old = 0;
  for (int back = 8; back <= 0x180; ++back) {
    BYTE *candidate = hit - back;
    if (memcmp(candidate, prologue, sizeof prologue) == 0) {
      fn = candidate;
      break;
    }
  }
  if (!fn) {
    log_print("[SPEED] Movement function prologue was not found. Using the FPS cap only.");
    return 0;
  }
  stub = page + 0x40;
  cursor = stub;
  memcpy(cursor, head, sizeof head);
  cursor += sizeof head;
  *(uintptr_t *)cursor = (uintptr_t)UpdateSpeedFactors;
  cursor += sizeof(uintptr_t);
  memcpy(cursor, mid, sizeof mid);
  cursor += sizeof mid;
  *(int32_t *)cursor = (int32_t)((fn + 8) - (cursor + 4));
  rel = (int32_t)(stub - (fn + 5));
  patch[0] = 0xE9;
  memcpy(patch + 1, &rel, 4);
  patch[5] = patch[6] = patch[7] = 0x90;
  if (!VirtualProtect(fn, sizeof patch, PAGE_EXECUTE_READWRITE, &old)) return 0;
  memcpy(fn, patch, sizeof patch);
  VirtualProtect(fn, sizeof patch, old, &old);
  FlushInstructionCache(GetCurrentProcess(), fn, sizeof patch);
  FlushInstructionCache(GetCurrentProcess(), stub, 0x40);
  log_print("[SPEED] Frame-time hook installed.");
  return 1;
}

static int apply_speed_fix(float fps, const GameModule *game) {
  static const BYTE pat[] = {
      0xF3, 0x0F, 0x58, 0x00, 0x0F, 0xC6, 0x00, 0x00, 0x0F, 0x51, 0x00, 0xF3, 0x0F, 0x59, 0x05,
      0x00, 0x00, 0x00, 0x00, 0x0F, 0x2F};
  static const char mask[] = "xxx?xx?xxx?xxxx????xx";
  BYTE *hit;
  void *page;
  volatile float *cave;

  if (!(fps > 0.f)) {
    log_print("[SPEED] FPS cap is not positive, sprint fix skipped.");
    return 0;
  }
  if (fps < 1.f) fps = 1.f;
  g_speed_cap = fps;

  if (!g_speed_cave) {
    hit = scan_module(game, pat, mask, NULL);
    if (!hit || hit[14] != 0x05) {
      log_print("[SPEED] Running-speed check was not found. Sprint slowdown is unchanged.");
      return 0;
    }
    page = alloc_near(hit, game);
    if (!page) {
      log_print("[SPEED] Could not allocate a constant near the game code.");
      return 0;
    }
    cave = (volatile float *)page;
    g_speed_cave = cave;
    write_speed_factors(1.f / g_speed_cap);
    if (!write_disp32(hit + 15, hit + 19, (void *)cave)) {
      g_speed_cave = NULL;
      VirtualFree(page, 0, MEM_RELEASE);
      log_print("[SPEED] Failed to retarget the distance multiply.");
      return 0;
    }
    if (!(region_covers(hit + 0x29, 8, 0) && hit[0x29] == 0xF3 && hit[0x2A] == 0x0F && hit[0x2B] == 0x59 &&
          hit[0x2C] == 0x05 && region_covers(hit + 0x54, 8, 0) && hit[0x54] == 0xF3 && hit[0x55] == 0x0F &&
          hit[0x56] == 0x59 && hit[0x57] == 0x05 &&
          write_disp32(hit + 0x29 + 4, hit + 0x29 + 8, (void *)(cave + 1)) &&
          write_disp32(hit + 0x54 + 4, hit + 0x54 + 8, (void *)(cave + 2)))) {
      log_print("[SPEED] Distance scale is live. Decay rates were left alone.");
    }
    install_speed_hook(hit, (BYTE *)page);
    log_print("[SPEED] Cap %g, slack %g, recovery base %g.", g_speed_cap, g_speed_cap >= 90.f ? SPEED_LENIENCY : 1.f,
              g_speed_cap >= 90.f ? SPEED_RECOVER_BASE : 1.2f);
    return 1;
  }

  write_speed_factors(1.f / g_speed_cap);
  return 1;
}

static DWORD WINAPI retry_fps(LPVOID unused) {
  (void)unused;
  for (int i = 0; i < 40; ++i) {
    Sleep(250);
    if (apply_fps_value(configFile.fps)) return 0;
  }
  log_print("[FPS] Gave up waiting for the flipper object.");
  return 0;
}

void setFps(float rFPS) {
  HWND window = FindWindowA(NULL, "DARK SOULS III");
  GameModule game;

  log_print("[INFO] setFps %g, window %p", rFPS, window);
  if (configFile.EnableBorderless && window) {
    log_print("[INFO] Applying borderless window mode");
    if (configFile.UseCustomScreenDimensions != 1) {
      final.right = GetSystemMetrics(SM_CXSCREEN);
      final.bottom = GetSystemMetrics(SM_CYSCREEN);
    } else {
      final.right = configFile.ScreenWidth;
      final.bottom = configFile.ScreenHeight;
    }
    final.left = 0;
    final.top = 0;
    SetWindowLong(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    AdjustWindowRect(&final, GetWindowLong(window, GWL_STYLE), FALSE);
    SetWindowLong(window, GWL_EXSTYLE, (GetWindowLong(window, GWL_EXSTYLE) | WS_EX_TOPMOST));
    MoveWindow(window, final.left, final.top, final.right - final.left, final.bottom - final.top, TRUE);
    log_print("[INFO] Borderless window mode applied successfully");
  }

  if (!get_game_module(&game)) {
    log_print("[INFO] Game module is not available.");
    return;
  }
  log_print("[INFO] Module base %p size 0x%lX", game.base, game.size);

  if (configFile.FixSprintSlowdown)
    apply_speed_fix(rFPS, &game);
  else
    log_print("[SPEED] FixSprintSlowdown is off.");

  if (!apply_fps_value(rFPS) && InterlockedCompareExchange(&g_retry_started, 1, 0) == 0) {
    HANDLE thread = CreateThread(NULL, 0, retry_fps, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    log_print("[FPS] Object not ready, retrying in the background.");
  }

  if (configFile.EnableCursorClip != 0 && window) {
    CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)containCursor, window, 0, NULL);
    log_print("[INFO] Cursor clip thread created.");
  }
}

HINSTANCE BaseAddress, BaseAddressGenuine;
wchar_t *BaseFileName, FullFilePath[512];

BOOL WINAPI DllMain(HINSTANCE baseaddr, DWORD reason, LPVOID reserved) {
  (void)reserved;
  switch (reason) {
  case DLL_PROCESS_ATTACH:
    BaseFileName = FullFilePath + GetModuleFileNameW(baseaddr, FullFilePath, _countof(FullFilePath));
    while (BaseFileName-- > FullFilePath)
      if (*BaseFileName == L'\\')
        break;
    readFile();
    log_init();
    break;
  case DLL_PROCESS_DETACH:
    log_close();
    break;
  default:
    break;
  }
  return TRUE;
}

importD3D(FARPROC D3DAssemble_, DebugSetMute_, D3DCompile_, D3DCompressShaders_, D3DCreateBlob_, D3DDecompressShaders_,
          D3DDisassemble_, D3DDisassemble10Effect_, D3DGetBlobPart_, D3DGetDebugInfo_, D3DGetInputAndOutputSignatureBlob_,
          D3DGetInputSignatureBlob_, D3DGetOutputSignatureBlob_, D3DPreprocess_, D3DReflect_, D3DReturnFailure1_,
          D3DStripShader_)

void LoadGenuineDll(void) {
  if (!BaseAddressGenuine) {
    static wchar_t filename[512];
    log_print("[INFO] Loading system D3DCompiler_43");
    GetSystemDirectoryW(filename, _countof(filename));
    BaseAddressGenuine = LoadLibraryW(wcscat(filename, BaseFileName));
    IMPORT(D3DAssemble);
    IMPORT(DebugSetMute);
    IMPORT(D3DCompile);
    IMPORT(D3DCompressShaders);
    IMPORT(D3DCreateBlob);
    IMPORT(D3DDecompressShaders);
    IMPORT(D3DDisassemble);
    IMPORT(D3DDisassemble10Effect);
    IMPORT(D3DGetBlobPart);
    IMPORT(D3DGetDebugInfo);
    IMPORT(D3DGetInputAndOutputSignatureBlob);
    IMPORT(D3DGetInputSignatureBlob);
    IMPORT(D3DGetOutputSignatureBlob);
    IMPORT(D3DPreprocess);
    IMPORT(D3DReflect);
    IMPORT(D3DReturnFailure1);
    IMPORT(D3DStripShader);
    setFps(configFile.fps);
  }
}

#if !defined(_MSC_VER)
int atexit(void (*func)(void)) {
  (void)func;
  return 0;
}
#endif
