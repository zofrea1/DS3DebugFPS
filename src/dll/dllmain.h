#ifndef _dllmain_h
#define _dllmain_h

#include <windows.h>

#define importD3D(...) __VA_ARGS__;
#define IMPORT(a) a##_ = GetProcAddress(BaseAddressGenuine, #a)
#define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0

/* Vanilla 1.15.2 slot of the SprjFlipper pointer. The object itself is heap
   allocated (0x368 bytes); this RVA is the global that points at it. */
#define FLIPPER_SLOT_RVA 0x489DD10
#define FLIPPER_OBJECT_SIZE 0x368
#define FLIPPER_FPS_OFFSET 0x354
#define FLIPPER_USE_OFFSET 0x358

RECT final;

void setFps(float rFPS);
float readFile();
void containCursor(void *args);
#endif
