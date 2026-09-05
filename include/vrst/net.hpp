#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace vrst::net {
struct Endpoint {
    sockaddr_in address{};
    static std::optional<Endpoint> parse(const std::string& host,std::uint16_t port) {
        Endpoint e;e.address.sin_family=AF_INET;e.address.sin_port=htons(port);
        if(inet_pton(AF_INET,host.c_str(),&e.address.sin_addr)!=1)return std::nullopt;return e;
    }
    bool operator==(const Endpoint& e) const { return address.sin_addr.s_addr==e.address.sin_addr.s_addr&&address.sin_port==e.address.sin_port; }
    std::string ip() const { char b[INET_ADDRSTRLEN]{};inet_ntop(AF_INET,&address.sin_addr,b,sizeof(b));return b; }
    std::uint16_t port() const { return ntohs(address.sin_port); }
};
class Socket {
public:
    explicit Socket(std::uint16_t port=0) {
#ifdef _WIN32
        WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data)!=0)throw std::runtime_error("Winsock initialization failed");
#endif
        fd_=::socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        if(fd_==invalid){cleanup();throw std::runtime_error("Cannot create UDP socket");}
        sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_ANY);a.sin_port=htons(port);
        if(::bind(fd_,reinterpret_cast<sockaddr*>(&a),sizeof(a))!=0){cleanup();throw std::runtime_error("Cannot bind UDP port "+std::to_string(port)+" (already in use?)");}
#ifdef _WIN32
        u_long mode=1;if(ioctlsocket(fd_,FIONBIO,&mode)!=0){cleanup();throw std::runtime_error("Cannot configure UDP socket");}
#else
        if(fcntl(fd_,F_SETFL,O_NONBLOCK)<0){cleanup();throw std::runtime_error("Cannot configure UDP socket");}
#endif
    }
    ~Socket(){cleanup();}
    Socket(const Socket&)=delete;Socket& operator=(const Socket&)=delete;
    std::uint16_t port() const { sockaddr_in a{};length_type len=sizeof(a);getsockname(fd_,reinterpret_cast<sockaddr*>(&a),&len);return ntohs(a.sin_port); }
    bool send(const Endpoint& to,const std::uint8_t* b,std::size_t n) const {
        return ::sendto(fd_,reinterpret_cast<const char*>(b),static_cast<int>(n),0,reinterpret_cast<const sockaddr*>(&to.address),sizeof(to.address))==static_cast<int>(n);
    }
    int receive(std::uint8_t* b,std::size_t n,Endpoint& from) const {
        length_type len=sizeof(from.address);
        return static_cast<int>(::recvfrom(fd_,reinterpret_cast<char*>(b),static_cast<int>(n),0,reinterpret_cast<sockaddr*>(&from.address),&len));
    }
    void wait(int milliseconds) const {
        fd_set set;FD_ZERO(&set);FD_SET(fd_,&set);timeval t{milliseconds/1000,(milliseconds%1000)*1000};
        select(static_cast<int>(fd_+1),&set,nullptr,nullptr,&t);
    }
private:
#ifdef _WIN32
    using handle=SOCKET;using length_type=int;static constexpr handle invalid=INVALID_SOCKET;
#else
    using handle=int;using length_type=socklen_t;static constexpr handle invalid=-1;
#endif
    handle fd_{invalid};
    void cleanup(){
        if(fd_!=invalid){
#ifdef _WIN32
            closesocket(fd_);
#else
            ::close(fd_);
#endif
            fd_=invalid;
        }
#ifdef _WIN32
        WSACleanup();
#endif
    }
};
} // namespace vrst::net
