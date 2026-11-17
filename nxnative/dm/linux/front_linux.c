// Linux front end (dm/host/front.h) on SDL3: the counterpart of dm/android/front_android.c.
//   dmhost <data dir> [-- game arguments]
// <data dir> is the data pack porter makes for Android (mods/, RomFS/, shaders.dmsv, env.txt). Saves go to <data dir>/save, scratch files to <data dir>/temp,
// the pause menu's settings to <data dir>/settings.txt (the Android app's keys: rumble, docked, fps, labels).
// The main thread owns SDL: the window (its Vulkan surface goes to gfx_vk_present.c through dm_front_vk_surface) and the controllers, which feed player 1's pad.
// The host (dm/host/main.c, built as dm_host_main) runs on a thread of its own with a 16 MB stack, as on Android.
// Controls: Guide (Home) opens and closes the pause menu; Guide + Start quits. Keyboard: Esc opens and closes the menu, F10 quits.
// The pause menu and the software keyboard are gfx_vk_present.c's panels (overlay_layout.h), driven with the controller as on Android: the d-pad or the left stick
// moves, A does, B goes back; in the keyboard Y is shift, B erases, + accepts, - cancels, and a physical keyboard types.
// DM_HEADLESS=1: no window and no SDL, frames counted only (gfx_vk_present.c's headless path, DM_VK_SHOT).
#include "dm.h"
#include "front.h"
#include "dm_input.h"
#include "hle.h"
#include "overlay_layout.h"
#include "pixel_font.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <android/native_window.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int dm_host_main(int argc, char **argv);   // dm/host/main.c (main, renamed by dm/linux/build.py)
void dm_movie_hold(int on);                // hle_movie.c
void dm_overlay_redraw(void);              // gfx_vk_present.c: the last frame presented again (a panel changed while the game is held)
void dm_haptics_linux_start(int on); void dm_haptics_linux_set(int on); void dm_haptics_linux_pad(SDL_JoystickID id); void dm_haptics_linux_tap(void);   // haptics_linux.c
void dm_haptics_suspend(unsigned reason, int suspended);

static char g_root[PATH_MAX];

// ---- host front end interface ----
void front_run(void (*guest)(void)) { guest(); }   // already on the host thread
void *front_metal_layer(void) { return NULL; }
void front_screen_match(int fps) { (void)fps; }

// ---- the window ----
static SDL_Window *g_win;
static _Atomic int32_t g_win_w, g_win_h;
static _Atomic int g_max_hz;
int front_screen_max_fps(void) { return atomic_load(&g_max_hz); }
ANativeWindow *dm_front_window(void) { return (ANativeWindow *)g_win; }
int32_t ANativeWindow_getWidth(ANativeWindow *w) { (void)w; return atomic_load(&g_win_w); }
int32_t ANativeWindow_getHeight(ANativeWindow *w) { (void)w; return atomic_load(&g_win_h); }
uint32_t dm_front_vk_instance_exts(const char **out, uint32_t cap) {
    if (!g_win) return 0;
    Uint32 n = 0; const char *const *e = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!e) { LOG("[RED] SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError()); return 0; }
    if (n > cap) n = cap;
    for (Uint32 i = 0; i < n; i++) out[i] = e[i];
    return n;
}
int dm_front_vk_surface(VkInstance inst, VkSurfaceKHR *out) {
    if (!g_win) return 0;
    if (!SDL_Vulkan_CreateSurface(g_win, inst, NULL, out)) { LOG("[RED] SDL_Vulkan_CreateSurface: %s", SDL_GetError()); return 0; }
    return 1;
}
static void win_size(void) { int w = 0, h = 0; SDL_GetWindowSizeInPixels(g_win, &w, &h); atomic_store(&g_win_w, w); atomic_store(&g_win_h, h); }

// ---- settings (the pause menu's), settings.txt as the Android app keeps them ----
static _Atomic int g_set_rumble = 1, g_set_fps, g_set_labels = -1;   // labels: a controller's face buttons by label (1) or position (0); -1: as env.txt says
static int pad_labels(void) {
    int v = atomic_load(&g_set_labels); if (v >= 0) return v;
    const char *e = getenv("DM_PAD_LABEL"); if (!e) e = getenv("DM_PAD_XBOX"); return e && atoi(e);
}
static void settings_load(void) {
    char p[PATH_MAX + 16]; snprintf(p, sizeof p, "%s/settings.txt", g_root); FILE *f = fopen(p, "r"); char line[128];
    if (!f) return;
    while (fgets(line, sizeof line, f)) { int v;
        if (sscanf(line, "rumble=%d", &v) == 1) atomic_store(&g_set_rumble, v != 0);
        else if (sscanf(line, "fps=%d", &v) == 1) atomic_store(&g_set_fps, v != 0);
        else if (sscanf(line, "docked=%d", &v) == 1) setenv("DM_OPMODE", v ? "1" : "0", 1);   // over env.txt's: the host reads it once, at its first query
        else if (sscanf(line, "labels=%d", &v) == 1) atomic_store(&g_set_labels, v != 0); }
    fclose(f);
}
static void settings_save(void) {
    char p[PATH_MAX + 16], tmp[PATH_MAX + 32]; snprintf(p, sizeof p, "%s/settings.txt", g_root); snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "w"); if (!f) { LOG("[RED] cannot write %s", tmp); return; }
    fprintf(f, "rumble=%d\ndocked=%d\nfps=%d\n", atomic_load(&g_set_rumble), dm_opmode() != 0, atomic_load(&g_set_fps));
    if (atomic_load(&g_set_labels) >= 0) fprintf(f, "labels=%d\n", atomic_load(&g_set_labels));
    fclose(f); rename(tmp, p);
}

