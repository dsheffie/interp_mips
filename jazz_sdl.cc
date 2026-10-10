#include <SDL.h>
#include <cstdlib>
#include "jazz_sdl.hh"
#include "jazz.hh"
#include "interpret.hh"

/* SDL keycode -> the key names in jazz.cc's g_keys table (set-1/set-2 scan codes). */
static const char *sdl_key_name(SDL_Keycode k) {
  switch(k)
    {
    case SDLK_a: return "a"; case SDLK_b: return "b"; case SDLK_c: return "c"; case SDLK_d: return "d";
    case SDLK_e: return "e"; case SDLK_f: return "f"; case SDLK_g: return "g"; case SDLK_h: return "h";
    case SDLK_i: return "i"; case SDLK_j: return "j"; case SDLK_k: return "k"; case SDLK_l: return "l";
    case SDLK_m: return "m"; case SDLK_n: return "n"; case SDLK_o: return "o"; case SDLK_p: return "p";
    case SDLK_q: return "q"; case SDLK_r: return "r"; case SDLK_s: return "s"; case SDLK_t: return "t";
    case SDLK_u: return "u"; case SDLK_v: return "v"; case SDLK_w: return "w"; case SDLK_x: return "x";
    case SDLK_y: return "y"; case SDLK_z: return "z";
    case SDLK_1: return "1"; case SDLK_2: return "2"; case SDLK_3: return "3"; case SDLK_4: return "4";
    case SDLK_5: return "5"; case SDLK_6: return "6"; case SDLK_7: return "7"; case SDLK_8: return "8";
    case SDLK_9: return "9"; case SDLK_0: return "0";
    case SDLK_MINUS: return "-"; case SDLK_EQUALS: return "="; case SDLK_LEFTBRACKET: return "[";
    case SDLK_RIGHTBRACKET: return "]"; case SDLK_SEMICOLON: return ";"; case SDLK_QUOTE: return "'";
    case SDLK_BACKQUOTE: return "`"; case SDLK_BACKSLASH: return "\\"; case SDLK_COMMA: return ",";
    case SDLK_PERIOD: return "."; case SDLK_SLASH: return "/";
    case SDLK_ESCAPE: return "esc"; case SDLK_BACKSPACE: return "backspace"; case SDLK_TAB: return "tab";
    case SDLK_RETURN: return "enter"; case SDLK_SPACE: return "space"; case SDLK_CAPSLOCK: return "capslock";
    case SDLK_LCTRL: case SDLK_RCTRL: return "ctrl";
    case SDLK_LSHIFT: return "shift"; case SDLK_RSHIFT: return "rshift";
    case SDLK_LALT: case SDLK_RALT: return "alt";
    case SDLK_F1: return "f1"; case SDLK_F2: return "f2"; case SDLK_F3: return "f3"; case SDLK_F4: return "f4";
    case SDLK_F5: return "f5"; case SDLK_F6: return "f6"; case SDLK_F7: return "f7"; case SDLK_F8: return "f8";
    case SDLK_F9: return "f9"; case SDLK_F10: return "f10"; case SDLK_F11: return "f11"; case SDLK_F12: return "f12";
    case SDLK_INSERT: return "insert"; case SDLK_DELETE: return "delete"; case SDLK_HOME: return "home";
    case SDLK_END: return "end"; case SDLK_PAGEUP: return "pgup"; case SDLK_PAGEDOWN: return "pgdn";
    case SDLK_UP: return "up"; case SDLK_DOWN: return "down"; case SDLK_LEFT: return "left"; case SDLK_RIGHT: return "right";
    default: return nullptr;
    }
}

static uint8_t ps2_buttons() {
  uint32_t m = SDL_GetMouseState(nullptr, nullptr);
  uint8_t b = 0;
  if(m & SDL_BUTTON_LMASK) { b |= 0x01; }
  if(m & SDL_BUTTON_RMASK) { b |= 0x02; }
  if(m & SDL_BUTTON_MMASK) { b |= 0x04; }
  return b;
}

