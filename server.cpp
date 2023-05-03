#include <iostream>
#include <stdio.h>
#include <memory>
#include <fstream>
#include <map>
#include "asio.hpp"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/signal_set.hpp>
#include <asio/write.hpp>
#include <cstdio>
#include <span>
#include <filesystem>
#include <thread>
using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::use_awaitable;
using asio::ip::tcp;
#define MAX_OFFSET 8 * 1024
#define PKT_SIZE 10 * 1024
#define HEAD_SIZE 9
using asio::ip::tcp;

enum Message
{
  ACK,
  FILE_INFO,
  FILE_REQ,
  DOWNLOAD_PIECE,
  ECHO,
  UPDATE_PIECE
};
const char *head = "head";
std::array<char, 4> uint32_endian(uint32_t num)
{

  std::array<char, 4> arr;
  arr[0] = (num & 0x000000FF) >> 0;
  arr[1] = (num & 0x0000FF00) >> 8;
  arr[2] = (num & 0x00FF0000) >> 16;
  arr[3] = (num & 0xFF000000) >> 24;
  return arr;
};

uint32_t endian_uint32(std::span<char, 4> arr)
{
  std::array<unsigned char, 4> arr2;
  for (int i = 0; i < 4; i++)
  {
    arr2[i] = arr[i];
  }
  return (arr2[3] << 24) | (arr2[2] << 16) | (arr2[1] << 8) | arr2[0];
};
struct File
{
  std::ifstream in;
  std::string path;
  std::string name;
  int size;
  File() {}
  File(std::string path) : path(path)
  {
    in = std::ifstream(path, std::ios::in | std::ios::binary);
    name = std::string(std::filesystem::path(path).filename().string());
    size = std::filesystem::file_size(path);
  }
};

struct Conn
    : std::enable_shared_from_this<Conn>
{
  tcp::socket socket;
  std::array<char, PKT_SIZE> in_packet;
  std::array<char, PKT_SIZE> out_packet;
  std::array<char, HEAD_SIZE> in_head;
  std::array<char, HEAD_SIZE> out_head;
  std::array<char, 4> endian;
  std::map<std::string, File> file_map;
  Conn(tcp::socket socket) : socket(std::move(socket))
  {
    std::printf("conn structed\n");
  }
  ~Conn()
  {
    this->socket.close();
    std::cout << "conn destructed" << std::endl;
  }
  awaitable<void> reader()
  {
    try
    {
      for (;;)
      {
        co_await async_read(socket, asio::buffer(in_head, HEAD_SIZE), use_awaitable);
        std::string h(in_head.data(), in_head.data() + 4);

        if (h.compare(0, 4, head, 0, 4) != 0)
        {
          std::cout << "head error, receive: " << h << std::endl;
          co_return;
        }
        uint32_t in_len;
        in_len = endian_uint32(std::span<char, 4>(in_head.data() + 5, in_head.data() + 9));

        co_await async_read(socket, asio::buffer(in_packet, in_len), use_awaitable);
        switch (in_head[4])
        {
        case ECHO:
        {
          handle_echo(in_len);
          break;
        }
        case DOWNLOAD_PIECE:
        {
          handle_download(in_len);
          break;
        }
        default:
          std::cout << "undefine type" << std::endl;
          break;
        }
      }
    }
    catch (std::exception &e)
    {
      std::printf("Exception: %s\n", e.what());
    }
  }
  void do_write(int size)
  {
    try
    {
      co_spawn(
          socket.get_executor(), [&]() -> awaitable<void>
          { co_await async_write(socket, asio::buffer(out_head, HEAD_SIZE), use_awaitable); },
          detached);
      co_spawn(
          socket.get_executor(), [&]() -> awaitable<void>
          { co_await async_write(socket, asio::buffer(out_packet, size), use_awaitable); },
          detached);
    }
    catch (const std::exception &e)
    {
      std::printf("Exception: %s\n", e.what());
    }
  }

  void handle_echo(int in_len)
  {
    std::string msg(in_packet.data(), in_len);
    std::cout << "echo: " << msg << std::endl;
    int out_len = in_len;
    std::copy(in_head.data(), in_head.data() + HEAD_SIZE, out_head.data());
    std::copy(in_packet.data(), in_packet.data() + out_len, out_packet.data());
    do_write(out_len);
  }
  void handle_download(int in_len)
  {
    std::copy(head, head + 4, out_head.data());
    out_head[4] = DOWNLOAD_PIECE;
    int out_len = in_len + MAX_OFFSET;
    endian = uint32_endian(in_len + MAX_OFFSET);
    std::copy(endian.data(), endian.data() + 4, out_head.data() + 5);

    int path_len = endian_uint32(std::span<char, 4>(in_packet.data(), in_packet.data() + 4));
    std::string path(in_packet.data() + 4, path_len);
    if (file_map.count(path) == 0)
    {
      file_map[path] = File(path);
    }
    int pos = endian_uint32(std::span<char, 4>(in_packet.data() + 4 + path_len, in_packet.data() + 4 + path_len + 4));

    std::copy(in_packet.data(), in_packet.data() + 8 + path_len, out_packet.data());
    file_map[path].in.seekg(pos);
    int offset = MAX_OFFSET;
    if (pos + MAX_OFFSET > file_map[path].size)
    {
      offset = file_map[path].size - pos;
    }

    file_map[path].in.read(out_packet.data() + 8 + path_len, offset);
    do_write(out_len);
    std::cout << "handle: " << pos << std::endl;
  }
  void start()
  {
    auto p(shared_from_this());
    co_spawn(
        socket.get_executor(), [p]()
        { return p->reader(); },
        detached);
  }
};

awaitable<void> listener(tcp::acceptor acceptor)
{
  for (;;)
  {
    auto socket = co_await acceptor.async_accept(use_awaitable);
    auto conn = std::make_shared<Conn>(std::move(socket));
    conn->start();
  }
}
int main()
{
  asio::io_context io_context(4);

  co_spawn(io_context, listener(tcp::acceptor(io_context, {tcp::v4(), 1000})), detached);
  io_context.run();
}