// ---- holding the game while the pause menu or the keyboard is open: its clock reads wait, its audio goes to the silent sink, movies stop ----
static _Atomic int g_menu, g_tx_on;
static pthread_mutex_t g_hold_mu = PTHREAD_MUTEX_INITIALIZER; static int g_held;
static void hold_sync(void) {
    pthread_mutex_lock(&g_hold_mu);
    int want = atomic_load(&g_menu) || atomic_load(&g_tx_on);
    if (want != g_held) { g_held = want; if (want) { dm_movie_hold(1); dm_audio_suspend(1); guest_clock_freeze_for(1, 1); } else { guest_clock_freeze_for(1, 0); dm_audio_suspend(0); dm_movie_hold(0); } }
    pthread_mutex_unlock(&g_hold_mu);
}

// ---- the pause menu (overlay_layout.h OVM_*, drawn by gfx_vk_present.c ov_menu_panel) ----
// Linux has no touch gamepad and no save manager yet: their rows are left out
int dm_menu_item_available(int item) { return item == OVM_RUMBLE || item == OVM_PAD || item == OVM_FPS || item == OVM_MODE || item == OVM_TAB || item == OVM_RESUME; }
static _Atomic int g_menu_hi = -1, g_menu_tab, g_tx_ver;
int dm_menu_state(void) {
    if (atomic_load(&g_tx_on)) return 1 << 16 | (atomic_load(&g_tx_ver) & 0xffff);   // the keyboard's panel (gfx_vk_present.c ov_text_panel)
    if (!atomic_load(&g_menu)) return -1;
    return atomic_load(&g_set_rumble) << 1 | (dm_opmode() != 0) << 2 | atomic_load(&g_set_fps) << 3 | (atomic_load(&g_menu_hi) + 1) << 4 | atomic_load(&g_menu_tab) << 8 | 60 << 9 | pad_labels() << 28;
}
int dm_overlay_fps(void) { return atomic_load(&g_set_fps) && !atomic_load(&g_menu); }
static int first_row(int tab) { for (int r = 1; r < OVM_ROWS; r++) if (ov_menu_item(tab, r) >= 0) return r; return OVM_ROWS - 1; }
static void menu_open(void) {
    if (atomic_exchange(&g_menu, 1)) return;
    atomic_store(&g_menu_hi, first_row(atomic_load(&g_menu_tab)));
    dm_pad_set(0, 0, 0, 0, 0);   // nothing stays pressed in the game
    dm_haptics_suspend(4, 1); hold_sync(); dm_overlay_redraw();
    LOG("[menu] opened: the game is paused");
}
static void menu_close(void) {
    if (!atomic_exchange(&g_menu, 0)) return;
    hold_sync(); dm_haptics_suspend(4, 0); dm_overlay_redraw();
    LOG("[menu] closed: the game goes on (rumble %s, face buttons by %s, frame rate %s, %s)", atomic_load(&g_set_rumble) ? "on" : "off", pad_labels() ? "label" : "position",
        atomic_load(&g_set_fps) ? "shown" : "hidden", dm_opmode() ? "docked" : "handheld");
}
static void menu_tab(int t) { t = (t % OVM_TABS + OVM_TABS) % OVM_TABS; if (atomic_exchange(&g_menu_tab, t) != t) dm_haptics_linux_tap(); dm_overlay_redraw(); }
static void menu_do(int item) {
    if (item == OVM_RUMBLE) { int on = !atomic_load(&g_set_rumble); atomic_store(&g_set_rumble, on); dm_haptics_linux_set(on); settings_save(); }
    else if (item == OVM_FPS) { atomic_store(&g_set_fps, !atomic_load(&g_set_fps)); settings_save(); }
    else if (item == OVM_MODE) { dm_set_opmode(dm_opmode() ? 0 : 1); settings_save(); }   // the game takes it when it goes on
    else if (item == OVM_PAD) { atomic_store(&g_set_labels, !pad_labels()); settings_save(); }
    else if (item == OVM_TAB) { menu_tab(atomic_load(&g_menu_tab) + 1); return; }
    if (item >= 0 && item < OVM_N) dm_haptics_linux_tap();
    if (item == OVM_RESUME) menu_close(); else dm_overlay_redraw();
}
static void menu_key(uint64_t bit) {   // up and down go through the rows, left and right (or L and R) change the tab, A does the row, B and + close
    int hi = atomic_load(&g_menu_hi), tab = atomic_load(&g_menu_tab), item = hi >= 0 ? ov_menu_item(tab, hi) : -1;
    if (bit == DM_BTN_UP || bit == DM_BTN_DOWN) {
        for (int k = 0; k < OVM_ROWS; k++) { hi = hi < 0 ? (bit == DM_BTN_UP ? OVM_ROWS - 1 : 1) : (hi + (bit == DM_BTN_UP ? OVM_ROWS - 1 : 1)) % OVM_ROWS; if (ov_menu_item(tab, hi) >= 0) break; }
        atomic_store(&g_menu_hi, hi); dm_overlay_redraw();
    } else if (((bit == DM_BTN_LEFT || bit == DM_BTN_RIGHT) && item == OVM_TAB) || bit == DM_BTN_L || bit == DM_BTN_ZL || bit == DM_BTN_R || bit == DM_BTN_ZR) {
        menu_tab(tab + (bit == DM_BTN_LEFT || bit == DM_BTN_L || bit == DM_BTN_ZL ? -1 : 1));
        if (hi > 0 && ov_menu_item(atomic_load(&g_menu_tab), hi) < 0) atomic_store(&g_menu_hi, first_row(atomic_load(&g_menu_tab)));
    }
    else if (bit == DM_BTN_A) menu_do(hi < 0 ? OVM_RESUME : item);
    else if (bit == DM_BTN_B || bit == DM_BTN_PLUS) menu_close();
}

