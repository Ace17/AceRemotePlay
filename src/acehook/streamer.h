#pragma once

#include <vector>
#include <memory>

union SDL_Event;

struct Size2i
{
  int width, height;
};

struct IStreamingServer
{
  virtual ~IStreamingServer() = default;
  virtual void push(std::vector<uint8_t> const& pixels) = 0;
  virtual bool pollEvent(SDL_Event& event) = 0;
};

std::unique_ptr<IStreamingServer> createServer(Size2i resolution);

