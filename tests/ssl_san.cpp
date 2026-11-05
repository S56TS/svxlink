// IPv4 SAN data is four network-order bytes, also on 64-bit hosts.
#include <cassert>
#include <iostream>
#include <openssl/x509v3.h>
#include <AsyncIpAddress.h>
#include <AsyncSslX509ExtSubjectAltName.h>

int main()
{
  for (const char* address : {"127.0.0.1","192.0.2.255","0.0.0.0","255.255.255.255"})
  {
    std::string text="IP:"+std::string(address);
    Async::SslX509ExtSubjectAltName names(text);
    assert(names.toString()=="IP Address:"+std::string(address));
    unsigned count=0;
    names.forEach([&](int type,std::string name) {
      assert(type==GEN_IPADD && name==address); ++count;
    });
    assert(count==1);
  }
  std::cout << "PASS X509 IPv4 SAN: bounded four-byte reads and exact network-order addresses\n";
}