// ---- the software keyboard: the game's text entry (nn::swkbd, hle_swkbd.c → front_text_input), as front_android.c's controller keyboard ----
static pthread_mutex_t g_tx_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_tx_cv = PTHREAD_COND_INITIALIZER;
static struct { char title[128], text[256]; int n, cap, max, done, ok, sel, shift; } g_tx;
int dm_text_entry(char *title, int tcap, char *text, int xcap, int *sel, int *shift) {
    if (!atomic_load(&g_tx_on)) return 0;
    pthread_mutex_lock(&g_tx_mu);
    snprintf(title, (size_t)tcap, "%s", g_tx.title); snprintf(text, (size_t)xcap, "%s", g_tx.text); *sel = g_tx.sel; *shift = g_tx.shift;
    pthread_mutex_unlock(&g_tx_mu);
    return 1;
}
static void tx_changed(void) { atomic_fetch_add(&g_tx_ver, 1); dm_overlay_redraw(); }
static void tx_add(int c) {   // the caller holds g_tx_mu; a capital ends the shift, as on a phone
    if (!c || g_tx.n >= g_tx.cap || pf_units(g_tx.text) >= g_tx.max) return;
    g_tx.text[g_tx.n++] = (char)c; g_tx.text[g_tx.n] = 0;
    if (c >= 'A' && c <= 'Z') g_tx.shift = 0;
}
static void tx_finish(int ok) { pthread_mutex_lock(&g_tx_mu); g_tx.ok = ok; g_tx.done = 1; pthread_cond_broadcast(&g_tx_cv); pthread_mutex_unlock(&g_tx_mu); }
static void tx_press(int i) {
    if (i == OVK_OK) { tx_finish(1); return; }
    pthread_mutex_lock(&g_tx_mu);
    if (i == OVK_SHIFT) g_tx.shift = !g_tx.shift;
    else if (i == OVK_SPACE) tx_add(' ');
    else if (i == OVK_DEL) g_tx.n = (int)pf_chop(g_tx.text, (size_t)g_tx.n);
    else tx_add(ov_kb_char(i, g_tx.shift));
    pthread_mutex_unlock(&g_tx_mu);
    dm_haptics_linux_tap(); tx_changed();
}
static void tx_key(uint64_t bit) {   // a controller button: the d-pad moves, A presses, B erases, Y shifts, + accepts, - cancels
    if (bit == DM_BTN_PLUS) { tx_finish(1); return; }
    if (bit == DM_BTN_MINUS) { tx_finish(0); return; }
    if (bit == DM_BTN_A) { int s; pthread_mutex_lock(&g_tx_mu); s = g_tx.sel; pthread_mutex_unlock(&g_tx_mu); tx_press(s); return; }
    pthread_mutex_lock(&g_tx_mu);
    int dx = bit == DM_BTN_LEFT ? -1 : bit == DM_BTN_RIGHT ? 1 : 0, dy = bit == DM_BTN_UP ? -1 : bit == DM_BTN_DOWN ? 1 : 0;
    if (dx || dy) g_tx.sel = ov_kb_move(g_tx.sel, dx, dy);
    else if (bit == DM_BTN_B) g_tx.n = (int)pf_chop(g_tx.text, (size_t)g_tx.n);
    else if (bit == DM_BTN_Y) g_tx.shift = !g_tx.shift;
    pthread_mutex_unlock(&g_tx_mu);
    tx_changed();
}
static void tx_typed(SDL_Keycode k, SDL_Keymod m) {   // a physical keyboard: ASCII characters, Backspace erases, Enter accepts, Esc cancels
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { tx_finish(1); return; }
    if (k == SDLK_ESCAPE) { tx_finish(0); return; }
    int sh = (m & SDL_KMOD_SHIFT) != 0, c = 0;
    if (k >= SDLK_A && k <= SDLK_Z) c = (sh ^ ((m & SDL_KMOD_CAPS) != 0) ? 'A' : 'a') + (int)(k - SDLK_A);
    else if (k >= SDLK_0 && k <= SDLK_9) c = sh ? ")!@#$%^&*("[k - SDLK_0] : '0' + (int)(k - SDLK_0);
    else if (k == SDLK_SPACE) c = ' ';
    else if (k == SDLK_MINUS) c = sh ? '_' : '-'; else if (k == SDLK_PERIOD) c = sh ? '>' : '.'; else if (k == SDLK_COMMA) c = sh ? '<' : ',';
    else if (k == SDLK_APOSTROPHE) c = sh ? '"' : '\''; else if (k == SDLK_SLASH) c = sh ? '?' : '/';
    pthread_mutex_lock(&g_tx_mu);
    if (k == SDLK_BACKSPACE) g_tx.n = (int)pf_chop(g_tx.text, (size_t)g_tx.n); else tx_add(c);
    pthread_mutex_unlock(&g_tx_mu);
    tx_changed();
}
static pthread_mutex_t g_keyboard_mu = PTHREAD_MUTEX_INITIALIZER;
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!out || out_cap <= 0 || !g_win) return -1;
    if (pthread_mutex_trylock(&g_keyboard_mu)) { LOG("[text] request cancelled: another keyboard request is active"); return 0; }
    pthread_mutex_lock(&g_tx_mu);
    const char *t = title && *title ? title : guide && *guide ? guide : sub && *sub ? sub : "Enter text";
    snprintf(g_tx.title, sizeof g_tx.title, "%s", t); pf_trim(g_tx.title);
    g_tx.cap = out_cap - 1 < (int)sizeof g_tx.text - 1 ? out_cap - 1 : (int)sizeof g_tx.text - 1;
    g_tx.n = 0; g_tx.text[0] = 0;
    for (const char *s = initial ? initial : ""; *s;) {   // the initial text without its control characters, whole characters only
        const char *c0 = s; uint32_t c = pf_next(&s); int k = (int)(s - c0);
        if (c < 32 || c == 127) continue;
        if (g_tx.n + k > g_tx.cap) break;
        memcpy(g_tx.text + g_tx.n, c0, (size_t)k); g_tx.n += k;
    }
    g_tx.text[g_tx.n] = 0;
    g_tx.max = max_chars > 0 && max_chars < (int)sizeof g_tx.text - 1 ? max_chars : (int)sizeof g_tx.text - 1;
    g_tx.done = g_tx.ok = 0; g_tx.sel = 11; g_tx.shift = g_tx.n == 0;   // the highlight on q; a capital first
    pthread_mutex_unlock(&g_tx_mu);
    dm_pad_set(0, 0, 0, 0, 0);
    atomic_store(&g_tx_on, 1); hold_sync(); tx_changed();
    LOG("[text] entry open: \"%s\" (at most %d characters)", g_tx.title, g_tx.max);
    pthread_mutex_lock(&g_tx_mu);
    while (!g_tx.done) pthread_cond_wait(&g_tx_cv, &g_tx_mu);
    int ok = g_tx.ok; if (ok) snprintf(out, (size_t)out_cap, "%s", g_tx.text);
    pthread_mutex_unlock(&g_tx_mu);
    atomic_store(&g_tx_on, 0); hold_sync(); dm_overlay_redraw();
    LOG("[text] entry %s", ok ? "accepted" : "cancelled");
    pthread_mutex_unlock(&g_keyboard_mu);
    return ok;
}

