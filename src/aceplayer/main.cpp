#include "player.h"

int main(int argc, char* argv[])
{
  try
  {
    if(argc != 2)
    {
      fprintf(stderr, "Usage: %s <host>\n", argv[0]);
      return 1;
    }

    auto const addr = Address::parse(argv[1], SERVER_PORT);
    playerMain(addr);

    printf("Finished.\n");
    return 0;
  }
  catch(const std::exception& e)
  {
    fprintf(stderr, "Fatal: %s\n", e.what());
    return 1;
  }
}

