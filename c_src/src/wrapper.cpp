/**
 * @file wrapper.cpp
 * @author Konrad Zemek
 * @copyright (C) 2015 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#include "commonDefs.hpp"
#include "nifpp.h"
#include "tlsAcceptor.hpp"
#include "tlsApplication.hpp"
#include "tlsSocket.hpp"

#include <asio/error.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>
#include <unordered_map>

using namespace std::literals;

/**
 * A shared pointer wrapper to help with Erlang NIF environment management.
 */
class Env {
public:
    /**
     * Creates a new environment by creating a @c shared_ptr with a custom
     * deleter.
     */
    Env()
        : env{enif_alloc_env(), enif_free_env}
    {
    }

    /**
     * Implicit conversion operator to @cErlNifEnv* .
     */
    operator ErlNifEnv *() { return env.get(); }

private:
    std::shared_ptr<ErlNifEnv> env;
};

namespace {

/**
 * @defgroup StaticAtoms Statically created atoms for ease of usage.
 * @{
 */
nifpp::str_atom ok{"ok"};
nifpp::str_atom error{"error"};
/** @} */

/**
 * The @c TLSApplication object has a static lifetime; it will live as long
 * as the shared library is loaded into the memory and can be simultaneously
 * used by multiple multi-threaded applications.
 * The object has no external dependencies, including any lifetime dependencies.
 */
one::etls::TLSApplication app;

/**
 * Creates an error callback.
 * An error callback implementation is shared between multiple NIF functions.
 * @param localEnv A local NIF environment.
 * @param pid A PID to which an error message will be sent.
 * @return An instance of created error callback for the parameters.
 */
one::etls::ErrorFun onError(Env localEnv, ErlNifPid pid)
{
    return [localEnv, pid](const std::error_code &ec) mutable {
        auto reason = nifpp::str_atom{ec.message()};
        auto message = nifpp::make(localEnv, std::make_tuple(error, reason));
        enif_send(nullptr, &pid, localEnv, message);
    };
}

/**
 * @copydoc onError(Env, ErlNifPid)
 * @param ref A custom Erlang reference for this call.
 */
one::etls::ErrorFun onError(Env localEnv, ErlNifPid pid, nifpp::TERM ref)
{
    return [localEnv, pid, ref](const std::error_code &ec) mutable {
        auto reason = nifpp::str_atom{ec.message()};
        auto message = nifpp::make(
            localEnv, std::make_tuple(ref, std::make_tuple(error, reason)));

        enif_send(nullptr, &pid, localEnv, message);
    };
}

template <typename... Args, std::size_t... I>
ERL_NIF_TERM wrap_helper(
    ERL_NIF_TERM (*fun)(ErlNifEnv *, Env, ErlNifPid, Args...), ErlNifEnv *env,
    const ERL_NIF_TERM args[], std::index_sequence<I...>)
{
    try {
        ErlNifPid pid;
        enif_self(env, &pid);
        return fun(env, Env{}, pid, nifpp::get<Args>(env, args[I])...);
    }
    catch (const nifpp::badarg &) {
        return enif_make_badarg(env);
    }
    catch (const std::system_error &e) {
        return nifpp::make(
            env, std::make_tuple(error, nifpp::str_atom{e.code().message()}));
    }
    catch (const std::exception &e) {
        return nifpp::make(env, std::make_tuple(error, std::string{e.what()}));
    }
}

template <typename... Args>
ERL_NIF_TERM wrap(ERL_NIF_TERM (*fun)(ErlNifEnv *, Env, ErlNifPid, Args...),
    ErlNifEnv *env, const ERL_NIF_TERM args[])
{
    return wrap_helper(fun, env, args, std::index_sequence_for<Args...>{});
}

ERL_NIF_TERM connect(ErlNifEnv *env, Env localEnv, ErlNifPid pid, nifpp::TERM r,
    std::string host, int port)
{
    nifpp::TERM ref{enif_make_copy(localEnv, r)};

    auto onSuccess = [=](one::etls::TLSSocket::Ptr socket) mutable {
        auto resource =
            nifpp::construct_resource<one::etls::TLSSocket::Ptr>(socket);

        auto message = nifpp::make(localEnv,
            std::make_tuple(ref, std::make_tuple(
                                     ok, nifpp::make(localEnv, resource))));

        enif_send(nullptr, &pid, localEnv, message);
    };

    auto sock = std::make_shared<one::etls::TLSSocket>(app.ioService());
    sock->connectAsync(
        sock, host, port, std::move(onSuccess), onError(localEnv, pid, ref));

    return nifpp::make(env, ok);
}

ERL_NIF_TERM send(ErlNifEnv *env, Env localEnv, ErlNifPid pid,
    one::etls::TLSSocket::Ptr sock, nifpp::TERM d)
{
    nifpp::TERM data{enif_make_copy(localEnv, d)};

    ErlNifBinary bin;
    enif_inspect_iolist_as_binary(localEnv, data, &bin);

    asio::const_buffer buffer{bin.data, bin.size};

    auto onSuccess = [=]() mutable {
        auto message = nifpp::make(localEnv, ok);
        enif_send(nullptr, &pid, localEnv, message);
    };

    sock->sendAsync(sock, buffer, std::move(onSuccess), onError(localEnv, pid));

    return nifpp::make(env, ok);
}

ERL_NIF_TERM recv(ErlNifEnv *env, Env localEnv, ErlNifPid pid,
    one::etls::TLSSocket::Ptr sock, std::size_t size)
{
    auto bin = std::make_shared<nifpp::binary>(size == 0 ? 10 * 1024 : size);

    auto onSuccess = [=](asio::mutable_buffer buffer) mutable {
        if (bin->size != asio::buffer_size(buffer))
            enif_realloc_binary(bin.get(), asio::buffer_size(buffer));

        auto message = nifpp::make(
            localEnv, std::make_tuple(ok, nifpp::make(localEnv, *bin)));

        enif_send(nullptr, &pid, localEnv, message);
    };

    asio::mutable_buffer buffer{bin->data, bin->size};
    if (size == 0) {
        sock->recvAnyAsync(
            sock, buffer, std::move(onSuccess), onError(localEnv, pid));
    }
    else {
        sock->recvAsync(
            sock, buffer, std::move(onSuccess), onError(localEnv, pid));
    }

    return nifpp::make(env, ok);
}

ERL_NIF_TERM listen(ErlNifEnv *env, Env /*localEnv*/, ErlNifPid /*pid*/,
    int port, std::string certPath, std::string keyPath)
{
    auto acceptor = std::make_shared<one::etls::TLSAcceptor>(
        app.ioService(), port, certPath, keyPath);

    auto res = nifpp::construct_resource<one::etls::TLSAcceptor::Ptr>(acceptor);

    return nifpp::make(env, std::make_tuple(ok, res));
}

ERL_NIF_TERM accept(ErlNifEnv *env, Env localEnv, ErlNifPid pid, nifpp::TERM r,
    one::etls::TLSAcceptor::Ptr acceptor)
{
    nifpp::TERM ref{enif_make_copy(localEnv, r)};

    auto onSuccess = [=](one::etls::TLSSocket::Ptr sock) mutable {
        auto resource =
            nifpp::construct_resource<one::etls::TLSSocket::Ptr>(sock);

        auto message = nifpp::make(localEnv,
            std::make_tuple(ref, std::make_tuple(
                                     ok, nifpp::make(localEnv, resource))));

        enif_send(nullptr, &pid, localEnv, message);
    };

    acceptor->acceptAsync(
        acceptor, std::move(onSuccess), onError(localEnv, pid, ref));

    return nifpp::make(env, ok);
}

ERL_NIF_TERM handshake(ErlNifEnv *env, Env localEnv, ErlNifPid pid,
    nifpp::TERM r, one::etls::TLSSocket::Ptr sock)
{
    nifpp::TERM ref{enif_make_copy(localEnv, r)};

    auto onSuccess = [=]() mutable {
        auto message = nifpp::make(localEnv, std::make_tuple(ref, ok));
        enif_send(nullptr, &pid, localEnv, message);
    };

    sock->handshakeAsync(
        sock, std::move(onSuccess), onError(localEnv, pid, ref));

    return nifpp::make(env, ok);
}

ERL_NIF_TERM peername(ErlNifEnv *env, Env localEnv, ErlNifPid pid,
    nifpp::TERM r, one::etls::TLSSocket::Ptr sock)
{
    nifpp::TERM ref{enif_make_copy(localEnv, r)};

    auto onSuccess = [=](auto &endpoint) mutable {
        auto address = endpoint.address().to_string();
        auto port = endpoint.port();
        auto message = nifpp::make(
            localEnv, std::make_tuple(ref,
                          std::make_tuple(ok, std::make_tuple(address, port))));

        enif_send(nullptr, &pid, localEnv, message);
    };

    sock->remoteEndpointAsync(
        sock, std::move(onSuccess), onError(localEnv, pid, ref));

    return nifpp::make(env, ok);
}

ERL_NIF_TERM sockname(ErlNifEnv *env, Env localEnv, ErlNifPid pid,
    nifpp::TERM r, one::etls::TLSSocket::Ptr sock)
{
    nifpp::TERM ref{enif_make_copy(localEnv, r)};

    auto onSuccess = [=](auto &endpoint) mutable {
        auto address = endpoint.address().to_string();
        auto port = endpoint.port();
        auto message = nifpp::make(
            localEnv, std::make_tuple(ref,
                          std::make_tuple(ok, std::make_tuple(address, port))));

        enif_send(nullptr, &pid, localEnv, message);
    };

    sock->localEndpointAsync(
        sock, std::move(onSuccess), onError(localEnv, pid, ref));

    return nifpp::make(env, ok);
}

ERL_NIF_TERM acceptor_sockname(ErlNifEnv *env, Env localEnv, ErlNifPid pid,
    nifpp::TERM r, one::etls::TLSAcceptor::Ptr acceptor)
{
    nifpp::TERM ref{enif_make_copy(localEnv, r)};

    auto onSuccess = [=](auto &endpoint) mutable {
        auto address = endpoint.address().to_string();
        auto port = endpoint.port();
        auto message = nifpp::make(
            localEnv, std::make_tuple(ref,
                          std::make_tuple(ok, std::make_tuple(address, port))));

        enif_send(nullptr, &pid, localEnv, message);
    };

    acceptor->localEndpointAsync(
        acceptor, std::move(onSuccess), onError(localEnv, pid, ref));

    return nifpp::make(env, ok);
}

ERL_NIF_TERM close(ErlNifEnv *env, Env localEnv, ErlNifPid pid, nifpp::TERM r,
    one::etls::TLSSocket::Ptr sock)
{
    nifpp::TERM ref{enif_make_copy(localEnv, r)};

    auto onSuccess = [=]() mutable {
        auto message = nifpp::make(localEnv, std::make_tuple(ref, ok));
        enif_send(nullptr, &pid, localEnv, message);
    };

    sock->closeAsync(sock, std::move(onSuccess), onError(localEnv, pid, ref));

    return nifpp::make(env, ok);
}

ERL_NIF_TERM certificate_chain(ErlNifEnv *env, Env /*localEnv*/,
    ErlNifPid /*pid*/, one::etls::TLSSocket::Ptr sock)
{
    auto &chain = sock->certificateChain();

    std::vector<nifpp::TERM> terms;
    std::transform(chain.begin(), chain.end(), std::back_inserter(terms),
        [env](auto &cert) {
            nifpp::binary bin{cert.size()};
            std::memcpy(bin.data, cert.data(), bin.size);
            return nifpp::TERM{nifpp::make(env, bin)};
        });

    return nifpp::make(env, std::make_tuple(ok, terms));
}

ERL_NIF_TERM shutdown(ErlNifEnv *env, Env localEnv, ErlNifPid pid,
    nifpp::TERM r, one::etls::TLSSocket::Ptr sock, nifpp::str_atom type)
{
    nifpp::TERM ref{enif_make_copy(localEnv, r)};

    auto onSuccess = [=]() mutable {
        auto message = nifpp::make(localEnv, std::make_tuple(ref, ok));
        enif_send(nullptr, &pid, localEnv, message);
    };

    auto asioType = asio::socket_base::shutdown_both;
    if (type == "read")
        asioType = asio::socket_base::shutdown_receive;
    else if (type == "write")
        asioType = asio::socket_base::shutdown_send;
    else if (type == "read_write")
        asioType = asio::socket_base::shutdown_both;
    else
        throw nifpp::badarg{};

    sock->shutdownAsync(
        sock, asioType, std::move(onSuccess), onError(localEnv, pid, ref));

    return nifpp::make(env, ok);
}

} // namespace

