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
#include <span>
#include <string>
#include <atomic>
#include <semaphore>
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
const char *head = "head";
enum Message
{
    ACK,
    FILE_INFO,
    FILE_REQ,
    DOWNLOAD_PIECE,
    ECHO,
    UPDATE_PIECE
};

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
    std::ofstream out;
    std::string path;
    std::string name;
    int size;
    File() {}
    File(std::string path, int size) : path(path), size(size)
    {
        out = std::ofstream(path, std::ios::out | std::ios::binary);
        name = std::string(std::filesystem::path(path).filename().string());
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
    std::counting_semaphore<5> *quene;

    int n = 5;

    Conn(tcp::socket socket) : socket(std::move(socket))
    {
        quene = new std::counting_semaphore<5>(5);
    }
    ~Conn()
    {
        this->socket.close();
        std::cout << "conn destructed" << std::endl;
    }
    awaitable<void> reader()
    {
        std::string msg;

        try
        {
            for (;;)
            {
                co_await async_read(socket, asio::buffer(in_head, HEAD_SIZE), use_awaitable);

                std::string h(in_head.data(), in_head.data() + 4);

                if (h.compare(0, 4, head, 0, 4) != 0)
                {
                    std::cout << "head error" << std::endl;
                    co_return;
                }
                uint32_t in_len;
                in_len = endian_uint32(std::span<char, 4>(in_head.data() + 5, in_head.data() + 9));

                co_await async_read(socket, asio::buffer(in_packet, in_len), use_awaitable);
                switch (in_head[4])
                {
                case ECHO:
                {
                    std::string msg(in_packet.data(), in_len);
                    std::cout << "echo: " << msg << std::endl;
                    std::getline(std::cin, msg);
                    if (msg.compare("exit") == 0)
                    {
                        input();
                        continue;
                    }
                    req_echo(msg);
                    break;
                }
                case DOWNLOAD_PIECE:
                {
                    int path_len = endian_uint32(std::span<char, 4>(in_packet.data(), in_packet.data() + 4));
                    std::string path(out_packet.data() + 4, path_len);
                    int pos = endian_uint32(std::span<char, 4>(in_packet.data() + 4 + path_len, in_packet.data() + 4 + path_len + 4));
                    file_map[path].out.seekp(pos);
                    int offset = MAX_OFFSET;
                    std::cout << "resp pos: " << pos << std::endl;
                    if (pos + MAX_OFFSET >= file_map[path].size)
                    {
                        if (pos + MAX_OFFSET > file_map[path].size)
                        {
                            offset = file_map[path].size - pos;
                        }
                        std::cout << "download done" << std::endl;
                    }
                    file_map[path].out.write(in_packet.data() + 8 + path_len, offset);
                    quene->release();
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
            std::cout << "Exception: " << e.what() << std::endl;
        }
    }
    void do_read()
    {
        co_spawn(
            socket.get_executor(), [p = shared_from_this()]()
            { return p->reader(); },
            detached);
    }
    void do_write(int size)
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
    void req_echo(std::string msg)
    {

        std::copy(head, head + 4, out_head.data());
        out_head[4] = ECHO;
        int out_len = msg.length();
        endian = uint32_endian(out_len);
        std::copy(endian.data(), endian.data() + 4, out_head.data() + 5);
        std::copy(msg.begin(), msg.end(), out_packet.data());
        do_write(out_len);
    }

    void input()
    {
        char n;
        std::string msg;

        std::cout << "\n0~关闭连接\n1~聊天\n2~文件上传\n输入对应数字选择功能:";
        std::cin >> n;
        std::getline(std::cin, msg);
        switch (n)
        {
        case '0':
        {
            socket.close();
            break;
        }
        case '1':
        {
            std::cout << "聊天开启(exit退出)" << std::endl;
            req_echo("");
            break;
        }
        case '2':
        {
            std::string path("D:/c++/asio/bunny.mp4");
            std::string path2("D:/c++/asio/test.mp4");
            int file_size = std::filesystem::file_size(path);
            file_map[path] = File(path2, file_size);
            req_download(path);

            break;
        }
        }
    }

    void req_download(std::string path)
    {
        std::copy(head, head + 4, out_head.data());
        out_head[4] = DOWNLOAD_PIECE;
        int path_len = path.length();

        int out_len = 4 + path_len + 4;
        endian = uint32_endian(out_len);
        std::copy(endian.data(), endian.data() + 4, out_head.data() + 5);
        endian = uint32_endian(path_len);
        std::copy(endian.data(), endian.data() + 4, out_packet.data());
        std::copy(path.begin(), path.end(), out_packet.data() + 4);
        for (int i = 0; i < file_map[path].size; i += MAX_OFFSET)
        {
            endian = uint32_endian(i);
            std::copy(endian.data(), endian.data() + 4, out_packet.data() + 4 + path_len);
            do_write(out_len);
            quene->acquire();
        }
    }
    void start()
    {
        // auto p(shared_from_this());
        do_read();
        input();
    }
};

awaitable<void> connector(asio::io_context &io_context)
{
    try
    {
        tcp::socket s(io_context);
        tcp::resolver resolver(io_context);
        co_await asio::async_connect(s, resolver.resolve("127.0.0.1", "1000"), use_awaitable);

        auto conn = std::make_shared<Conn>(std::move(s));
        conn->start();
    }
    catch (std::exception &e)
    {
        std::cout << "Exception: " << e.what();
    }
}
int main()
{
    asio::io_context io_context(4);
    std::jthread t([&io_context]()
                   { io_context.run(); });
    std::jthread t21([&io_context]()
                     { io_context.run(); });
    std::jthread t3([&io_context]()
                    { io_context.run(); });
    co_spawn(io_context, connector(io_context), detached);
}