jazz_sdl::jazz_sdl(jazz *jz, state_t *s) : jz(jz), s(s) {
  if(SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "[jazz] sdl: SDL_Init failed: %s\n", SDL_GetError());
    return;
  }
  uint32_t w = jz->fb_width(), h = jz->fb_height();
  /* Nearest-neighbor scaling: the default (bilinear) smears the 8bpp console text into an
   * unreadable blur whenever the window isn't exactly the framebuffer size.  Must be set
   * before the renderer is created. */
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  /* Open at the native framebuffer size so text renders 1:1 (crisp).  Still resizable; with
   * nearest filtering a resized window stays sharp instead of smearing.  NOTE: no
   * SDL_WINDOW_ALLOW_HIGHDPI -- on Retina it quadruples the render backbuffer (2560x2048),
   * which slows the render/sim loop enough to make the guest cursor visibly lag the mouse. */
  SDL_Window *wn = SDL_CreateWindow("interp_mips jazz (NT)", SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED, (int)w, (int)h, SDL_WINDOW_RESIZABLE);
  if(wn == nullptr) {
    fprintf(stderr, "[jazz] sdl: CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return;
  }
  SDL_Renderer *rn = SDL_CreateRenderer(wn, -1, SDL_RENDERER_ACCELERATED)   /* no vsync: never block the sim */;
  if(rn == nullptr) {
    rn = SDL_CreateRenderer(wn, -1, 0);          /* fall back to software */
  }
  win = wn;
  ren = rn;
  ensure_texture(w, h);
  SDL_RaiseWindow(wn);                            /* a terminal-launched (non-bundled) binary opens in the background on macOS */
  fprintf(stderr, "[jazz] sdl: window up (framebuffer %ux%u); click the window to capture keyboard+mouse, Ctrl+Alt+G to release, close window to stop\n", w, h);
}

/* Modal input capture.  On macOS a bare (non-.app) binary never becomes the key app on its own, so
 * keystrokes leak to the terminal; and without pointer capture the host and guest cursors drift apart
 * (different acceleration, deltas clamp at the window edge) so you can't aim at a dialog.  Relative-mouse
 * mode hides the host cursor, feeds raw deltas to the PS/2 mouse, and forces the window to the foreground,
 * which fixes both.  Ctrl+Alt (or switching away) releases it.  Keyboard/mouse are injected only while
 * captured, so this is a clean modal grab. */
void jazz_sdl::set_grab(bool on) {
  if(on == grabbed) {
    return;
  }
  grabbed = on;
  SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
  if(on) {
    SDL_RaiseWindow((SDL_Window *)win);
  }
  SDL_SetWindowTitle((SDL_Window *)win,
                     on ? "interp_mips jazz (NT) -- captured; Ctrl+Alt+Backspace = Ctrl-Alt-Del, Ctrl+Alt+G to release"
                        : "interp_mips jazz (NT) -- click to capture keyboard+mouse");
}

jazz_sdl::~jazz_sdl() {
  free(pixels);
  if(tex) { SDL_DestroyTexture((SDL_Texture *)tex); }
  if(ren) { SDL_DestroyRenderer((SDL_Renderer *)ren); }
  if(win) { SDL_DestroyWindow((SDL_Window *)win); SDL_Quit(); }
}

