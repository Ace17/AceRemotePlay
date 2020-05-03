#pragma once

#include <stdint.h>
#include <string>

struct Address
{
  uint32_t address;
  int port;

  static Address parse(std::string s, int port);
  std::string toString() const;
};

class Socket
{
public:
  Socket(int port);
  ~Socket();

  void send(Address dstAddr, const void* packet_data, int packet_size);
  int recv(Address& sender, void* packet_data, int max_packet_size);
  int port() const;

private:
  int m_sock;
};

// protocol definition
static const int SERVER_PORT = 0xACE1;

enum Op
{
  KeepAlive,
  KeyEvent,
  Disconnect,
};

