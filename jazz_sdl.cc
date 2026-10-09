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
  /* open smaller than the 1280x1024 framebuffer but resizable; logical size scales the content */
  SDL_Window *wn = SDL_CreateWindow("interp_mips jazz (NT)", SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED, 1024, 820, SDL_WINDOW_RESIZABLE);
  if(wn == nullptr) {
    fprintf(stderr, "[jazz] sdl: CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return;
  }
  SDL_Renderer *rn = SDL_CreateRenderer(wn, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if(rn == nullptr) {
    rn = SDL_CreateRenderer(wn, -1, 0);          /* fall back to software */
  }
  win = wn;
  ren = rn;
  ensure_texture(w, h);
  fprintf(stderr, "[jazz] sdl: window up (framebuffer %ux%u); click to focus, close window to stop\n", w, h);
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
      case SDL_KEYDOWN:
      case SDL_KEYUP:
        if(e.key.repeat) {                       /* the guest does its own key repeat */
          break;
        }
        {
          const char *nm = sdl_key_name(e.key.keysym.sym);
          if(nm) {
            jz->inject_key_name(nm, e.type == SDL_KEYDOWN);
          }
        }
        break;
      case SDL_MOUSEMOTION:
        jz->inject_mouse(e.motion.xrel, e.motion.yrel, ps2_buttons());
        break;
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP:
        jz->inject_mouse(0, 0, ps2_buttons());
        break;
      default:
        break;
      }
  }
  uint32_t w = jz->fb_width(), h = jz->fb_height();
  ensure_texture(w, h);
  jz->fb_rgba(pixels);
  SDL_UpdateTexture((SDL_Texture *)tex, nullptr, pixels, (int)(w * 4));
  SDL_RenderClear((SDL_Renderer *)ren);
  SDL_RenderCopy((SDL_Renderer *)ren, (SDL_Texture *)tex, nullptr, nullptr);
  SDL_RenderPresent((SDL_Renderer *)ren);
}
