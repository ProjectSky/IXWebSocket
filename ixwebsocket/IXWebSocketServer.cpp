/*
 *  IXWebSocketServer.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2018 Machine Zone, Inc. All rights reserved.
 */

#include "IXWebSocketServer.h"

#include "IXNetSystem.h"
#include <algorithm>
#include "IXSetThreadName.h"
#include "IXSocketConnect.h"
#include "IXWebSocket.h"
#include "IXWebSocketTransport.h"
#include <future>
#include <sstream>
#include <string.h>

namespace ix
{
    const int WebSocketServer::kDefaultHandShakeTimeoutSecs(5); // 5 seconds
    const bool WebSocketServer::kDefaultEnablePong(true);
    const int WebSocketServer::kPingIntervalSeconds(-1); // disable heartbeat

    WebSocketServer::WebSocketServer(int port,
                                     const std::string& host,
                                     int backlog,
                                     size_t maxConnections,
                                     int handshakeTimeoutSecs,
                                     int addressFamily,
                                     int pingIntervalSeconds)
        : SocketServer(port, host, backlog, maxConnections, addressFamily)
        , _handshakeTimeoutSecs(handshakeTimeoutSecs)
        , _enablePong(kDefaultEnablePong)
        , _enablePerMessageDeflate(true)
        , _pingIntervalSeconds(pingIntervalSeconds)
        , _maxConnectionsPerIp(0)
    {
    }

    WebSocketServer::~WebSocketServer()
    {
        stop();
    }

    void WebSocketServer::stop()
    {
        stopAcceptingConnections();

        auto clients = getClients();
        for (const auto& pair : clients)
        {
            pair.first->close();
        }

        SocketServer::stop();
    }

    void WebSocketServer::setPong(bool enabled)
    {
        _enablePong = enabled;
    }

    void WebSocketServer::setPerMessageDeflate(bool enabled)
    {
        _enablePerMessageDeflate = enabled;
    }

    void WebSocketServer::addSubProtocol(const std::string& subProtocol)
    {
        _subProtocols.push_back(subProtocol);
    }

    void WebSocketServer::clearSubProtocols()
    {
        _subProtocols.clear();
    }

    void WebSocketServer::removeSubProtocol(const std::string& subProtocol)
    {
        _subProtocols.erase(
            std::remove(_subProtocols.begin(), _subProtocols.end(), subProtocol),
            _subProtocols.end());
    }

    void WebSocketServer::setTimeouts(const WebSocketTimeouts& timeouts)
    {
        _timeouts = timeouts;
    }

    const WebSocketTimeouts& WebSocketServer::getTimeouts() const
    {
        return _timeouts;
    }

    void WebSocketServer::setMaxConnectionsPerIp(size_t maxConnections)
    {
        _maxConnectionsPerIp = maxConnections;
    }

    size_t WebSocketServer::getMaxConnectionsPerIp() const
    {
        return _maxConnectionsPerIp;
    }

    size_t WebSocketServer::getConnectionCountForIp(const std::string& ip)
    {
        std::lock_guard<std::mutex> lock(_rateLimitMutex);
        auto it = _connectionsPerIp.find(ip);
        return (it != _connectionsPerIp.end()) ? it->second : 0;
    }

    void WebSocketServer::setOnConnectionCallback(const OnConnectionCallback& callback)
    {
        _onConnectionCallback = callback;
    }

    void WebSocketServer::setOnClientMessageCallback(const OnClientMessageCallback& callback)
    {
        _onClientMessageCallback = callback;
    }

    void WebSocketServer::handleConnection(std::unique_ptr<Socket> socket,
                                           std::shared_ptr<ConnectionState> connectionState)
    {
        handleUpgrade(std::move(socket), connectionState);

        connectionState->setTerminated();
    }

