/*
 *  IXHttpServer.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2019 Machine Zone, Inc. All rights reserved.
 */

#include "IXHttpServer.h"

#include "IXGzipCodec.h"
#include "IXNetSystem.h"
#include "IXSocketConnect.h"
#include "IXUserAgent.h"
#include <charconv>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

namespace
{
    std::optional<std::vector<uint8_t>> load(const std::string& path)
    {
        std::ifstream file(path);
        if (!file.is_open()) return std::nullopt;

        file.seekg(0, file.end);
        std::streamoff size = file.tellg();
        file.seekg(0, file.beg);

        std::vector<uint8_t> memblock((size_t) size);
        file.read((char*) &memblock.front(), static_cast<std::streamsize>(size));

        return memblock;
    }

    std::optional<std::string> readAsString(const std::string& path)
    {
        auto res = load(path);
        if (!res) return std::nullopt;
        return std::string(res->begin(), res->end());
    }

    std::string response_head_file(const std::string& file_name){
        auto endsWith = [](const std::string& str, const std::string& suffix) {
            return str.size() >= suffix.size() &&
                   str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
        };

        if (endsWith(file_name, ".html") || endsWith(file_name, ".htm"))
            return "text/html";
        else if (endsWith(file_name, ".css"))
            return "text/css";
        else if (endsWith(file_name, ".js") || endsWith(file_name, ".mjs"))
            return "application/x-javascript";
        else if (endsWith(file_name, ".ico"))
            return "image/x-icon";
        else if (endsWith(file_name, ".png"))
            return "image/png";
        else if (endsWith(file_name, ".jpg") || endsWith(file_name, ".jpeg"))
            return "image/jpeg";
        else if (endsWith(file_name, ".gif"))
            return "image/gif";
        else if (endsWith(file_name, ".svg"))
            return "image/svg+xml";
        else
            return "application/octet-stream";
    }

} // namespace

namespace ix
{
    const int HttpServer::kDefaultTimeoutSecs(30);

    HttpServer::HttpServer(int port,
                           const std::string& host,
                           int backlog,
                           size_t maxConnections,
                           int addressFamily,
                           int timeoutSecs,
                           int handshakeTimeoutSecs)
        : WebSocketServer(port, host, backlog, maxConnections, handshakeTimeoutSecs, addressFamily)
        , _timeoutSecs(timeoutSecs)
    {
        setDefaultConnectionCallback();
    }

    void HttpServer::setOnConnectionCallback(const OnConnectionCallback& callback)
    {
        _onConnectionCallback = callback;
    }

    void HttpServer::handleConnection(std::unique_ptr<Socket> socket,
                                      std::shared_ptr<ConnectionState> connectionState)
    {
        auto [success, errorMsg, request] = Http::parseRequest(socket, _timeoutSecs);

        if (!success)
        {
            logError("HTTP request parsing failed: " + errorMsg);
            auto errorResponse = std::make_shared<HttpResponse>(
                400, "Bad Request", HttpErrorCode::HeaderParsingError,
                WebSocketHttpHeaders(), errorMsg);
            Http::sendResponse(errorResponse, socket);
            connectionState->setTerminated();
            return;
        }
        if (request->headers["Upgrade"] == "websocket")
        {
            WebSocketServer::handleUpgrade(std::move(socket), connectionState, request);
        }
        else
        {
            auto response = _onConnectionCallback(request, connectionState);
            if (!Http::sendResponse(response, socket))
            {
                logError("Cannot send response");
            }
        }
        connectionState->setTerminated();
    }

