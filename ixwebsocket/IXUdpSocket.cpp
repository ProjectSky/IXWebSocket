/*
 *  IXUdpSocket.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2020 Machine Zone, Inc. All rights reserved.
 */

#include "IXUdpSocket.h"

#include "IXNetSystem.h"
#include <cstring>
#include <sstream>

namespace ix
{
    UdpSocket::UdpSocket(int fd)
        : _sockfd(fd)
        , _server{}
        , _serverLen(0)
        , _addressFamily(AF_INET)
    {
    }

    UdpSocket::~UdpSocket()
    {
        close();
    }

    void UdpSocket::close()
    {
        if (_sockfd == -1) return;

        closeSocket(_sockfd);
        _sockfd = -1;
    }

    int UdpSocket::getErrno()
    {
        int err;

#ifdef _WIN32
        err = WSAGetLastError();
#else
        err = errno;
#endif

        return err;
    }

    bool UdpSocket::isWaitNeeded()
    {
        int err = getErrno();

        if (err == EWOULDBLOCK || err == EAGAIN || err == EINPROGRESS)
        {
            return true;
        }

        return false;
    }

    void UdpSocket::closeSocket(int fd)
    {
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
    }

    bool UdpSocket::init(const std::string& host, int port, std::string& errMsg)
    {
        // DNS resolution with IPv4/IPv6 support
        struct addrinfo hints{}, *result = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;

        std::string portStr = std::to_string(port);
        int ret = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result);
        if (ret != 0 || result == nullptr)
        {
            errMsg = gai_strerror(ret);
            if (result) freeaddrinfo(result);
            return false;
        }

        _addressFamily = result->ai_family;
        _sockfd = socket(_addressFamily, SOCK_DGRAM, IPPROTO_UDP);
        if (_sockfd < 0)
        {
            errMsg = "Could not create socket";
            freeaddrinfo(result);
            return false;
        }

#ifdef _WIN32
        unsigned long nonblocking = 1;
        ioctlsocket(_sockfd, FIONBIO, &nonblocking);
#else
        fcntl(_sockfd, F_SETFL, O_NONBLOCK);
#endif

        _server = {};
        memcpy(&_server, result->ai_addr, result->ai_addrlen);
        _serverLen = result->ai_addrlen;
        freeaddrinfo(result);

        return true;
    }

    IoResult UdpSocket::sendto(const std::string& buffer)
    {
        auto ret = ::sendto(
            _sockfd, buffer.data(), buffer.size(), 0, (struct sockaddr*) &_server, _serverLen);
        if (ret > 0) return {static_cast<size_t>(ret), IoError::Success};
        if (ret == 0) return {0, IoError::ConnectionClosed};
        if (isWaitNeeded()) return {0, IoError::WouldBlock};
        return {0, IoError::Error};
    }

    IoResult UdpSocket::recvfrom(char* buffer, size_t length)
    {
#ifdef _WIN32
        int addressLen = (int) _serverLen;
#else
        socklen_t addressLen = _serverLen;
#endif
        auto ret = ::recvfrom(
            _sockfd, buffer, length, 0, (struct sockaddr*) &_server, &addressLen);
        if (ret > 0) return {static_cast<size_t>(ret), IoError::Success};
        if (ret == 0) return {0, IoError::ConnectionClosed};
        if (isWaitNeeded()) return {0, IoError::WouldBlock};
        return {0, IoError::Error};
    }
} // namespace ix
