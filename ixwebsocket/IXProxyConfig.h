/*
 *  IXProxyConfig.h
 *  Author: ProjectSky
 *  Copyright (c) 2024 Machine Zone, Inc. All rights reserved.
 */

#pragma once

#include <string>

namespace ix
{
    enum class ProxyType
    {
        None,
        Http,
        Https,
        Socks5
    };

    struct ProxyConfig
    {
        ProxyType type = ProxyType::None;
        std::string host;
        int port = 0;
        std::string username;
        std::string password;

        bool isEnabled() const { return type != ProxyType::None && !host.empty() && port > 0; }
        bool requiresAuth() const { return !username.empty(); }

        // Parse proxy URL: http://user:pass@host:port, socks5://host:port, etc.
        static ProxyConfig fromUrl(const std::string& url);
    };
} // namespace ix
