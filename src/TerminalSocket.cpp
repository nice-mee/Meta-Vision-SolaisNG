//
// Created by liuzikai on 3/11/21.
//

#include "TerminalSocket.h"
#include <iostream>
#include <boost/bind.hpp>
#include <utility>

namespace meta {

TerminalSocketBase::TerminalSocketBase()
        : recvBuf(RECV_BUFFER_SIZE * 4, 0),
          ioThread(&TerminalSocketBase::ioThreadBody, this) {

}

TerminalSocketBase::~TerminalSocketBase() {
    ioContext.stop();

    // Terminate io_context thread
    ioThread.join();
}

bool TerminalSocketBase::sendSingleString(const string &name, const string &s) {
    if (!isConnected()) return false;

    auto buf = allocateBuffer(SINGLE_STRING, name, s.length() + 1);

    // The string itself
    buf->insert(buf->end(), s.begin(), s.end());
    buf->emplace_back('\0');

    // Send the data
    boost::asio::async_write(*socket,
                             boost::asio::buffer(*buf),
                             [this, buf](auto &error, auto numBytes) { handleSend(buf, error, numBytes); });

    return true;
}

bool TerminalSocketBase::sendSingleInt(const string &name, int32_t n) {
    if (!isConnected()) return false;

    auto buf = allocateBuffer(SINGLE_INT, name, 4);

    // The int itself
    emplaceInt32(*buf, (uint32_t) n);

    // Send the data
    boost::asio::async_write(*socket,
                             boost::asio::buffer(*buf),
                             [this, buf](auto &error, auto numBytes) { handleSend(buf, error, numBytes); });

    return true;
}

bool TerminalSocketBase::sendBytes(const string &name, uint8_t *data, size_t size) {
    if (!isConnected()) return false;

    auto buf = allocateBuffer(BYTES, name, size);

    // Data
    if (size != 0) {
        buf->resize(buf->size() + size);
        ::memcpy(buf->data() + (buf->size() - size), data, size);
    }

    // Send the data
    boost::asio::async_write(*socket,
                             boost::asio::buffer(*buf),
                             [this, buf](auto &error, auto numBytes) { handleSend(buf, error, numBytes); });

    return true;
}

bool TerminalSocketBase::sendListOfStrings(const string &name, const vector<string> &list) {
    if (!isConnected()) return false;

    // Iterate through strings to count the size
    size_t contentSize = 0;
    for (const auto &s : list) {
        contentSize += s.length() + 1;  // include the NUL
    }

    auto buf = allocateBuffer(LIST_OF_STRINGS, name, contentSize);

    // Emplace the strings
    for (const auto &s : list) {
        buf->insert(buf->end(), s.begin(), s.end());
        buf->emplace_back('\0');
    }

    // Send the data
    boost::asio::async_write(*socket,
                             boost::asio::buffer(*buf),
                             [this, buf](auto &error, auto numBytes) { handleSend(buf, error, numBytes); });

    return true;
}

std::shared_ptr<vector<uint8_t>>
TerminalSocketBase::allocateBuffer(PackageType type, const string &name, size_t contentSize) {
    auto buf = std::make_shared<vector<uint8_t>>();

    size_t bufSize = 1 + 1 + (name.length() + 1) + 4 + contentSize;
    buf->reserve(bufSize);

    // Preamble, 1 byte
    buf->emplace_back(PREAMBLE);

    // Type, 1 byte
    buf->emplace_back((uint8_t) type);

    // NUL-terminated name
    buf->insert(buf->end(), name.begin(), name.end());
    buf->emplace_back('\0');

    // Size, 4 bytes, little endian
    emplaceInt32(*buf, contentSize);

    return buf;
}

void TerminalSocketBase::emplaceInt32(vector<uint8_t> &buf, int32_t n) {
    // Little endian
    buf.emplace_back((uint8_t) (n & 0xFF));
    buf.emplace_back((uint8_t) ((n >> 8) & 0xFF));
    buf.emplace_back((uint8_t) ((n >> 16) & 0xFF));
    buf.emplace_back((uint8_t) ((n >> 24) & 0xFF));
}

void TerminalSocketBase::handleSend(std::shared_ptr<vector<uint8_t>> buf, const boost::system::error_code &error,
                                    size_t numBytes) {
    if (error == boost::asio::error::eof || error == boost::asio::error::connection_reset ||
        error == boost::asio::error::operation_aborted) {  // disconnected or socket released

        // Do nothing

    } else if (error) {
        std::cerr << "TerminalSocketBase: send error: " << error.message() << "\n";
    } else {
        assert(buf->size() == numBytes && "Unexpected # of bytes sent");
    }
}

void TerminalSocketBase::setupSocket(std::shared_ptr<tcp::socket> newSocket, DisconnectCallback disconnectCallback) {
    // As socket is replaced, previous socket will be released. Boost cleans things up well.
    // Here we just need to make sure current disconnectCallback is called before replacing with the new one

    // Setup socket
    socket = std::move(newSocket);

    // Start recv cycle
    recvState = RECV_PREAMBLE;
    recvOffset = 0;
    boost::asio::async_read(*socket,
                            boost::asio::buffer(&recvBuf[recvOffset], 1),
                            boost::asio::transfer_exactly(1),
                            [this, disconnectCallback](auto &error, auto numBytes) { handleRecv(error, numBytes, disconnectCallback); });
}

void TerminalSocketBase::closeSocket() {
    socket.reset();
}

void TerminalSocketBase::ioThreadBody() {
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> workGuard(ioContext.get_executor());
    ioContext.run();  // this operation is blocking, until ioContext is deleted in the class.
}

void TerminalSocketBase::handleRecv(const boost::system::error_code &error, size_t numBytes,
                                    DisconnectCallback disconnectCallback) {

    if (error == boost::asio::error::eof || error == boost::asio::error::connection_reset ||
        error == boost::asio::error::operation_aborted) {

        if (disconnectCallback) disconnectCallback(this);

        return;  // do not start next async_recv

    } else if (error) {

        std::cerr << "TerminalSocketBase: recv error: " << error.message() << "\n";
        // Continue to process data and start next async_recv
    }

    // Otherwise, process data and continue next async_recv

    /**
     * To avoid copying, strings and bytes are passed as const pointers, which however requires data to be continuous
     * in the memory.
     *
     * Here a vector is used as the buffer. All bytes are discarded at once if the end of the buffer is exactly the end
     * of one package. Before starting the next async_recv, the buffer is extended as needed. It's possible that the
     * buffer can grow large (if packages are always received partially), but if the data flow is not heavy, it may not
     * occur at all. An warning message is printed everytime the buffer is enlarged. Pay close attention to these
     * warnings.
     */


    ssize_t i = recvOffset;
    recvOffset += numBytes;  // move forward

    for (; i < recvOffset; i++) {
        switch (recvState) {
            case RECV_PREAMBLE:
                if (recvBuf[i] == PREAMBLE) {
                    recvState = RECV_PACKAGE_TYPE;  // transfer to receive package type
                }  // otherwise keep in RECV_PREAMBLE state
                break;
            case RECV_PACKAGE_TYPE:
                if (recvBuf[i] < PACKAGE_TYPE_COUNT) {  // valid
                    recvCurrentPackageType = (PackageType) recvBuf[i];
                    recvState = RECV_NAME;  // transfer to receiving name
                    recvNameStart = i + 1;
                } else {
                    std::cerr << "Received invalid package type " << recvBuf[i] << "\n";
                    recvState = RECV_PREAMBLE;
                }
                break;
            case RECV_NAME:
                if (recvBuf[i] == '\0') {
                    recvState = RECV_SIZE;   // transfer to receive 4-byte size
                    recvRemainingBytes = 4;  // for the 4 bytes of size
                }
                break;
            case RECV_SIZE:
                recvRemainingBytes--;
                if (recvRemainingBytes == 0) {
                    recvContentSize = decodeInt32(recvBuf.data() + (i - 3));
                    recvRemainingBytes = recvContentSize;
                    recvContentStart = i + 1;
                    if (recvContentSize > 0) {
                        recvState = RECV_CONTENT;  // transfer to receive content
                    } else {
                        // Empty content
                        handlePackage();
                        recvState = RECV_PREAMBLE;  // transfer to receive preamble
                        if (i + 1 == recvOffset) {  // happen to be at the end of buffer
                            recvOffset = 0;  // discard all received bytes
                        }
                    }
                }
                break;
            case RECV_CONTENT:
                recvRemainingBytes--;
                if (recvRemainingBytes == 0) {
                    handlePackage();

                    recvState = RECV_PREAMBLE;  // transfer to receive preamble

                    if (i + 1 == recvOffset) {  // happen to be at the end of buffer
                        recvOffset = 0;  // discard all received bytes
                    }
                }
                break;
        }
    }


    if (recvOffset + RECV_BUFFER_SIZE > recvBuf.size()) {
        std::cerr << "Warning: TerminalSocketBase enlarges its receiver buffer to 2x" << std::endl;
        recvBuf.resize(recvBuf.size() * 2);  // enlarge the buffer to 2x
    }

    boost::asio::async_read(*socket,
                            boost::asio::buffer(&recvBuf[recvOffset], RECV_BUFFER_SIZE),
                            boost::asio::transfer_at_least(1),
                            // boost::asio::transfer_all doesn't work
                            [this, disconnectCallback](auto &error, auto numBytes) { handleRecv(error, numBytes, disconnectCallback); });
}

void TerminalSocketBase::handlePackage() const {
    switch (recvCurrentPackageType) {
        case SINGLE_STRING:
            if (singleStringCallBack) {
                singleStringCallBack(callBackParam,
                                     (const char *) (recvBuf.data() + recvNameStart),
                                     (const char *) (recvBuf.data() + recvContentStart));
            }
            break;
        case SINGLE_INT:
            if (singleIntCallBack) {
                singleIntCallBack(callBackParam,
                                  (const char *) (recvBuf.data() + recvNameStart),
                                  decodeInt32(recvBuf.data() + recvContentStart));
            }
            break;
        case BYTES:
            if (bytesCallBack) {
                bytesCallBack(callBackParam,
                              (const char *) (recvBuf.data() + recvNameStart),
                              recvBuf.data() + recvContentStart,
                              recvContentSize);
            }
            break;
        case LIST_OF_STRINGS:
            if (listOfStringsCallBack) {
                vector<const char *> list;
                size_t strStart = recvContentStart;
                while (strStart < recvContentStart + recvContentSize) {
                    list.emplace_back((const char *) (recvBuf.data() + strStart));

                    // Find the end of the string
                    while (strStart < recvContentStart + recvContentSize && recvBuf[strStart] != '\0') {
                        strStart++;
                    }

                    // Move to the next start
                    strStart++;
                }
                listOfStringsCallBack(callBackParam,
                                      (char *) (recvBuf.data() + recvNameStart),
                                      list);
            }
            break;
        default:
            break;
    }
}

int32_t TerminalSocketBase::decodeInt32(const uint8_t *start) {
    // Little endian
    return (start[3] << 24) | (start[2] << 16) | (start[1] << 8) | start[0];
}

TerminalSocketServer::TerminalSocketServer(int port)
        : port(port),
          acceptor(ioContext, tcp::endpoint(tcp::v4(), port)) {

    acceptor.listen();
}

void TerminalSocketServer::startAccept(ServerDisconnectionCallback disconnectionCallback) {

    auto socket = std::make_shared<tcp::socket>(ioContext);
    // shared_ptr is used to manage socket. If the raw pointer is used and it doesn't reach handleAccept, memory leaks.

    // ioContext is running as TerminalSocketBase instantiates
    acceptor.async_accept(*socket,
                          [this, socket, disconnectionCallback](const auto &error) { handleAccept(socket, error, disconnectionCallback); });


    std::cout << "TerminalSocketServer: listen on " << port << std::endl;
}

void TerminalSocketServer::handleAccept(std::shared_ptr<tcp::socket> socket, const boost::system::error_code &error,
                                        ServerDisconnectionCallback disconnectionCallback) {
    if (!error) {
        std::cout << "TerminalSocketServer: get connection from " << socket->remote_endpoint().address().to_string()
                  << "\n";
        setupSocket(std::move(socket), reinterpret_cast<DisconnectCallback>(disconnectionCallback));
    } else if (error == boost::asio::error::operation_aborted) {
        // Ignore
    } else {
        std::cerr << "TerminalSocketServer: accept error " << error.message() << "\n";
    }
}

void TerminalSocketServer::disconnect() {
    if (isConnected()) {
        closeSocket();
    }
}

bool TerminalSocketClient::connect(const string &server, const string &port, ClientDisconnectionCallback disconnectCallback) {

    tcp::resolver::results_type endpoints = resolver.resolve(server, port);

    auto socket = std::make_shared<tcp::socket>(ioContext);
    // shared_ptr is used to manage socket. If the raw pointer is used and it doesn't reach handleAccept, memory leaks.

    boost::system::error_code err;

    boost::asio::connect(*socket, endpoints, err);

    if (!err) {
        setupSocket(socket, reinterpret_cast<DisconnectCallback>(disconnectCallback));
        std::cout << "TerminalSocketClient: connected to " << server << ":" << port << std::endl;
        return true;
    } else {
        std::cerr << "TerminalSocketClient: connection to " << server << ":" << port
                  << " failed:" << err.message() << std::endl;
        return false;
    }
}

void TerminalSocketClient::disconnect() {
    if (isConnected()) {
        closeSocket();
    }
}

}