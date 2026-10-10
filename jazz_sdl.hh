#ifndef __JAZZ_SDL_HH__
#define __JAZZ_SDL_HH__

#include <cstdint>

class jazz;
struct state_t;

/* jazz_sdl: a live SDL2 window for the G364 framebuffer plus keyboard/mouse capture, for
 * interp_mips --jazz --sdl.  Keystrokes become set-1/2 scan codes into the i8042; mouse motion
 * and buttons become standard PS/2 packets on the aux port.  SDL types are kept out of this header
 * (opaque void*) so only jazz_sdl.cc needs SDL.  pump() is called periodically from the run loop;
 * it polls input and renders one frame, and sets s->brk when the window is closed. */
class jazz_sdl {
public:
  jazz_sdl(jazz *jz, state_t *s);
  ~jazz_sdl();
  bool ok() const { return win != nullptr; }
  void pump();
private:
  jazz *jz;
  state_t *s;
  void *win = nullptr, *ren = nullptr, *tex = nullptr;
  uint32_t *pixels = nullptr;
  uint32_t tw = 0, th = 0;
  uint32_t last_render_ms = 0;        /* wall-clock render throttle (~30 Hz) */
  void ensure_texture(uint32_t w, uint32_t h);
};

#endif