// ---- controllers: every one feeds player 1 (main thread) ----
static uint64_t g_kbd;    // the keyboard's buttons
static uint64_t g_prev;   // the buttons last seen, for the panels' presses
static int g_guide, g_guide_combo;   // Guide held; Start was pressed while it was (quit, not the menu)
static int32_t axis(Sint16 v) { int a = v < -32767 ? -32767 : v; return a > -4000 && a < 4000 ? 0 : a; }
static uint64_t face(SDL_Gamepad *g, SDL_GamepadButton b) {
    static const uint64_t pos[4] = {DM_BTN_B, DM_BTN_A, DM_BTN_Y, DM_BTN_X};   // SOUTH, EAST, WEST, NORTH: the Switch's layout by position
    if (pad_labels()) switch (SDL_GetGamepadButtonLabel(g, b)) {   // by label: an Xbox-style A is the game's A (a PlayStation controller has no letters: by position)
    case SDL_GAMEPAD_BUTTON_LABEL_A: return DM_BTN_A; case SDL_GAMEPAD_BUTTON_LABEL_B: return DM_BTN_B;
    case SDL_GAMEPAD_BUTTON_LABEL_X: return DM_BTN_X; case SDL_GAMEPAD_BUTTON_LABEL_Y: return DM_BTN_Y; default: break;
    }
    return pos[b - SDL_GAMEPAD_BUTTON_SOUTH];
}
static uint64_t pad_buttons(SDL_Gamepad *g) {
    static const struct { SDL_GamepadButton b; uint64_t bit; } M[] = {
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, DM_BTN_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, DM_BTN_R}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, DM_BTN_LS},
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, DM_BTN_RS}, {SDL_GAMEPAD_BUTTON_START, DM_BTN_PLUS}, {SDL_GAMEPAD_BUTTON_BACK, DM_BTN_MINUS},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, DM_BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, DM_BTN_DOWN}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, DM_BTN_LEFT},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, DM_BTN_RIGHT}};
    uint64_t b = 0;
    for (SDL_GamepadButton f = SDL_GAMEPAD_BUTTON_SOUTH; f <= SDL_GAMEPAD_BUTTON_NORTH; f++) if (SDL_GetGamepadButton(g, f)) b |= face(g, f);
    for (unsigned i = 0; i < sizeof M / sizeof *M; i++) if (SDL_GetGamepadButton(g, M[i].b)) b |= M[i].bit;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) b |= DM_BTN_ZL;
    if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) b |= DM_BTN_ZR;
    return b;
}
static void pad_publish(void) {
    uint64_t b = g_kbd; int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n; i++) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]); if (!g) continue;
        b |= pad_buttons(g);
        int32_t x = axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX)), y = -axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));   // SDL's y grows downward, the Switch's upward
        int32_t u = axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX)), v = -axis(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
        if (abs(x) > abs(lx)) lx = x; if (abs(y) > abs(ly)) ly = y; if (abs(u) > abs(rx)) rx = u; if (abs(v) > abs(ry)) ry = v;
    }
    SDL_free(ids);
    int panel = atomic_load(&g_menu) || atomic_load(&g_tx_on);
    if (panel) {   // the panels take presses (the left stick as the d-pad); the game sees nothing
        if (lx > 24000) b |= DM_BTN_RIGHT; else if (lx < -24000) b |= DM_BTN_LEFT;
        if (ly > 24000) b |= DM_BTN_UP; else if (ly < -24000) b |= DM_BTN_DOWN;
        uint64_t down = b & ~g_prev;
        for (int k = 0; k < 16; k++) if (down >> k & 1) { if (atomic_load(&g_tx_on)) tx_key(1ull << k); else menu_key(1ull << k); }
    } else dm_pad_set(b, lx, ly, rx, ry);
    g_prev = b;
}
static uint64_t key_bit(SDL_Keycode k) {   // a keyboard for desktop runs: arrows, Z/X = B/A, A/S = Y/X, Q/W = L/R, 1/2 = ZL/ZR, Enter = +, Backspace = -
    switch (k) {
    case SDLK_UP: return DM_BTN_UP; case SDLK_DOWN: return DM_BTN_DOWN; case SDLK_LEFT: return DM_BTN_LEFT; case SDLK_RIGHT: return DM_BTN_RIGHT;
    case SDLK_Z: return DM_BTN_B; case SDLK_X: return DM_BTN_A; case SDLK_A: return DM_BTN_Y; case SDLK_S: return DM_BTN_X;
    case SDLK_Q: return DM_BTN_L; case SDLK_W: return DM_BTN_R; case SDLK_1: return DM_BTN_ZL; case SDLK_2: return DM_BTN_ZR;
    case SDLK_RETURN: return DM_BTN_PLUS; case SDLK_BACKSPACE: return DM_BTN_MINUS;
    default: return 0;
    }
}
static void guide_event(SDL_GamepadButtonEvent *e) {   // Guide alone toggles the menu (on release); Guide + Start quits
    if (e->button == SDL_GAMEPAD_BUTTON_GUIDE) {
        if (e->down) { g_guide = 1; g_guide_combo = 0; return; }
        g_guide = 0;
        if (!g_guide_combo && !atomic_load(&g_tx_on)) { if (atomic_load(&g_menu)) menu_close(); else menu_open(); }
    } else if (e->button == SDL_GAMEPAD_BUTTON_START && e->down && g_guide) { LOG("quit: Guide + Start"); _exit(0); }
}