    void HttpServer::setDefaultConnectionCallback()
    {
        setOnConnectionCallback(
            [this](HttpRequestPtr request,
                   std::shared_ptr<ConnectionState> connectionState) -> HttpResponsePtr
            {
                std::string uri(request->uri);
                if (uri.empty() || uri == "/")
                {
                    uri = "/index.html";
                }

                WebSocketHttpHeaders headers;
                const std::string& customServer = getCustomServerHeader();
                headers["Server"] = customServer.empty() ? userAgent() : customServer;
                headers["Content-Type"] = response_head_file(uri);

                // CORS support
                auto origin = request->headers.find("Origin");
                if (origin != request->headers.end())
                {
                    headers["Access-Control-Allow-Origin"] = origin->second;
                    headers["Access-Control-Allow-Methods"] = "GET, POST, PUT, DELETE, OPTIONS";
                    headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization";
                    headers["Access-Control-Max-Age"] = "86400";
                }

                // Handle OPTIONS preflight
                if (request->method == "OPTIONS")
                {
                    return std::make_shared<HttpResponse>(
                        204, "No Content", HttpErrorCode::Ok, headers, std::string());
                }

                std::string path("." + uri);
                auto contentOpt = readAsString(path);
                if (!contentOpt)
                {
                    return std::make_shared<HttpResponse>(
                        404, "Not Found", HttpErrorCode::Ok, WebSocketHttpHeaders(), std::string());
                }
                std::string content = std::move(*contentOpt);

                // Generate ETag from content hash
                std::hash<std::string> hasher;
                size_t contentHash = hasher(content);
                std::stringstream etagStream;
                etagStream << "\"" << std::hex << contentHash << "\"";
                std::string etag = etagStream.str();
                headers["ETag"] = etag;

                // Check If-None-Match for ETag validation
                auto ifNoneMatch = request->headers.find("If-None-Match");
                if (ifNoneMatch != request->headers.end() && ifNoneMatch->second == etag)
                {
                    return std::make_shared<HttpResponse>(
                        304, "Not Modified", HttpErrorCode::Ok, headers, std::string());
                }

                // Handle Range requests
                auto rangeHeader = request->headers.find("Range");
                if (rangeHeader != request->headers.end())
                {
                    std::string rangeValue = rangeHeader->second;
                    if (rangeValue.find("bytes=") == 0)
                    {
                        rangeValue = rangeValue.substr(6);
                        size_t dashPos = rangeValue.find('-');
                        if (dashPos != std::string::npos)
                        {
                            size_t start = 0, end = content.size() - 1;
                            if (dashPos > 0)
                                std::from_chars(rangeValue.data(), rangeValue.data() + dashPos, start);
                            if (dashPos + 1 < rangeValue.size())
                                std::from_chars(rangeValue.data() + dashPos + 1, rangeValue.data() + rangeValue.size(), end);

                            if (start < content.size() && start <= end)
                            {
                                end = std::min(end, content.size() - 1);
                                std::string rangeContent = content.substr(start, end - start + 1);

                                std::stringstream rangeStream;
                                rangeStream << "bytes " << start << "-" << end << "/" << content.size();
                                headers["Content-Range"] = rangeStream.str();
                                headers["Accept-Ranges"] = "bytes";

                                return std::make_shared<HttpResponse>(
                                    206, "Partial Content", HttpErrorCode::Ok, headers, rangeContent);
                            }
                        }
                    }
                }

                headers["Accept-Ranges"] = "bytes";

#ifdef IXWEBSOCKET_USE_ZLIB
                std::string acceptEncoding = request->headers["Accept-encoding"];
                if (acceptEncoding == "*" || acceptEncoding.find("gzip") != std::string::npos)
                {
                    content = gzipCompress(content);
                    headers["Content-Encoding"] = "gzip";
                }
                headers["Accept-Encoding"] = "gzip";
#endif

                // Log request
                std::stringstream ss;
                ss << connectionState->getRemoteIp() << ":" << connectionState->getRemotePort()
                   << " " << request->method << " " << request->headers["User-Agent"] << " "
                   << request->uri << " " << content.size();
                logInfo(ss.str());


                return std::make_shared<HttpResponse>(
                    200, "OK", HttpErrorCode::Ok, headers, content);
            });
    }

    void HttpServer::makeRedirectServer(const std::string& redirectUrl)
    {
        //
        // See https://developer.mozilla.org/en-US/docs/Web/HTTP/Redirections
        //
        setOnConnectionCallback(
            [this, redirectUrl](HttpRequestPtr request,
                                std::shared_ptr<ConnectionState> connectionState) -> HttpResponsePtr
            {
                WebSocketHttpHeaders headers;
                const std::string& customServer = getCustomServerHeader();
                headers["Server"] = customServer.empty() ? userAgent() : customServer;

                // Log request
                std::stringstream ss;
                ss << connectionState->getRemoteIp() << ":" << connectionState->getRemotePort()
                   << " " << request->method << " " << request->headers["User-Agent"] << " "
                   << request->uri;
                logInfo(ss.str());

                if (request->method == "POST")
                {
                    return std::make_shared<HttpResponse>(
                        200, "OK", HttpErrorCode::Ok, headers, std::string());
                }

                headers["Location"] = redirectUrl;

                return std::make_shared<HttpResponse>(
                    301, "OK", HttpErrorCode::Ok, headers, std::string());
            });
    }

    //
    // Display the client parameter and body on the console
    //
    void HttpServer::makeDebugServer()
    {
        setOnConnectionCallback(
            [this](HttpRequestPtr request,
                   std::shared_ptr<ConnectionState> connectionState) -> HttpResponsePtr
            {
                WebSocketHttpHeaders headers;
                const std::string& customServer = getCustomServerHeader();
                headers["Server"] = customServer.empty() ? userAgent() : customServer;

                // Log request
                std::stringstream ss;
                ss << connectionState->getRemoteIp() << ":" << connectionState->getRemotePort()
                   << " " << request->method << " " << request->headers["User-Agent"] << " "
                   << request->uri;
                logInfo(ss.str());

                logInfo("== Headers == ");
                for (const auto& [name, value] : request->headers)
                {
                    std::ostringstream oss;
                    oss << name << ": " << value;
                    logInfo(oss.str());
                }
                logInfo("");

                logInfo("== Body == ");
                logInfo(request->body);
                logInfo("");

                return std::make_shared<HttpResponse>(
                    200, "OK", HttpErrorCode::Ok, headers, std::string("OK"));
            });
    }

    int HttpServer::getTimeoutSecs()
    {
        return _timeoutSecs;
    }

    void HttpServer::setTimeoutSecs(int secs)
    {
        _timeoutSecs = secs;
    }

} // namespace ix