void jazz_sdl::ensure_texture(uint32_t w, uint32_t h) {
  if(w == tw and h == th and tex != nullptr) {
    return;
  }
  if(tex) { SDL_DestroyTexture((SDL_Texture *)tex); }
  tex = SDL_CreateTexture((SDL_Renderer *)ren, SDL_PIXELFORMAT_ARGB8888,
                          SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
  SDL_RenderSetLogicalSize((SDL_Renderer *)ren, (int)w, (int)h);
  pixels = (uint32_t *)realloc(pixels, (size_t)w * h * 4);
  tw = w;
  th = h;
}

void jazz_sdl::pump() {
  if(win == nullptr) {
    return;
  }
  SDL_Event e;
  while(SDL_PollEvent(&e)) {
    switch(e.type)
      {
      case SDL_QUIT:
        s->brk = 1;
        return;
      case SDL_WINDOWEVENT:
        if(e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {   /* Cmd-Tab / click away frees the cursor */
          set_grab(false);
        }
        break;
      case SDL_KEYDOWN:
      case SDL_KEYUP:
        if(e.key.repeat) {                       /* the guest does its own key repeat */
          break;
        }
        /* Ctrl+Alt+G is the capture-release "host key" (QEMU convention).  It needs the 'G' so that
         * bare Ctrl+Alt+Del passes through to NT (otherwise the release fires the instant Ctrl+Alt
         * are held and Del never reaches the guest).  Send the Ctrl/Alt breaks so NT doesn't see
         * them stuck down after the release. */
        if(grabbed and e.type == SDL_KEYDOWN and e.key.keysym.sym == SDLK_g and
           (SDL_GetModState() & KMOD_CTRL) and (SDL_GetModState() & KMOD_ALT)) {
          set_grab(false);
          jz->inject_key_name("ctrl", false);
          jz->inject_key_name("alt", false);
          break;
        }
        /* Ctrl+Alt+Backspace injects the full Ctrl+Alt+Del "secure attention sequence".
         * On a Mac keyboard the key labelled "delete" is actually Backspace, so pressing
         * Ctrl+Alt+Delete there sends Ctrl+Alt+Backspace and never reaches NT (the real
         * forward-Delete is fn+delete).  Map the natural Mac combo to a self-contained SAS so
         * the three-finger salute Just Works regardless of which Ctrl/Alt are physically held. */
        if(grabbed and e.type == SDL_KEYDOWN and e.key.keysym.sym == SDLK_BACKSPACE and
           (SDL_GetModState() & KMOD_CTRL) and (SDL_GetModState() & KMOD_ALT)) {
          jz->inject_key_name("ctrl",   true);
          jz->inject_key_name("alt",    true);
          jz->inject_key_name("delete", true);
          jz->inject_key_name("delete", false);
          jz->inject_key_name("alt",    false);
          jz->inject_key_name("ctrl",   false);
          break;
        }
        if(grabbed) {
          const char *nm = sdl_key_name(e.key.keysym.sym);
          if(nm) {
            jz->inject_key_name(nm, e.type == SDL_KEYDOWN);
          }
        }
        break;
      case SDL_MOUSEMOTION:
        if(grabbed) {
          jz->inject_mouse(e.motion.xrel, e.motion.yrel, ps2_buttons());
        }
        break;
      case SDL_MOUSEBUTTONDOWN:
        if(not grabbed) {                        /* first click captures input; consume it */
          set_grab(true);
          break;
        }
        jz->inject_mouse(0, 0, ps2_buttons());
        break;
      case SDL_MOUSEBUTTONUP:
        if(grabbed) {
          jz->inject_mouse(0, 0, ps2_buttons());
        }
        break;
      default:
        break;
      }
  }
  /* events are polled on every call (keeps the window responsive); rendering the
   * 1.3M-pixel framebuffer is throttled to ~30 Hz by wall clock */
  uint32_t now = SDL_GetTicks();
  if(now - last_render_ms < 33) {
    return;
  }
  last_render_ms = now;
  uint32_t w = jz->fb_width(), h = jz->fb_height();
  ensure_texture(w, h);
  jz->fb_rgba(pixels);
  SDL_UpdateTexture((SDL_Texture *)tex, nullptr, pixels, (int)(w * 4));
  SDL_RenderClear((SDL_Renderer *)ren);
  SDL_RenderCopy((SDL_Renderer *)ren, (SDL_Texture *)tex, nullptr, nullptr);
  SDL_RenderPresent((SDL_Renderer *)ren);
}