    void WebSocketServer::handleUpgrade(std::unique_ptr<Socket> socket,
                                        std::shared_ptr<ConnectionState> connectionState,
                                        HttpRequestPtr request)
    {
        setThreadName("Srv:ws:" + connectionState->getId());

        std::string remoteIp = connectionState->getRemoteIp();

        // Track connections per IP and check rate limit
        {
            std::lock_guard<std::mutex> lock(_rateLimitMutex);
            if (_maxConnectionsPerIp > 0 && _connectionsPerIp[remoteIp] >= _maxConnectionsPerIp)
            {
                logError("Rate limit exceeded for IP: " + remoteIp);
                connectionState->setTerminated();
                return;
            }
            _connectionsPerIp[remoteIp]++;
        }

        auto webSocket = std::make_shared<WebSocket>();

        webSocket->setAutoThreadName(false);
        webSocket->setPingInterval(_pingIntervalSeconds);
        webSocket->setTimeouts(_timeouts);

        if (_onConnectionCallback)
        {
            _onConnectionCallback(webSocket, connectionState);

            if (!webSocket->isOnMessageCallbackRegistered())
            {
                logError("WebSocketServer Application developer error: Server callback improperly "
                         "registered.");
                logError("Missing call to setOnMessageCallback inside setOnConnectionCallback.");
                connectionState->setTerminated();
                return;
            }
        }
        else if (_onClientMessageCallback)
        {
            WebSocket* webSocketRawPtr = webSocket.get();
            webSocket->setOnMessageCallback(
                [this, webSocketRawPtr, connectionState](const WebSocketMessagePtr& msg)
                { _onClientMessageCallback(connectionState, *webSocketRawPtr, msg); });
        }
        else
        {
            logError(
                "WebSocketServer Application developer error: No server callback is registerered.");
            logError("Missing call to setOnConnectionCallback or setOnClientMessageCallback.");
            connectionState->setTerminated();
            return;
        }

        webSocket->setAutomaticReconnection(false);
        webSocket->setPong(_enablePong);

        // Add this client to our client map
        {
            std::lock_guard<std::mutex> lock(_clientsMutex);
            _clients[webSocket] = connectionState;
        }

        auto status = webSocket->connectToSocket(
            std::move(socket), _handshakeTimeoutSecs, _enablePerMessageDeflate, request, _subProtocols);
        if (status.success)
        {
            // Process incoming messages and execute callbacks
            // until the connection is closed
            webSocket->run();
        }
        else
        {
            std::stringstream ss;
            ss << "WebSocketServer::handleConnection() HTTP status: " << status.http_status
               << " error: " << status.errorStr;
            logError(ss.str());
        }

        webSocket->setOnMessageCallback(nullptr);

        // Remove this client from our client set
        {
            std::lock_guard<std::mutex> lock(_clientsMutex);
            if (_clients.erase(webSocket) != 1)
            {
                logError("Cannot delete client");
            }
        }

        // Decrement connection counter
        {
            std::lock_guard<std::mutex> lock(_rateLimitMutex);
            auto it = _connectionsPerIp.find(remoteIp);
            if (it != _connectionsPerIp.end() && it->second > 0)
            {
                if (--it->second == 0)
                {
                    _connectionsPerIp.erase(it);
                }
            }
        }
    }

    std::map<std::shared_ptr<WebSocket>, std::shared_ptr<ConnectionState>> WebSocketServer::getClients()
    {
        std::lock_guard<std::mutex> lock(_clientsMutex);
        return _clients;
    }

    std::shared_ptr<WebSocket> WebSocketServer::getClientById(const std::string& id)
    {
        std::lock_guard<std::mutex> lock(_clientsMutex);
        for (const auto& pair : _clients)
        {
            if (pair.second && pair.second->getId() == id)
            {
                return pair.first;
            }
        }
        return nullptr;
    }

    size_t WebSocketServer::getConnectedClientsCount()
    {
        std::lock_guard<std::mutex> lock(_clientsMutex);
        return _clients.size();
    }

    //
    // Classic servers
    //
    void WebSocketServer::makeBroadcastServer()
    {
        setOnClientMessageCallback(
            [this](std::shared_ptr<ConnectionState> connectionState,
                   WebSocket& webSocket,
                   const WebSocketMessagePtr& msg)
            {
                auto remoteIp = connectionState->getRemoteIp();
                if (msg->type == ix::WebSocketMessageType::Message)
                {
                    for (auto&& pair : getClients())
                    {
                        if (pair.first.get() != &webSocket)
                        {
                            pair.first->send(msg->str, msg->binary);

                            // Make sure the OS send buffer is flushed before moving on
                            do
                            {
                                std::chrono::duration<double, std::milli> duration(500);
                                std::this_thread::sleep_for(duration);
                            } while (pair.first->bufferedAmount() != 0);
                        }
                    }
                }
            });
    }

    void WebSocketServer::broadcast(const std::string& data, bool binary, WebSocket* exclude)
    {
        auto clients = getClients();
        for (const auto& pair : clients)
        {
            if (pair.first.get() != exclude)
            {
                pair.first->send(data, binary);
            }
        }
    }

    bool WebSocketServer::listenAndStart()
    {
        auto err = listen();
        if (err)
        {
            return false;
        }

        start();
        return true;
    }

    int WebSocketServer::getHandshakeTimeoutSecs()
    {
        return _handshakeTimeoutSecs;
    }

    void WebSocketServer::setHandshakeTimeoutSecs(int secs)
    {
        _handshakeTimeoutSecs = secs;
    }

    bool WebSocketServer::isPongEnabled()
    {
        return _enablePong;
    }

    bool WebSocketServer::isPerMessageDeflateEnabled()
    {
        return _enablePerMessageDeflate;
    }
} // namespace ix
