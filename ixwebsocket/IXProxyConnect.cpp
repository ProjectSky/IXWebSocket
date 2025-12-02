/*
 *  IXProxyConnect.cpp
 *  Author: ProjectSky
 *  Copyright (c) 2024 Machine Zone, Inc. All rights reserved.
 */

#include "IXProxyConnect.h"
#include "IXBase64.h"
#include "IXNetSystem.h"
#include <cstring>
#include <sstream>

#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <poll.h>
#include <errno.h>
#endif

namespace ix
{
    bool ProxyConnect::rawSend(int sockfd, const std::string& data,
                               const CancellationRequest& isCancellationRequested)
    {
        size_t offset = 0;
        size_t len = data.size();

        while (offset < len)
        {
            if (isCancellationRequested && isCancellationRequested()) return false;

            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#endif
            ssize_t ret = ::send(sockfd, data.c_str() + offset, len - offset, flags);

            if (ret > 0)
            {
                offset += ret;
            }
            else if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
            {
                continue;
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    bool ProxyConnect::rawRecv(int sockfd, void* buffer, size_t len,
                               const CancellationRequest& isCancellationRequested)
    {
        size_t offset = 0;
        char* buf = static_cast<char*>(buffer);

        while (offset < len)
        {
            if (isCancellationRequested && isCancellationRequested()) return false;

            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#endif
            ssize_t ret = ::recv(sockfd, buf + offset, len - offset, flags);

            if (ret > 0)
            {
                offset += ret;
            }
            else if (ret < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
            {
                struct pollfd pfd;
                pfd.fd = sockfd;
                pfd.events = POLLIN;
                ix::poll(&pfd, 1, 100, nullptr);
                continue;
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    bool ProxyConnect::rawRecvLine(int sockfd, std::string& line,
                                   const CancellationRequest& isCancellationRequested)
    {
        line.clear();
        char c;
        while (true)
        {
            if (!rawRecv(sockfd, &c, 1, isCancellationRequested)) return false;
            line += c;
            if (line.size() >= 2 && line[line.size() - 2] == '\r' && line[line.size() - 1] == '\n')
            {
                return true;
            }
        }
    }

    bool ProxyConnect::connect(int sockfd,
                               const ProxyConfig& proxy,
                               const std::string& targetHost,
                               int targetPort,
                               std::string& errMsg,
                               const CancellationRequest& isCancellationRequested)
    {
        switch (proxy.type)
        {
            case ProxyType::Http:
            case ProxyType::Https:
                return httpConnect(sockfd, proxy, targetHost, targetPort, errMsg, isCancellationRequested);
            case ProxyType::Socks5:
                return socks5Connect(sockfd, proxy, targetHost, targetPort, errMsg, isCancellationRequested);
            default:
                errMsg = "Unknown proxy type";
                return false;
        }
    }

    bool ProxyConnect::httpConnect(int sockfd,
                                   const ProxyConfig& proxy,
                                   const std::string& targetHost,
                                   int targetPort,
                                   std::string& errMsg,
                                   const CancellationRequest& isCancellationRequested)
    {
        std::stringstream ss;
        ss << "CONNECT " << targetHost << ":" << targetPort << " HTTP/1.1\r\n";
        ss << "Host: " << targetHost << ":" << targetPort << "\r\n";

        if (proxy.requiresAuth())
        {
            ss << "Proxy-Authorization: " << buildBasicAuthHeader(proxy.username, proxy.password) << "\r\n";
        }

        ss << "\r\n";

        if (!rawSend(sockfd, ss.str(), isCancellationRequested))
        {
            errMsg = "Failed to send CONNECT request to proxy";
            return false;
        }

        std::string statusLine;
        if (!rawRecvLine(sockfd, statusLine, isCancellationRequested))
        {
            errMsg = "Failed to read proxy response";
            return false;
        }

        int statusCode = 0;
        if (statusLine.find("HTTP/1.") == 0 && statusLine.size() >= 12)
        {
            statusCode = std::stoi(statusLine.substr(9, 3));
        }

        // Read and discard headers
        while (true)
        {
            std::string header;
            if (!rawRecvLine(sockfd, header, isCancellationRequested))
            {
                errMsg = "Failed to read proxy headers";
                return false;
            }
            if (header == "\r\n") break;
        }

        if (statusCode == 200) return true;

        errMsg = "Proxy CONNECT failed with status: " + std::to_string(statusCode);
        return false;
    }

    bool ProxyConnect::socks5Connect(int sockfd,
                                     const ProxyConfig& proxy,
                                     const std::string& targetHost,
                                     int targetPort,
                                     std::string& errMsg,
                                     const CancellationRequest& isCancellationRequested)
    {
        // Step 1: Send greeting with auth methods
        std::string greeting;
        greeting.push_back(0x05); // SOCKS5 version
        if (proxy.requiresAuth())
        {
            greeting.push_back(0x02); // 2 methods
            greeting.push_back(0x00); // No auth
            greeting.push_back(0x02); // Username/password
        }
        else
        {
            greeting.push_back(0x01); // 1 method
            greeting.push_back(0x00); // No auth
        }

        if (!rawSend(sockfd, greeting, isCancellationRequested))
        {
            errMsg = "Failed to send SOCKS5 greeting";
            return false;
        }

        // Step 2: Read server's chosen method
        uint8_t response[2];
        if (!rawRecv(sockfd, response, 2, isCancellationRequested))
        {
            errMsg = "Failed to read SOCKS5 greeting response";
            return false;
        }

        if (response[0] != 0x05)
        {
            errMsg = "Invalid SOCKS5 version in response";
            return false;
        }

        if (response[1] == 0xFF)
        {
            errMsg = "SOCKS5 server rejected all auth methods";
            return false;
        }

        // Step 3: Username/password auth if required
        if (response[1] == 0x02)
        {
            if (!proxy.requiresAuth())
            {
                errMsg = "SOCKS5 server requires auth but no credentials provided";
                return false;
            }

            std::string authRequest;
            authRequest.push_back(0x01); // Auth version
            authRequest.push_back(static_cast<char>(proxy.username.size()));
            authRequest += proxy.username;
            authRequest.push_back(static_cast<char>(proxy.password.size()));
            authRequest += proxy.password;

            if (!rawSend(sockfd, authRequest, isCancellationRequested))
            {
                errMsg = "Failed to send SOCKS5 auth request";
                return false;
            }

            uint8_t authResponse[2];
            if (!rawRecv(sockfd, authResponse, 2, isCancellationRequested))
            {
                errMsg = "Failed to read SOCKS5 auth response";
                return false;
            }

            if (authResponse[1] != 0x00)
            {
                errMsg = "SOCKS5 authentication failed";
                return false;
            }
        }

        // Step 4: Send connect request
        std::string connectRequest;
        connectRequest.push_back(0x05); // Version
        connectRequest.push_back(0x01); // Connect command
        connectRequest.push_back(0x00); // Reserved

        // Use domain name (ATYP = 0x03)
        connectRequest.push_back(0x03);
        connectRequest.push_back(static_cast<char>(targetHost.size()));
        connectRequest += targetHost;

        // Port in network byte order
        connectRequest.push_back(static_cast<char>((targetPort >> 8) & 0xFF));
        connectRequest.push_back(static_cast<char>(targetPort & 0xFF));

        if (!rawSend(sockfd, connectRequest, isCancellationRequested))
        {
            errMsg = "Failed to send SOCKS5 connect request";
            return false;
        }

        // Step 5: Read connect response
        uint8_t connectResponse[4];
        if (!rawRecv(sockfd, connectResponse, 4, isCancellationRequested))
        {
            errMsg = "Failed to read SOCKS5 connect response";
            return false;
        }

        if (connectResponse[0] != 0x05)
        {
            errMsg = "Invalid SOCKS5 version in connect response";
            return false;
        }

        if (connectResponse[1] != 0x00)
        {
            const char* errors[] = {
                "succeeded",
                "general SOCKS server failure",
                "connection not allowed by ruleset",
                "network unreachable",
                "host unreachable",
                "connection refused",
                "TTL expired",
                "command not supported",
                "address type not supported"
            };
            int errCode = connectResponse[1];
            errMsg = "SOCKS5 connect failed: ";
            errMsg += (errCode < 9) ? errors[errCode] : "unknown error";
            return false;
        }

        // Read and discard bound address
        uint8_t atyp = connectResponse[3];
        if (atyp == 0x01) // IPv4
        {
            uint8_t addr[4];
            if (!rawRecv(sockfd, addr, 4, isCancellationRequested)) return false;
        }
        else if (atyp == 0x03) // Domain
        {
            uint8_t len;
            if (!rawRecv(sockfd, &len, 1, isCancellationRequested)) return false;
            std::string domain(len, '\0');
            if (!rawRecv(sockfd, &domain[0], len, isCancellationRequested)) return false;
        }
        else if (atyp == 0x04) // IPv6
        {
            uint8_t addr[16];
            if (!rawRecv(sockfd, addr, 16, isCancellationRequested)) return false;
        }

        // Read bound port
        uint8_t port[2];
        if (!rawRecv(sockfd, port, 2, isCancellationRequested))
        {
            return false;
        }

        return true;
    }

    std::string ProxyConnect::buildBasicAuthHeader(const std::string& username,
                                                   const std::string& password)
    {
        std::string credentials = username + ":" + password;
        return "Basic " + macaron::Base64::Encode(credentials);
    }
} // namespace ix
