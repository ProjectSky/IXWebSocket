/*
 *  IXWebSocketTimeouts.h
 *  Author: ProjectSky
 *  Copyright (c) 2024 Machine Zone, Inc. All rights reserved.
 */

#pragma once

namespace ix
{
    struct WebSocketTimeouts
    {
        int pingIntervalSecs = -1;      // -1 means disabled
        int pingTimeoutSecs = -1;       // -1 means disabled
        int idleTimeoutSecs = -1;       // -1 means disabled
        int sendTimeoutSecs = 300;
        int closeTimeoutSecs = 5;

        WebSocketTimeouts() = default;

        WebSocketTimeouts& setPingInterval(int secs)
        {
            pingIntervalSecs = secs;
            return *this;
        }

        WebSocketTimeouts& setPingTimeout(int secs)
        {
            pingTimeoutSecs = secs;
            return *this;
        }

        WebSocketTimeouts& setIdleTimeout(int secs)
        {
            idleTimeoutSecs = secs;
            return *this;
        }

        WebSocketTimeouts& setSendTimeout(int secs)
        {
            sendTimeoutSecs = secs;
            return *this;
        }

        WebSocketTimeouts& setCloseTimeout(int secs)
        {
            closeTimeoutSecs = secs;
            return *this;
        }
    };
} // namespace ix
