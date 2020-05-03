#include <SDL.h>
#include <SDL_opengl.h> // glReadPixels
#include <dlfcn.h> // dlsym
#include <vector>
#include "streamer.h"

#define IMPORT(func) \
  (decltype(func)*)dlsym(RTLD_NEXT, # func);

namespace
{
auto const old_SDL_PollEvent = IMPORT(SDL_PollEvent);
auto const old_SDL_GL_SwapWindow = IMPORT(SDL_GL_SwapWindow);
std::unique_ptr<IStreamingServer> g_server;

void captureFrame(SDL_Window* window)
{
  int width, height;
  SDL_GetWindowSize(window, &width, &height);

  // hack: we need the resolution to be a multiple of 16,
  // so crop the bottom of the picture if needed.
  const int y = height % 16;
  width -= width % 16;
  height -= height % 16;

  std::vector<uint8_t> pixels(width * height * 4);
  glReadPixels(0, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

  if(!g_server)
    g_server = createServer({ width, height });

  g_server->push(pixels);
}
}

int SDL_PollEvent(SDL_Event* event)
{
  int r = old_SDL_PollEvent(event);

  if(r)
    return r;

  if(g_server && g_server->pollEvent(*event))
    return 1;

  return 0; // no event
}

void SDL_GL_SwapWindow(SDL_Window* window)
{
  old_SDL_GL_SwapWindow(window);
  captureFrame(window);
}

