/*
 *  IXGetFreePort.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2019 Machine Zone. All rights reserved.
 */

#include "IXGetFreePort.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXSocket.h>
#include <cstring>
#include <random>
#include <string>

namespace ix
{
    int getAnyFreePortRandom()
    {
        std::random_device rd;
        std::uniform_int_distribution<int> dist(1024 + 1, 65535);

        return dist(rd);
    }

    int getAnyFreePort(int addressFamily)
    {
        socket_t sockfd;
        if ((sockfd = socket(addressFamily, SOCK_STREAM, 0)) < 0)
        {
            return getAnyFreePortRandom();
        }

        int enable = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (char*) &enable, sizeof(enable)) < 0)
        {
            Socket::closeSocket(sockfd);
            return getAnyFreePortRandom();
        }

        // Bind to port 0. This is the standard way to get a free port.
        int port = -1;
        if (addressFamily == AF_INET)
        {
            struct sockaddr_in server;
            memset(&server, 0, sizeof(server));
            server.sin_family = AF_INET;
            server.sin_port = htons(0);
            server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            if (bind(sockfd, (struct sockaddr*) &server, sizeof(server)) < 0)
            {
                Socket::closeSocket(sockfd);
                return getAnyFreePortRandom();
            }

            struct sockaddr_in sa;
            socklen_t len = sizeof(sa);
            if (getsockname(sockfd, (struct sockaddr*) &sa, &len) < 0)
            {
                Socket::closeSocket(sockfd);
                return getAnyFreePortRandom();
            }
            port = ntohs(sa.sin_port);
        }
        else // AF_INET6
        {
            struct sockaddr_in6 server;
            memset(&server, 0, sizeof(server));
            server.sin6_family = AF_INET6;
            server.sin6_port = htons(0);
            server.sin6_addr = in6addr_loopback;

            if (bind(sockfd, (struct sockaddr*) &server, sizeof(server)) < 0)
            {
                Socket::closeSocket(sockfd);
                return getAnyFreePortRandom();
            }

            struct sockaddr_in6 sa;
            socklen_t len = sizeof(sa);
            if (getsockname(sockfd, (struct sockaddr*) &sa, &len) < 0)
            {
                Socket::closeSocket(sockfd);
                return getAnyFreePortRandom();
            }
            port = ntohs(sa.sin6_port);
        }

        Socket::closeSocket(sockfd);
        return port;
    }

    int getAnyFreePort()
    {
        return getAnyFreePort(AF_INET);
    }

    int getFreePort()
    {
        while (true)
        {
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
            int port = getAnyFreePortRandom();
#else
            int port = getAnyFreePort();
#endif
#else
            int port = getAnyFreePort();
#endif
            //
            // Only port above 1024 can be used by non root users, but for some
            // reason I got port 7 returned with macOS when binding on port 0...
            //
            if (port > 1024)
            {
                return port;
            }
        }

        return -1;
    }
} // namespace ix
