/*
 *  IXHttpConnectionPool.h
 *  Author: ProjectSky
 *  Copyright (c) 2025 SkyServers. All rights reserved.
 */

#pragma once

#include "IXSocket.h"
#include "IXSocketTLSOptions.h"
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace ix
{
    struct PooledConnection
    {
        std::unique_ptr<Socket> socket;
        std::chrono::steady_clock::time_point lastUsed;
    };

    class HttpConnectionPool
    {
    public:
        static HttpConnectionPool& getInstance();

        std::unique_ptr<Socket> acquire(const std::string& host,
                                        int port,
                                        bool tls,
                                        const SocketTLSOptions& tlsOptions,
                                        std::string& errorMsg);

        void release(std::unique_ptr<Socket> socket,
                     const std::string& host,
                     int port,
                     bool tls);

        void setMaxConnectionsPerHost(size_t max);
        void setIdleTimeout(int seconds);
        void clear();

    private:
        HttpConnectionPool() = default;
        ~HttpConnectionPool() = default;
        HttpConnectionPool(const HttpConnectionPool&) = delete;
        HttpConnectionPool& operator=(const HttpConnectionPool&) = delete;

        std::string makeKey(const std::string& host, int port, bool tls) const;
        void cleanup();

        std::map<std::string, std::vector<PooledConnection>> _pool;
        std::mutex _mutex;
        size_t _maxConnectionsPerHost = 4;
        int _idleTimeoutSecs = 60;
    };
} // namespace ix
