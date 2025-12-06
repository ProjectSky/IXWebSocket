/*
 *  IXSocket.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2017-2018 Machine Zone, Inc. All rights reserved.
 */

#include "IXSocket.h"

#include "IXNetSystem.h"
#include "IXProxyConnect.h"
#include "IXSelectInterrupt.h"
#include "IXSelectInterruptFactory.h"
#include "IXSocketConnect.h"
#include <algorithm>
#include <array>
#include <optional>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <vector>

#ifdef min
#undef min
#endif

namespace ix
{
    const int Socket::kDefaultPollNoTimeout = -1; // No poll timeout by default
    const int Socket::kDefaultPollTimeout = kDefaultPollNoTimeout;

    Socket::Socket(int fd)
        : _sockfd(fd)
        , _selectInterrupt(createSelectInterrupt())
    {
        ;
    }

    Socket::~Socket()
    {
        close();
    }

    PollResultType Socket::poll(bool readyToRead,
                                int timeoutMs,
                                int sockfd,
                                const SelectInterruptPtr& selectInterrupt)
    {
        PollResultType pollResult = PollResultType::ReadyForRead;

        //
        // We used to use ::select to poll but on Android 9 we get large fds out of
        // ::connect which crash in FD_SET as they are larger than FD_SETSIZE. Switching
        // to ::poll does fix that.
        //
        // However poll isn't as portable as select and has bugs on Windows, so we
        // have a shim to fallback to select on those platforms. See
        // https://github.com/mpv-player/mpv/pull/5203/files for such a select wrapper.
        //
        nfds_t nfds = 1;
        struct pollfd fds[2]{};

        fds[0].fd = sockfd;
        fds[0].events = (readyToRead) ? POLLIN : POLLOUT;

        // this is ignored by poll, but our select based poll wrapper on Windows needs it
        fds[0].events |= POLLERR;

        // File descriptor used to interrupt select when needed
        int interruptFd = -1;
        void* interruptEvent = nullptr;
        if (selectInterrupt)
        {
            interruptFd = selectInterrupt->getFd();
            interruptEvent = selectInterrupt->getEvent();

            if (interruptFd != -1)
            {
                nfds = 2;
                fds[1].fd = interruptFd;
                fds[1].events = POLLIN;
            }
            else if (interruptEvent == nullptr)
            {
                // Emulation mode: SelectInterrupt neither supports file descriptors nor events

                // Check the selectInterrupt for requests before doing the poll().
                if (readSelectInterruptRequest(selectInterrupt, &pollResult))
                {
                    return pollResult;
                }
            }
        }

        void* event = interruptEvent; // ix::poll will set event to nullptr if it wasn't signaled
        int ret = ix::poll(fds, nfds, timeoutMs, &event);

        if (ret < 0)
        {
            pollResult = PollResultType::Error;
        }
        else if (ret == 0)
        {
            pollResult = PollResultType::Timeout;
            if (selectInterrupt && interruptFd == -1 && interruptEvent == nullptr)
            {
                // Emulation mode: SelectInterrupt neither supports fd nor events

                // Check the selectInterrupt for requests
                readSelectInterruptRequest(selectInterrupt, &pollResult);
            }
        }
        else if ((interruptFd != -1 && fds[1].revents & POLLIN) || (interruptEvent != nullptr && event != nullptr))
        {
            // The InterruptEvent was signaled
            readSelectInterruptRequest(selectInterrupt, &pollResult);
        }
        else if (sockfd != -1 && readyToRead && fds[0].revents & POLLIN)
        {
            pollResult = PollResultType::ReadyForRead;
        }
        else if (sockfd != -1 && !readyToRead && fds[0].revents & POLLOUT)
        {
            pollResult = PollResultType::ReadyForWrite;

#ifdef _WIN32
            // On connect error, in async mode, windows will write to the exceptions fds
            if (fds[0].revents & POLLERR)
            {
                pollResult = PollResultType::Error;
            }
#else
            int optval = -1;
            socklen_t optlen = sizeof(optval);

            // getsockopt() puts the errno value for connect into optval so 0
            // means no-error.
            if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &optval, &optlen) == -1 || optval != 0)
            {
                pollResult = PollResultType::Error;

                // set errno to optval so that external callers can have an
                // appropriate error description when calling strerror
                errno = optval;
            }
#endif
        }
        else if (sockfd != -1 && (fds[0].revents & POLLERR || fds[0].revents & POLLHUP ||
                                  fds[0].revents & POLLNVAL))
        {
            pollResult = PollResultType::Error;
        }

        return pollResult;
    }

    bool Socket::readSelectInterruptRequest(const SelectInterruptPtr& selectInterrupt,
                                            PollResultType* pollResult)
    {
        auto value = selectInterrupt->read();
        if (!value) return false;

        if (*value == SelectInterrupt::kSendRequest)
        {
            *pollResult = PollResultType::SendRequest;
            return true;
        }
        else if (*value == SelectInterrupt::kCloseRequest)
        {
            *pollResult = PollResultType::CloseRequest;
            return true;
        }

        return false;
    }

    PollResultType Socket::isReadyToRead(int timeoutMs)
    {
        if (_sockfd == -1)
        {
            return PollResultType::Error;
        }

        bool readyToRead = true;
        return poll(readyToRead, timeoutMs, _sockfd, _selectInterrupt);
    }

    PollResultType Socket::isReadyToWrite(int timeoutMs)
    {
        if (_sockfd == -1)
        {
            return PollResultType::Error;
        }

        bool readyToRead = false;
        return poll(readyToRead, timeoutMs, _sockfd, _selectInterrupt);
    }

    // Wake up from poll/select by writing to the pipe which is watched by select
    bool Socket::wakeUpFromPoll(uint64_t wakeUpCode)
    {
        return _selectInterrupt->notify(wakeUpCode);
    }

    bool Socket::isWakeUpFromPollSupported()
    {
        return _selectInterrupt->getFd() != -1 || _selectInterrupt->getEvent() != nullptr;
    }

    bool Socket::accept(std::string& errMsg)
    {
        if (_sockfd == -1)
        {
            errMsg = "Socket is uninitialized";
            return false;
        }
        return true;
    }

    bool Socket::connect(const std::string& host,
                         int port,
                         std::string& errMsg,
                         const CancellationRequest& isCancellationRequested)
    {
        std::lock_guard<std::mutex> lock(_socketMutex);

        if (!_selectInterrupt->clear()) return false;

        if (_proxyConfig.isEnabled())
        {
            return connectThroughProxy(host, port, errMsg, isCancellationRequested);
        }

        _sockfd = SocketConnect::connect(host, port, errMsg, isCancellationRequested);
        return _sockfd != -1;
    }

    bool Socket::connectThroughProxy(const std::string& host,
                                     int port,
                                     std::string& errMsg,
                                     const CancellationRequest& isCancellationRequested)
    {
        _sockfd = SocketConnect::connect(_proxyConfig.host, _proxyConfig.port,
                                         errMsg, isCancellationRequested);
        if (_sockfd == -1) return false;

        if (!ProxyConnect::connect(_sockfd, _proxyConfig, host, port,
                                   errMsg, isCancellationRequested))
        {
            close();
            return false;
        }
        return true;
    }

    void Socket::setProxyConfig(const ProxyConfig& proxyConfig)
    {
        _proxyConfig = proxyConfig;
    }

    const ProxyConfig& Socket::getProxyConfig() const
    {
        return _proxyConfig;
    }

    void Socket::close()
    {
        std::lock_guard<std::mutex> lock(_socketMutex);

        if (_sockfd == -1) return;

        closeSocket(_sockfd);
        _sockfd = -1;
    }

    bool Socket::isOpen() const
    {
        return _sockfd != -1;
    }

    IoResult Socket::send(const char* buffer, size_t length)
    {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif

        auto ret = ::send(_sockfd, buffer, length, flags);
        if (ret > 0) return {static_cast<size_t>(ret), IoError::Success};
        if (ret == 0) return {0, IoError::ConnectionClosed};
        if (isWaitNeeded()) return {0, IoError::WouldBlock};
        return {0, IoError::Error};
    }

    IoResult Socket::send(const std::string& buffer)
    {
        return send(buffer.data(), buffer.size());
    }

    IoResult Socket::recv(void* buffer, size_t length)
    {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif

        auto ret = ::recv(_sockfd, (char*) buffer, length, flags);
        if (ret > 0) return {static_cast<size_t>(ret), IoError::Success};
        if (ret == 0) return {0, IoError::ConnectionClosed};
        if (isWaitNeeded()) return {0, IoError::WouldBlock};
        return {0, IoError::Error};
    }

    int Socket::getErrno()
    {
        int err;

#ifdef _WIN32
        err = WSAGetLastError();
#else
        err = errno;
#endif

        return err;
    }

    bool Socket::isWaitNeeded()
    {
        int err = getErrno();

        if (err == EWOULDBLOCK || err == EAGAIN || err == EINPROGRESS)
        {
            return true;
        }

        return false;
    }

    void Socket::closeSocket(int fd)
    {
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
    }

    bool Socket::init(std::string& errorMsg)
    {
        return _selectInterrupt->init(errorMsg);
    }

    bool Socket::writeBytes(const std::string& str,
                            const CancellationRequest& isCancellationRequested)
    {
        size_t offset = 0;
        size_t len = str.size();

        while (true)
        {
            if (isCancellationRequested && isCancellationRequested()) return false;

            auto result = send((char*) &str[offset], len);

            if (result)
            {
                if (result.bytes == len) return true;
                offset += result.bytes;
                len -= result.bytes;
                continue;
            }
            if (result.wouldBlock())
            {
                if (isReadyToWrite(1) == PollResultType::Error) return false;
                continue;
            }
            return false;
        }
    }

    bool Socket::readByte(void* buffer, const CancellationRequest& isCancellationRequested)
    {
        while (true)
        {
            if (isCancellationRequested && isCancellationRequested()) return false;

            auto result = recv(buffer, 1);

            if (result && result.bytes == 1) return true;
            if (result.wouldBlock())
            {
                if (isReadyToRead(1) == PollResultType::Error) return false;
                continue;
            }
            return false;
        }
    }

    std::optional<std::string> Socket::readLine(
        const CancellationRequest& isCancellationRequested)
    {
        constexpr size_t maxLineLength = 8192;
        char c;
        std::string line;
        line.reserve(64);

        while (line.size() < maxLineLength)
        {
            if (!readByte(&c, isCancellationRequested))
            {
                return std::nullopt;
            }

            line += c;
            const size_t currentSize = line.size();

            if (currentSize >= 2 && line[currentSize - 2] == '\r' && line[currentSize - 1] == '\n')
            {
                return line;
            }
        }

        return std::nullopt;
    }

    std::optional<std::string> Socket::readBytes(
        size_t length,
        const OnProgressCallback& onProgressCallback,
        const OnChunkCallback& onChunkCallback,
        const CancellationRequest& isCancellationRequested)
    {
        std::array<uint8_t, 1 << 14> readBuffer;
        std::vector<uint8_t> output;
        if (!onChunkCallback)
        {
            output.reserve(length);
        }
        size_t bytesRead = 0;

        while (bytesRead != length)
        {
            if (isCancellationRequested && isCancellationRequested())
            {
                return std::nullopt;
            }

            size_t size = std::min(readBuffer.size(), length - bytesRead);
            auto result = recv((char*) &readBuffer[0], size);

            if (result)
            {
                if (onChunkCallback)
                {
                    std::string chunk(readBuffer.begin(), readBuffer.begin() + result.bytes);
                    onChunkCallback(chunk);
                }
                else
                {
                    output.insert(output.end(), readBuffer.begin(), readBuffer.begin() + result.bytes);
                }
                bytesRead += result.bytes;

                if (onProgressCallback) onProgressCallback((int) bytesRead, (int) length);
            }
            else if (result.wouldBlock())
            {
                if (isReadyToRead(1) == PollResultType::Error) return std::nullopt;
            }
            else
            {
                return std::nullopt;
            }
        }

        return std::string(output.begin(), output.end());
    }
} // namespace ix
