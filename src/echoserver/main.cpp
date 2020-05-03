#include "common/socket.h"
#include <cstdio>
#include <cstring>
#include "SDL.h"

int main()
{
  Socket sock(10888);

  while(1)
  {
    SDL_Delay(200);
    Address addr;
    char pkt[1024];
    int n = sock.recv(addr, pkt, sizeof pkt);

    if(n > 0)
    {
      char buf[2048] {};
      strcat(buf, "Reply from server. You said: '");
      strcat(buf, pkt);
      strcat(buf, "'");
      printf("packet from %s\n", addr.toString().c_str());
      sock.send(addr, buf, strlen(buf));
    }
  }

  return 0;
}

