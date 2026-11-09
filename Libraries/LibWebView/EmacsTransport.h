/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/JsonObject.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Queue.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibCore/Forward.h>
#include <LibWebView/EmacsProtocol.h>
#include <LibWebView/Export.h>

#include <sys/types.h>

namespace WebView {

// The byte stream to the Emacs client: the standard streams this process was started with, or connections on a Unix
// socket. It turns a stream into records and queues what the client is slow to read.
//
// A connection becomes the client once it has said a hello that the server accepts. On a socket that takes the place of
// the client before it, which is told so; connections that have yet to say hello leave the client alone.
class WEBVIEW_API EmacsTransport : public Weakable<EmacsTransport> {
public:
    AK_ALLOC_WITH_KMALLOC;

    enum class Kind : u8 {
        Stdio,
        Socket,
    };

    // Keeps the standard streams for the client, and sends what this process and every helper it starts would print
    // there to a log file instead. Must run before any helper process is started.
    static ErrorOr<void> take_over_stdio(Kind, Optional<ByteString> const& log_path);
    static ByteString const& log_path();

    static ErrorOr<NonnullOwnPtr<EmacsTransport>> create(Kind, Optional<ByteString> const& socket_path);
    ~EmacsTransport();

    // Says why a hello is not acceptable, or nothing for one that is.
    Function<Optional<String>(JsonObject const&)> on_validate_hello;
    // A connection became the client. Its hello follows as the first control message.
    Function<void()> on_client_connected;
    Function<void(JsonObject const&)> on_control;
    // The client is gone and none took its place, or on the standard streams, the stream ended. This is never called
    // from within a call into the transport.
    Function<void()> on_client_disconnected;

    Kind kind() const { return m_kind; }
    bool has_client() const { return m_client; }

    // Fails for a message that does not fit a record.
    ErrorOr<void> send_control(JsonObject const&);
    // Takes a whole record, header included.
    void send_record(ByteBuffer&&);

    // Closes every connection, stops listening and removes the socket.
    void shutdown();

private:
    struct Connection {
        AK_ALLOC_WITH_KMALLOC;

        ~Connection();
        void close();

        u64 id { 0 };
        int in_fd { -1 };
        int out_fd { -1 };
        bool is_socket { false };
        RefPtr<Core::Notifier> read_notifier;
        RefPtr<Core::Notifier> write_notifier;
        RefPtr<Core::Timer> drain_timer;
        EmacsProtocol::RecordReader reader;
        Queue<ByteBuffer> outbox;
        size_t outbox_offset { 0 };
        size_t outbox_size { 0 };
    };

    enum class WriteResult : u8 {
        Drained,
        Blocked,
        Failed,
    };

    explicit EmacsTransport(Kind);

    ErrorOr<void> listen(ByteString const& socket_path);
    void accept_connections();
    ErrorOr<void> add_connection(int in_fd, int out_fd);

    Connection* find_connection(u64 id);
    Optional<size_t> pending_index(u64 id) const;
    Optional<size_t> retiring_index(u64 id) const;

    void connection_readable(u64 id);
    void connection_writable(u64 id);
    void connection_lost(u64 id);
    void handle_first_message(u64 id, JsonObject const&);
    void reject_connection(u64 id, StringView reason);

    // Sends the connection one last message in place of what it has not been sent yet, and closes it.
    void retire(NonnullOwnPtr<Connection>, JsonObject const& last_message);
    void finish_writing(Connection&);
    void discard(NonnullOwnPtr<Connection>);
    void notify_client_disconnected();

    WriteResult enqueue(Connection&, ByteBuffer&&);
    WriteResult write_queued(Connection&);

    Kind m_kind;

    int m_listen_fd { -1 };
    ByteString m_socket_path;
    dev_t m_socket_device { 0 };
    ino_t m_socket_inode { 0 };
    RefPtr<Core::Notifier> m_listen_notifier;

    u64 m_next_connection_id { 1 };
    OwnPtr<Connection> m_client;
    // Connections that have yet to say hello.
    Vector<NonnullOwnPtr<Connection>> m_pending;
    // Connections that are only read their last message.
    Vector<NonnullOwnPtr<Connection>> m_retiring;
};

}
