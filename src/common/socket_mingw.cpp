#include "socket.h"

#include <assert.h>
#include <winsock2.h>

#include <string>
#include <stdexcept>

std::string Address::toString() const
{
  sockaddr_in sa {};
  sa.sin_addr.s_addr = htonl(address);

  return inet_ntoa(sa.sin_addr) + std::string(":") + std::to_string(port);
}

Address Address::parse(std::string s, int port)
{
  return { ntohl(inet_addr(s.c_str())), port };
}

Socket::Socket(int port)
{
  WSADATA wsaData;
  WSAStartup(MAKEWORD(2, 2), &wsaData);

  m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

  if(m_sock <= 0)
    throw std::runtime_error("failed to create UDP socket");

  int enable = 1;

  if(setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&enable, sizeof(int)) < 0)
    throw std::runtime_error("failed to reuse addr");

  sockaddr_in address;
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);

  if(bind(m_sock, (const sockaddr*)&address, sizeof(sockaddr_in)) < 0)
    throw std::runtime_error("failed to bind socket");

  // set non-blocking
  unsigned long Yes = TRUE;
  ioctlsocket(m_sock, FIONBIO, &Yes);

  printf("Listening on: udp/%d\n", this->port());
}

Socket::~Socket()
{
  closesocket(m_sock);
  WSACleanup();
}

void Socket::send(Address dstAddr, const void* packet_data, int packet_size)
{
  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(dstAddr.address);
  addr.sin_port = htons(dstAddr.port);

  int sent_bytes = sendto(m_sock, (const char*)packet_data, packet_size, 0, (sockaddr*)&addr, sizeof(sockaddr_in));

  if(sent_bytes != packet_size)
  {
    printf("failed to send packet\n");
    assert(0);
  }
}

int Socket::recv(Address& sender, void* packet_data, int max_packet_size)
{
  sockaddr_in from;
  int fromLength = sizeof(from);

  int bytes = recvfrom(m_sock,
                       (char*)packet_data,
                       max_packet_size,
                       0,
                       (sockaddr*)&from,
                       &fromLength);

  sender.address = ntohl(from.sin_addr.s_addr);
  sender.port = ntohs(from.sin_port);

  return bytes;
}

int Socket::port() const
{
  struct sockaddr_in sin;
  int len = sizeof(sin);

  if(getsockname(m_sock, (struct sockaddr*)&sin, &len) == -1)
  {
    perror("getsockname");
    assert(0);
  }

  return ntohs(sin.sin_port);
}

