// Exercise the production framed TCP parser across interrupted transports.
#include <bits/stdc++.h>
#include <sigc++/sigc++.h>
#include <AsyncCppApplication.h>
#define protected public
#include <AsyncFramedTcpConnection.h>
#undef protected
#include <sys/socket.h>
#include <unistd.h>

static std::string frame(const std::string& body)
{
  uint32_t size=htonl(body.size());
  return std::string(reinterpret_cast<char*>(&size),4)+body;
}

int main()
{
  Async::CppApplication app;
  Async::FramedTcpConnection connection;
  std::vector<std::string> received;
  connection.frameReceived.connect([&](Async::FramedTcpConnection*, std::vector<uint8_t>& bytes) {
    received.emplace_back(bytes.begin(),bytes.end());
  });
  auto feed=[&](std::string bytes) {
    return connection.onDataReceived(&bytes[0],bytes.size());
  };
  for (int n=0;n<1000;++n)
  {
    int sockets[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    connection.setSocket(sockets[0]);
    // Abort after the length and a partial body, then reuse this connection
    // object, as TcpClient<FramedTcpConnection> does on reconnect.
    std::string partial=frame(std::string(1024,'x')).substr(0,4+n%37);
    assert(feed(partial)==static_cast<int>(partial.size()));
    assert(received.empty());
    connection.disconnect(); close(sockets[1]);
    assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    connection.setSocket(sockets[0]);
    const auto hello=frame("J{\"event\":\"hello\"}");
    assert(feed(hello.substr(0,6))==6);
    assert(received.empty());
    assert(feed(hello.substr(6))==static_cast<int>(hello.size()-6));
    assert((received==std::vector<std::string>{"J{\"event\":\"hello\"}"}));
    received.clear();
    std::string multiple=frame("one")+frame("")+frame("three");
    assert(feed(multiple)==static_cast<int>(multiple.size()));
    assert((received==std::vector<std::string>{"one","","three"}));
    received.clear();
    connection.disconnect(); close(sockets[1]);
  }
  std::cout << "PASS framed TCP: 1000 interrupted-body reconnects, split fresh frames and batched frames\n";
}
