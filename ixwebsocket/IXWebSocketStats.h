/*
 *  IXWebSocketStats.h
 *  Author: ProjectSky
 *  Copyright (c) 2024 Machine Zone, Inc. All rights reserved.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ix
{
    struct WebSocketStats
    {
        std::atomic<uint64_t> messagesSent{0};
        std::atomic<uint64_t> messagesReceived{0};
        std::atomic<uint64_t> bytesSent{0};
        std::atomic<uint64_t> bytesReceived{0};
        std::atomic<uint64_t> pingsSent{0};
        std::atomic<uint64_t> pongsSent{0};
        std::atomic<uint64_t> pingsReceived{0};
        std::atomic<uint64_t> pongsReceived{0};
        std::chrono::time_point<std::chrono::steady_clock> connectionStartTime;

        void reset()
        {
            messagesSent = 0;
            messagesReceived = 0;
            bytesSent = 0;
            bytesReceived = 0;
            pingsSent = 0;
            pongsSent = 0;
            pingsReceived = 0;
            pongsReceived = 0;
            connectionStartTime = std::chrono::steady_clock::now();
        }

        int64_t connectionDurationSecs() const
        {
            auto now = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::seconds>(
                now - connectionStartTime).count();
        }
    };
} // namespace ix
