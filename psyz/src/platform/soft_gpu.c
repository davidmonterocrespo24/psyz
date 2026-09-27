// PSY-Z software rasterizer backend.
//
// Everything between "==== core ====" markers is plain C over a u16 VRAM
// array - no SDL, no OS - and is the exact code intended to run on the
// ESP32-S3 port of SOTN. The thin shell at the bottom gives it a window on PC
// so the software pipeline can be A/B'd pixel-for-pixel against the sdl3_gpu
// backend before any hardware is involved (the PsyCross -> Driver2 method).
//
// VRAM model: one u16[512][1024] in native PSX RGB5551. The game already owns
// such an array (g_RawVram in the SOTN PC port) and writes it directly through
// MyStoreImage/MyClearImage, so this backend adopts it as THE video memory:
// no shadow copies, no format conversions, and what the game pokes is what
// gets displayed. A standalone psyz build without a game supplies its own
// (see soft_vram_fallback below).

#include <psyz.h>
#include <psyz/overlay.h>
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <libetc.h>
#include "../internal.h"
#include <SDL3/SDL.h>

#ifdef _MSC_VER
#define ALIGNAS(n) __declspec(align(n))
#else
#include <stdalign.h>
#define ALIGNAS(n) alignas(n)
#endif

#include "sdl3_common.h"
// 4x4 dither: pointless on an RGB565 LCD; common's SetDither is reused as-is.

#include "soft_raster.inc.c"


// ==================== PC shell: window + present ====================

static SDL_Renderer* soft_renderer;
static SDL_Texture* soft_texture; // VRAM_W x VRAM_H streaming, XBGR1555

bool InitPlatform() {
    if (sdl3_window) {
        return true;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        ERRORF("SDL_Init: %s", SDL_GetError());
        return false;
    }
    vram = SOFT_VRAM;
    if (!SDL_CreateWindowAndRenderer("SOTN (psyz soft)", 256 * 3, 240 * 3,
                                     SDL_WINDOW_RESIZABLE, &sdl3_window,
                                     &soft_renderer)) {
        ERRORF("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
        return false;
    }
    SDL_GetWindowSizeInPixels(
        sdl3_window, &wnd_size_in_pixels.w, &wnd_size_in_pixels.h);
    // PSX 5551 has red in the LOW bits; SDL's XBGR1555 matches exactly.
    soft_texture = SDL_CreateTexture(soft_renderer, SDL_PIXELFORMAT_XBGR1555,
                                     SDL_TEXTUREACCESS_STREAMING, VRAM_W,
                                     VRAM_H);
    if (!soft_texture) {
        ERRORF("SDL_CreateTexture: %s", SDL_GetError());
        return false;
    }
    SDL_SetTextureScaleMode(soft_texture, SDL_SCALEMODE_NEAREST);
    Sdl3Common_TimingInit();
    return true;
}

void Draw_Reset() {
    st.x0 = 0;
    st.y0 = 0;
    st.x1 = VRAM_W - 1;
    st.y1 = VRAM_H - 1;
    st.ox = st.oy = 0;
    if (!vram) {
        vram = SOFT_VRAM;
    }
}

static void PlatformBackend_Present(void) {
    if (!sdl3_window && !InitPlatform()) {
        return;
    }
    SDL_UpdateTexture(soft_texture, NULL, vram, VRAM_W * 2);

    SDL_SetRenderDrawColor(soft_renderer, 0, 0, 0, 255);
    SDL_RenderClear(soft_renderer);

    if (display_enabled) {
        SDL_FRect src = {(float)display_area_px.x, (float)display_area_px.y,
                         (float)display_size_px.x, (float)display_size_px.y};
        float aspect =
            GetCurrentGameAspectRatio(display_size_px.x, display_size_px.y);
        if (debug_show_vram) {
            src = (SDL_FRect){0, 0, VRAM_W, VRAM_H};
            aspect = (float)VRAM_W / (float)VRAM_H;
        }
        WndSize win;
        SDL_GetWindowSizeInPixels(sdl3_window, &win.w, &win.h);
        SDL_Rect dsti = FitGameToWindow(aspect, win);
        SDL_FRect dst = {(float)dsti.x, (float)dsti.y, (float)dsti.w,
                         (float)dsti.h};
        SDL_RenderTexture(soft_renderer, soft_texture, &src, &dst);
    }
    if (overlay_frame_cb) {
        overlay_frame_cb();
    }
    finish_time = SDL_GetPerformanceCounter();
    SDL_RenderPresent(soft_renderer);
}

// ---- platform lifecycle hooks the common header expects --------------------

static void PlatformBackend_SetDriverVsync(bool enable) {
    if (soft_renderer) {
        SDL_SetRenderVSync(soft_renderer, enable ? 1 : 0);
    }
}

static void QuitPlatform(void) {
    if (overlay_destroy_cb) {
        overlay_destroy_cb();
    }
    if (soft_texture) {
        SDL_DestroyTexture(soft_texture);
        soft_texture = NULL;
    }
    if (soft_renderer) {
        SDL_DestroyRenderer(soft_renderer);
        soft_renderer = NULL;
    }
    if (sdl3_window) {
        SDL_DestroyWindow(sdl3_window);
        sdl3_window = NULL;
        is_window_visible = false;
    }
    SDL_Quit();
    is_platform_initialized = false;
    is_platform_init_successful = false;
}

void ResetPlatform(void) {
    cur_tpage = 0;
    Draw_Reset();
}

int Psyz_VideoSetInternalResolution(unsigned multiplier) {
    // the software rasterizer renders at native PSX resolution by design
    if (multiplier > 1) {
        WARNF("internal resolution scaling not supported by the soft backend");
    }
    return 1;
}
