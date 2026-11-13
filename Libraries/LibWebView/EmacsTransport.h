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
    // there to a log file instead. On a socket, it first listens there, or without a socket path takes the socket that
    // take_activated_socket() describes, so that a server that cannot have the socket leaves the log alone. Must run
    // before any helper process is started. Without a log path the log is
    // ladybird-emacs-<pid>.log in the directory of the default socket when default_socket_path() gave the socket path,
    // else in $XDG_RUNTIME_DIR, and where there is neither or that cannot be opened, /tmp/ladybird-emacs-<pid>-XXXXXX.log
    // with XXXXXX chosen at random. A log that cannot be opened is named on stderr and to a waiting starter.
    static ErrorOr<void> take_over_stdio(Kind, Optional<ByteString> const& socket_path, Optional<ByteString> const& log_path);
    static ByteString const& log_path();

    // $XDG_RUNTIME_DIR/ladybird-emacs.sock, or where that variable is unset, empty or relative, ladybird-emacs.sock in
    // the directory ladybird-emacs-<uid> of $TMPDIR, or of /tmp where that is unset, empty or relative, with <uid> the
    // real user ID. That directory is made with mode 0700 when it is missing. It fails, naming the path on stderr and
    // to a waiting starter, when the socket path does not fit, or when the directory is a symbolic link, not a
    // directory, not the user's, or open to its group or others, or its parent belongs to another user than root or the
    // user, or its group or others can write to the parent and it is not sticky.
    static ErrorOr<ByteString> default_socket_path();
    // Fails for a path that does not fit the address of a Unix socket.
    static ErrorOr<void> validate_socket_path(StringView);

    // Runs this program again with the given arguments in a session of its own, with its standard streams on /dev/null,
    // and waits up to a minute until it accepts connections on its socket or fails to start. Says on stderr where it
    // logs to, or why it did not start, and returns the status for this process to exit with.
    static ErrorOr<int> start_in_background(ReadonlySpan<StringView> arguments);
    // The variable through which start_in_background() names the pipe to the process waiting for the server.
    static constexpr StringView starter_fd_variable = "LADYBIRD_EMACS_STARTER_FD"sv;
    // Takes the pipe out of the environment, so that no helper process sees it. Says whether there was one.
    static ErrorOr<bool> take_starter_fd();
    // Tells the waiting process, if there is one, that the server accepts connections, or why it does not.
    static void notify_starter(Optional<ByteString> const& failure);

    static ErrorOr<NonnullOwnPtr<EmacsTransport>> create(Kind);
    ~EmacsTransport();

    // Says why a hello is not acceptable, or nothing for one that is.
    Function<Optional<String>(JsonObject const&)> on_validate_hello;
    // A connection became the client. Its hello follows as the first control message.
    Function<void()> on_client_connected;
    Function<void(JsonObject const&)> on_control;
    // The client is gone and none took its place, or on the standard streams, the stream ended. This is never called
    // from within a call into the transport.
    Function<void()> on_client_disconnected;
    // The client has read some of what it was sent.
    Function<void()> on_wrote;

    Kind kind() const { return m_kind; }
    bool has_client() const { return m_client; }
    // The bytes the client has yet to read.
    size_t queued_bytes() const { return m_client ? m_client->outbox_size : 0; }
    // How far into the stream the client has read, and where the stream ends so far, in bytes from where it started.
    u64 written_bytes() const { return m_client ? m_client->bytes_written : 0; }
    u64 end_of_stream() const { return written_bytes() + queued_bytes(); }

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
        u64 bytes_written { 0 };
    };

    enum class WriteResult : u8 {
        Drained,
        Blocked,
        Failed,
    };

    explicit EmacsTransport(Kind);

    static ErrorOr<void> listen(ByteString const& socket_path);
    // Takes the listening socket that systemd or launchd made for this process: with LISTEN_PID naming this process,
    // the one socket that LISTEN_FDS counts, as descriptor 3, else on macOS the socket named Listeners in the launchd job.
    // LISTEN_PID, LISTEN_FDS, LISTEN_FDNAMES and LISTEN_PIDFDID are taken out of the environment. It fails for anything
    // but a Unix stream socket that is bound and listens, and the socket file, which belongs to whoever made the socket,
    // is left alone.
    static ErrorOr<void> take_activated_socket();
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