// ---- the host ----
static int g_hargc; static char **g_hargv;
static void *host_thread(void *a) {
    (void)a;
    int rc = dm_host_main(g_hargc, g_hargv);
    LOG("host returned %d", rc);
    _exit(rc);
}
static void env_file(const char *path) {   // KEY=VALUE per line (# comments), as the Android app's env.txt
    FILE *f = fopen(path, "r"); char line[1024];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *e = line + strlen(line); while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '='); if (!eq) continue; *eq = 0;
        // porter writes the Android app's files folder (/sdcard/Android/data/<package>/files, datapack.files_folder): it is the data dir here
        const char *v = eq + 1, *pre = "/sdcard/Android/data/", *rest;
        if (!strncmp(v, pre, strlen(pre)) && (rest = strchr(v + strlen(pre), '/')) && !strncmp(rest, "/files", 6) && (!rest[6] || rest[6] == '/')) {
            char p[PATH_MAX + 1024]; snprintf(p, sizeof p, "%s%s", g_root, rest + 6); setenv(line, p, 1);
        } else setenv(line, v, 1);
    }
    fclose(f);
}
extern _Thread_local void *dm_tls_slot_app;   // tls_slot.c
static void tls_slot_check(void) {
    void **tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    if ((void *)&dm_tls_slot_app != (void *)(tp + 2))
        die("the guest thread slot is not at tpidr_el0[2] (%p, tpidr_el0 %p): dm/linux/tls_slot.c must be the first object of the link", (void *)&dm_tls_slot_app, (void *)tp);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <data dir> [-- game arguments]\n", argv[0]); return 2; }
    tls_slot_check();
    if (!realpath(argv[1], g_root)) { fprintf(stderr, "%s: %s\n", argv[1], strerror(errno)); return 2; }
    char p[PATH_MAX + 32];
    setenv("DM_ROOT", g_root, 1);
    snprintf(p, sizeof p, "%s/RomFS", g_root); setenv("DM_ROMFS", p, 0);
    snprintf(p, sizeof p, "%s/save", g_root); mkdir(p, 0700); setenv("DM_SAVE", p, 0);
    snprintf(p, sizeof p, "%s/temp", g_root); mkdir(p, 0700); setenv("DM_TEMP", p, 0);
    snprintf(p, sizeof p, "%s/shaders.dmsv", g_root); setenv("DM_VK_SHADERS", p, 0);
    snprintf(p, sizeof p, "%s/env.txt", g_root); env_file(p);
#ifdef DM_RELEASE
    unsetenv("DM_CTL");
#endif
    settings_load();
    // the host's arguments: the module folder, then whatever followed (-- game arguments)
    g_hargv = calloc((size_t)argc + 2, sizeof *g_hargv); g_hargv[g_hargc++] = argv[0];
    snprintf(p, sizeof p, "%s/mods", g_root); g_hargv[g_hargc++] = strdup(p);
    for (int i = 2; i < argc; i++) g_hargv[g_hargc++] = argv[i];

    const char *hl = getenv("DM_HEADLESS"); int headless = hl && atoi(hl);
    if (!headless) {
        SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
        const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (dm) atomic_store(&g_max_hz, (int)(dm->refresh_rate + 0.5f));
        const char *ws = getenv("DM_WINDOWED"); int windowed = ws && atoi(ws);
        g_win = SDL_CreateWindow("nx2linux", dm && !windowed ? dm->w : 1280, dm && !windowed ? dm->h : 720, SDL_WINDOW_VULKAN | (windowed ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_FULLSCREEN));
        if (!g_win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
        SDL_HideCursor(); win_size();
        dm_haptics_linux_start(atomic_load(&g_set_rumble));
    }
    LOG("NX2A host on Linux: files %s, RomFS %s, saves %s, window %dx%d", g_root, getenv("DM_ROMFS"), getenv("DM_SAVE"), atomic_load(&g_win_w), atomic_load(&g_win_h));

    // the game's main thread: a 16 MB stack mapped here and handed over whole, with a guard page below (front_android.c: IL2CPP's collector scans the reported range)
    size_t ssz = 16 << 20, guard = 64 << 10;
    uint8_t *stk = mmap(NULL, ssz + guard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    pthread_attr_t at; pthread_attr_init(&at); pthread_t t;
    if (stk != MAP_FAILED) { mprotect(stk, guard, PROT_NONE); pthread_attr_setstack(&at, stk + guard, ssz); } else pthread_attr_setstacksize(&at, ssz);
    if (pthread_create(&t, &at, host_thread, NULL)) die("cannot start the host thread");
    pthread_attr_destroy(&at);
    if (headless) { pthread_join(t, NULL); return 0; }

    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) continue;
        switch (e.type) {
        case SDL_EVENT_QUIT: _exit(0);
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: win_size(); break;
        case SDL_EVENT_GAMEPAD_ADDED: { SDL_Gamepad *g = SDL_OpenGamepad(e.gdevice.which); LOG("controller: %s", g ? SDL_GetGamepadName(g) : SDL_GetError());
            if (g) dm_haptics_linux_pad(e.gdevice.which); pad_publish(); break; }
        case SDL_EVENT_GAMEPAD_REMOVED: { SDL_Gamepad *g = SDL_GetGamepadFromID(e.gdevice.which); if (g) SDL_CloseGamepad(g); pad_publish(); break; }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
            dm_haptics_linux_pad(e.gbutton.which);   // rumble goes to the controller in use
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE || e.gbutton.button == SDL_GAMEPAD_BUTTON_START) guide_event(&e.gbutton);
            if (g_guide && e.gbutton.down) g_guide_combo = 1;
            pad_publish(); break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: pad_publish(); break;
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
            int down = e.type == SDL_EVENT_KEY_DOWN;
            if (down && e.key.key == SDLK_F10) { LOG("quit: F10"); _exit(0); }
            if (atomic_load(&g_tx_on)) { if (down) tx_typed(e.key.key, e.key.mod); break; }
            if (down && e.key.key == SDLK_ESCAPE && !e.key.repeat) { if (atomic_load(&g_menu)) menu_close(); else menu_open(); break; }
            if (down) g_kbd |= key_bit(e.key.key); else g_kbd &= ~key_bit(e.key.key);
            pad_publish(); break; }
        default: break;
        }
    }
}
