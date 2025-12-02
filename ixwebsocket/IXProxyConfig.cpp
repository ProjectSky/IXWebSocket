/*
 *  IXProxyConfig.cpp
 *  Author: ProjectSky
 *  Copyright (c) 2024 Machine Zone, Inc. All rights reserved.
 */

#include "IXProxyConfig.h"

namespace ix
{
    // Parse: http://user:pass@host:port, socks5://host:port, etc.
    ProxyConfig ProxyConfig::fromUrl(const std::string& url)
    {
        ProxyConfig config;
        if (url.empty()) return config;

        std::string remaining = url;

        // Parse scheme
        size_t schemeEnd = remaining.find("://");
        if (schemeEnd == std::string::npos) return config;

        std::string scheme = remaining.substr(0, schemeEnd);
        remaining = remaining.substr(schemeEnd + 3);

        if (scheme == "http")
            config.type = ProxyType::Http;
        else if (scheme == "https")
            config.type = ProxyType::Https;
        else if (scheme == "socks5")
            config.type = ProxyType::Socks5;
        else
            return config;

        // Parse auth (user:pass@)
        size_t atPos = remaining.find('@');
        if (atPos != std::string::npos)
        {
            std::string auth = remaining.substr(0, atPos);
            remaining = remaining.substr(atPos + 1);

            size_t colonPos = auth.find(':');
            if (colonPos != std::string::npos)
            {
                config.username = auth.substr(0, colonPos);
                config.password = auth.substr(colonPos + 1);
            }
            else
            {
                config.username = auth;
            }
        }

        // Parse host:port
        size_t colonPos = remaining.rfind(':');
        if (colonPos != std::string::npos)
        {
            config.host = remaining.substr(0, colonPos);
            config.port = std::stoi(remaining.substr(colonPos + 1));
        }
        else
        {
            config.host = remaining;
            // Default ports
            if (config.type == ProxyType::Http)
                config.port = 80;
            else if (config.type == ProxyType::Https)
                config.port = 443;
            else if (config.type == ProxyType::Socks5)
                config.port = 1080;
        }

        return config;
    }
} // namespace ix
