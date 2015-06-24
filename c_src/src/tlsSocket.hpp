/**
 * @file tlsSocket.hpp
 * @author Konrad Zemek
 * @copyright (C) 2015 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#ifndef ONE_ETLS_TLS_SOCKET_HPP
#define ONE_ETLS_TLS_SOCKET_HPP

#include "commonDefs.hpp"

#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/strand.hpp>

#include <memory>
#include <string>
#include <vector>

namespace one {
namespace etls {

/**
 * The @c TLSSocket class is responsible for handling a single TLS socket.
 */
class TLSSocket {
    friend class TLSAcceptor;

public:
    /**
     * A shortcut alias for frequent usage.
     */
    using Ptr = std::shared_ptr<TLSSocket>;

    /**
     * Constructor.
     * Prepares a new @c boost::asio socket with a local @c ssl::context.
     * @param ioService @c io_service object to use for this object's
     * asynchronous operations.
     */
    TLSSocket(boost::asio::io_service &ioService);

    /**
     * Constructor.
     * Prepares a new @c boost::asio socket with a given @c ssl::context.
     * @param context a @c ssl::context to use for this socket's configuration.
     * @param ioService @c io_service object to use for this object's
     * asynchronous operations.
     */
    TLSSocket(
        boost::asio::io_service &ioService, boost::asio::ssl::context &context);

    /**
     * Asynchronously connects the socket to a remote service.
     * Calls success callback with @c self.
     * @param self Shared pointer to this.
     * @param host Host to connect to.
     * @param port TCP port to connect to.
     * @param success Callback function to call on success.
     * @param error Callback function to call on error.
     */
    void connectAsync(Ptr self, std::string host, const unsigned short port,
        SuccessFun<Ptr> success, ErrorFun error);

    /**
     * Asynchronously sends a message through the socket.
     * @param self Shared pointer to this.
     * @param buffer The message to send.
     * @param success Callback function to call on success.
     * @param error Callback function to call on error.
     */
    void sendAsync(Ptr self, boost::asio::const_buffer buffer,
        SuccessFun<> success, ErrorFun error);

    /**
     * Asynchronously receives a message from the socket.
     * Calls success callback with @c buffer. Either all of the buffer will
     * be filled with received data, or error will be called.
     * @param self Shared pointer to this.
     * @param buffer Buffer to save the received message to.
     * @param success Callback function to call on success.
     * @param error Callback function to call on error.
     */
    void recvAsync(Ptr self, boost::asio::mutable_buffer buffer,
        SuccessFun<boost::asio::mutable_buffer> success, ErrorFun error);

    /**
     * Asynchronously receive a message from the socket.
     * Calls success callback with a given @c buffer resized to the size of
     * received message. Consequently, @c recvAnyAsync returns with success
     * after any data is received.
     * @param self Shared pointer to this.
     * @param buffer Buffer to save the received message to.
     * @param success Callback function to call on success.
     * @param error Callback function to call on error.
     */
    void recvAnyAsync(Ptr self, boost::asio::mutable_buffer buffer,
        SuccessFun<boost::asio::mutable_buffer> success, ErrorFun error);

    /**
     * Asynchronously perform a handshake for an incoming connection.
     * @param self Shared pointer to this.
     * @param success Callback function to call on success.
     * @param error Callback function to call on error.
     */
    void handshakeAsync(Ptr self, SuccessFun<> success, ErrorFun error);

    /**
     * Asynchronously shutdown the TCP connection on the socket.
     * @param self Shared pointer to this.
     * @param type Type of the shutdown (read, write or both).
     * @param success Callback function to call on success.
     * @param error Callback function to call on error.
     */
    void shutdownAsync(Ptr self,
        const boost::asio::socket_base::shutdown_type type,
        SuccessFun<> success, ErrorFun error);

    /**
     * Asynchronously retrieve the local endpoint information.
     * Calls success callback with the retrieved endpoint.
     * @param self Shared pointer to this.
     * @param success Callback function to call on success.
     * @param error Callback function to call on error.
     */
    void localEndpointAsync(Ptr self,
        SuccessFun<const boost::asio::ip::tcp::endpoint &> success,
        ErrorFun error);

    /**
     * Asynchronously retrieve the remote endpoint information.
     * Calls success callback with the retrieved endpoint.
     * @param self Shared pointer to this.
     * @param success Callback function to call on success.
     * @param error Callback function to call on error.
     */
    void remoteEndpointAsync(Ptr self,
        SuccessFun<const boost::asio::ip::tcp::endpoint &> success,
        ErrorFun error);

    /**
     * @returns A DER-encoded list of certificates that form peer's certificate
     * chain.
     */
    const std::vector<std::vector<unsigned char>> &certificateChain() const;

    /**
     * Asynchronously close the socket.
     * @param self Shared pointer to this.
     * @param success Callback function to call on success.
     * @param error Callback function to call on error.
     */
    void closeAsync(Ptr self, SuccessFun<> success, ErrorFun error);

private:
    std::vector<boost::asio::ip::basic_resolver_entry<boost::asio::ip::tcp>>
    shuffleEndpoints(boost::asio::ip::tcp::resolver::iterator iterator);

    bool saveCertificate(bool, boost::asio::ssl::verify_context &ctx);

    boost::asio::ssl::context m_clientContext{
        boost::asio::ssl::context::tlsv12_client};

    boost::asio::io_service &m_ioService;
    boost::asio::ip::tcp::resolver m_resolver{m_ioService};
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> m_socket;
    std::vector<std::vector<unsigned char>> m_certificateChain;
};

} // namespace etls
} // namespace one

#endif // ONE_ETLS_TLS_SOCKET_HPP