/**
 * NIF functions.
 */
extern "C" {

static int load(ErlNifEnv *env, void ** /*priv*/, ERL_NIF_TERM /*load_info*/)
{
    nifpp::register_resource<one::etls::TLSSocket::Ptr>(
        env, nullptr, "TLSSocket");

    nifpp::register_resource<one::etls::TLSAcceptor::Ptr>(
        env, nullptr, "TLSAcceptor");

    return 0;
}

static ERL_NIF_TERM connect_nif(
    ErlNifEnv *env, int /*argc*/, const ERL_NIF_TERM argv[])
{
    return wrap(connect, env, argv);
}

static ERL_NIF_TERM send_nif(
    ErlNifEnv *env, int /*argc*/, const ERL_NIF_TERM argv[])
{
    return wrap(send, env, argv);
}

static ERL_NIF_TERM recv_nif(
    ErlNifEnv *env, int /*argc*/, const ERL_NIF_TERM argv[])
{
    return wrap(recv, env, argv);
}

static ERL_NIF_TERM listen_nif(
    ErlNifEnv *env, int /*argc*/, const ERL_NIF_TERM argv[])
{
    return wrap(listen, env, argv);
}

static ERL_NIF_TERM accept_nif(
    ErlNifEnv *env, int /*argc*/, const ERL_NIF_TERM argv[])
{
    return wrap(accept, env, argv);
}

static ERL_NIF_TERM handshake_nif(
    ErlNifEnv *env, int /*argc*/, const ERL_NIF_TERM argv[])
{
    return wrap(handshake, env, argv);
}

static ERL_NIF_TERM peername_nif(
    ErlNifEnv *env, int /*argc*/, const ERL_NIF_TERM argv[])
{
    return wrap(peername, env, argv);
}

static ERL_NIF_TERM sockname_nif(
    ErlNifEnv *env, int /*argc*/, const ERL_NIF_TERM argv[])
{
    return wrap(sockname, env, argv);
}

static ERL_NIF_TERM acceptor_sockname_nif(
    ErlNifEnv *env, int /*argc*/, const ERL_NIF_TERM argv[])
{
    return wrap(acceptor_sockname, env, argv);
}

static ERL_NIF_TERM close_nif(
    ErlNifEnv *env, int /*argc*/, const ERL_NIF_TERM argv[])
{
    return wrap(close, env, argv);
}

static ERL_NIF_TERM certificate_chain_nif(
    ErlNifEnv *env, int /*argc*/, const ERL_NIF_TERM argv[])
{
    return wrap(certificate_chain, env, argv);
}

static ERL_NIF_TERM shutdown_nif(
    ErlNifEnv *env, int /*argc*/, const ERL_NIF_TERM argv[])
{
    return wrap(shutdown, env, argv);
}

static ErlNifFunc nif_funcs[] = {{"connect", 3, connect_nif},
    {"send", 2, send_nif}, {"recv", 2, recv_nif}, {"listen", 3, listen_nif},
    {"accept", 2, accept_nif}, {"handshake", 2, handshake_nif},
    {"peername", 2, peername_nif}, {"sockname", 2, sockname_nif},
    {"acceptor_sockname", 2, acceptor_sockname_nif}, {"close", 2, close_nif},
    {"certificate_chain", 1, certificate_chain_nif},
    {"shutdown", 3, shutdown_nif}};

ERL_NIF_INIT(ssl2_nif, nif_funcs, load, NULL, NULL, NULL)

} // extern C
