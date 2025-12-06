/*
 *  IXProxyConfig.cpp
 *  Author: ProjectSky
 *  Copyright (c) 2025 SkyServers. All rights reserved.
 */

#include "IXProxyConfig.h"
#include "IXUrlParser.h"

namespace ix
{
    ProxyConfig ProxyConfig::fromUrl(const std::string& url)
    {
        ProxyConfig config;
        if (url.empty()) return config;

        std::string protocol, host, path, query;
        int port;

        if (!UrlParser::parse(url, protocol, host, path, query, port,
                              config.username, config.password))
        {
            return config;
        }

        if (protocol == "http")
            config.type = ProxyType::Http;
        else if (protocol == "https")
            config.type = ProxyType::Https;
        else if (protocol == "socks5")
            config.type = ProxyType::Socks5;
        else
            return config;

        config.host = host;
        config.port = (port > 0) ? port : (protocol == "socks5" ? 1080 : (protocol == "https" ? 443 : 80));

        return config;
    }
} // namespace ix
