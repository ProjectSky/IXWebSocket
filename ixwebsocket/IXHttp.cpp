/*
 *  IXHttp.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2019 Machine Zone, Inc. All rights reserved.
 */

#include "IXHttp.h"

#include "IXCancellationRequest.h"
#include "IXGzipCodec.h"
#include "IXSocket.h"
#include <sstream>
#include <vector>

namespace ix
{
    std::string Http::trim(const std::string& str)
    {
        std::string out;
        out.reserve(str.size());
        for (char c : str)
        {
            if (c != ' ' && c != '\n' && c != '\r')
            {
                out += c;
            }
        }

        return out;
    }

    std::pair<std::string, int> Http::parseStatusLine(const std::string& line)
    {
        // Request-Line   = Method SP Request-URI SP HTTP-Version CRLF
        std::string token;
        std::stringstream tokenStream(line);
        std::vector<std::string> tokens;

        // Split by ' '
        while (std::getline(tokenStream, token, ' '))
        {
            tokens.push_back(token);
        }

        std::string httpVersion;
        if (tokens.size() >= 1)
        {
            httpVersion = trim(tokens[0]);
        }

        int statusCode = -1;
        if (tokens.size() >= 2)
        {
            std::stringstream ss;
            ss << trim(tokens[1]);
            ss >> statusCode;
        }

        return std::make_pair(httpVersion, statusCode);
    }

    std::tuple<std::string, std::string, std::string> Http::parseRequestLine(
        const std::string& line)
    {
        // Request-Line   = Method SP Request-URI SP HTTP-Version CRLF
        std::string token;
        std::stringstream tokenStream(line);
        std::vector<std::string> tokens;

        // Split by ' '
        while (std::getline(tokenStream, token, ' '))
        {
            tokens.push_back(token);
        }

        std::string method;
        if (tokens.size() >= 1)
        {
            method = trim(tokens[0]);
        }

        std::string requestUri;
        if (tokens.size() >= 2)
        {
            requestUri = trim(tokens[1]);
        }

        std::string httpVersion;
        if (tokens.size() >= 3)
        {
            httpVersion = trim(tokens[2]);
        }

        return std::make_tuple(method, requestUri, httpVersion);
    }

    std::tuple<bool, std::string, HttpRequestPtr> Http::parseRequest(
        std::unique_ptr<Socket>& socket, int timeoutSecs)
    {
        HttpRequestPtr httpRequest;

        std::atomic<bool> requestInitCancellation(false);

        auto isCancellationRequested =
            makeCancellationRequestWithTimeout(timeoutSecs, requestInitCancellation);

        // Read first line
        auto line = socket->readLine(isCancellationRequested);
        if (!line)
        {
            return std::make_tuple(false, "Error reading HTTP request line", httpRequest);
        }

        // Parse request line (GET /foo HTTP/1.1\r\n)
        auto [method, uri, httpVersion] = Http::parseRequestLine(*line);

        // Retrieve and validate HTTP headers
        auto headersOpt = parseHttpHeaders(socket, isCancellationRequested);
        if (!headersOpt)
        {
            return std::make_tuple(false, "Error parsing HTTP headers", httpRequest);
        }
        auto headers = std::move(*headersOpt);

        std::string body;
        if (headers.find("Content-Length") != headers.end())
        {
            int contentLength = 0;
            {
                const char* p = headers["Content-Length"].c_str();
                char* p_end {};
                errno = 0;
                long val = std::strtol(p, &p_end, 10);
                if (p_end == p         // invalid argument
                    || errno == ERANGE // out of range
                )
                {
                    return std::make_tuple(
                        false, "Error parsing HTTP Header 'Content-Length'", httpRequest);
                }
                if (val > std::numeric_limits<int>::max())
                {
                    return std::make_tuple(
                        false, "Error: 'Content-Length' value was above max", httpRequest);
                }
                if (val < std::numeric_limits<int>::min())
                {
                    return std::make_tuple(
                        false, "Error: 'Content-Length' value was below min", httpRequest);
                }
                contentLength = static_cast<int>(val);
            }
            if (contentLength < 0)
            {
                return std::make_tuple(
                    false, "Error: 'Content-Length' should be a positive integer", httpRequest);
            }

            auto res = socket->readBytes(contentLength, nullptr, nullptr, isCancellationRequested);
            if (!res)
            {
                return std::make_tuple(
                    false, std::string("Error reading request body"), httpRequest);
            }
            body = std::move(*res);
        }

        // If the content was compressed with gzip, decode it
        if (headers["Content-Encoding"] == "gzip")
        {
#ifdef IXWEBSOCKET_USE_ZLIB
            std::string decompressedPayload;
            if (!gzipDecompress(body, decompressedPayload))
            {
                return std::make_tuple(
                    false, std::string("Error during gzip decompression of the body"), httpRequest);
            }
            body = decompressedPayload;
#else
            std::string errorMsg("ixwebsocket was not compiled with gzip support on");
            return std::make_tuple(false, errorMsg, httpRequest);
#endif
        }

        httpRequest = std::make_shared<HttpRequest>(uri, method, httpVersion, body, headers);
        return std::make_tuple(true, "", httpRequest);
    }

    bool Http::sendResponse(HttpResponsePtr response, std::unique_ptr<Socket>& socket)
    {
        // Write the response to the socket
        std::stringstream ss;
        ss << "HTTP/1.1 ";
        ss << response->statusCode;
        ss << " ";
        ss << response->description;
        ss << "\r\n";

        if (!socket->writeBytes(ss.str(), nullptr))
        {
            return false;
        }

        // Check if chunked encoding should be used
        bool useChunked = response->headers.find("Transfer-Encoding") != response->headers.end() &&
                          response->headers.at("Transfer-Encoding") == "chunked";

        // Write headers
        ss.str("");
        if (!useChunked)
        {
            ss << "Content-Length: " << response->body.size() << "\r\n";
        }
        for (auto&& it : response->headers)
        {
            ss << it.first << ": " << it.second << "\r\n";
        }
        ss << "\r\n";

        if (!socket->writeBytes(ss.str(), nullptr))
        {
            return false;
        }

        // Send body
        if (response->body.empty())
        {
            return true;
        }

        if (useChunked)
        {
            // Send as chunked
            ss.str("");
            ss << std::hex << response->body.size() << "\r\n";
            if (!socket->writeBytes(ss.str(), nullptr))
                return false;
            if (!socket->writeBytes(response->body, nullptr))
                return false;
            if (!socket->writeBytes("\r\n0\r\n\r\n", nullptr))
                return false;
            return true;
        }

        return socket->writeBytes(response->body, nullptr);
    }
} // namespace ix
