/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonValue.h>
#include <AK/NeverDestroyed.h>
#include <AK/Time.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibWebView/EmacsTransport.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace WebView {

static int s_stream_in_fd = -1;
static int s_stream_out_fd = -1;
static NeverDestroyed<ByteString> s_log_path;

// A client that reads nothing while this much waits for it is taken to be stuck.
static constexpr size_t maximum_queued_bytes = 256 * MiB;
// The oldest connection that has yet to say hello goes when there are more than this many.
static constexpr size_t maximum_pending_connections = 8;
static constexpr int listen_backlog = 32;
// How long a connection that is being closed gets to read its last message.
static constexpr auto drain_timeout = AK::Duration::from_seconds(3);

static ErrorOr<void> set_nonblocking_and_close_on_exec(int fd)
{
    auto flags = TRY(Core::System::fcntl(fd, F_GETFL));
    TRY(Core::System::fcntl(fd, F_SETFL, flags | O_NONBLOCK));
    TRY(Core::System::fcntl(fd, F_SETFD, FD_CLOEXEC));
    return {};
}

static bool is_socket(int fd)
{
    struct stat status {};
    return ::fstat(fd, &status) == 0 && S_ISSOCK(status.st_mode);
}

// Writes without raising SIGPIPE when the reader is gone, so that the write fails with EPIPE instead. Where a file
// descriptor cannot be told to do that, a socket is told with each write, and for a pipe the signal is held back from
// this thread during the write and taken off it afterwards.
static ssize_t write_without_sigpipe(int fd, ReadonlyBytes bytes, [[maybe_unused]] bool fd_is_socket)
{
#if defined(F_SETNOSIGPIPE)
    return ::write(fd, bytes.data(), bytes.size());
#else
    if (fd_is_socket)
        return ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);

    sigset_t sigpipe;
    sigemptyset(&sigpipe);
    sigaddset(&sigpipe, SIGPIPE);

    sigset_t pending_before;
    sigemptyset(&pending_before);
    sigpending(&pending_before);
    auto was_pending = sigismember(&pending_before, SIGPIPE) == 1;

    sigset_t previous_mask;
    pthread_sigmask(SIG_BLOCK, &sigpipe, &previous_mask);
    auto written = ::write(fd, bytes.data(), bytes.size());
    auto write_errno = errno;
    if (written < 0 && write_errno == EPIPE && !was_pending) {
        struct timespec no_wait {};
        (void)sigtimedwait(&sigpipe, nullptr, &no_wait);
    }
    pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    errno = write_errno;
    return written;
#endif
}

ErrorOr<void> EmacsTransport::take_over_stdio(Kind kind, Optional<ByteString> const& log_path)
{
    int log_fd = -1;
    if (log_path.has_value()) {
        *s_log_path = *log_path;
        log_fd = TRY(Core::System::open(*s_log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600));
    } else {
        // NB: Anyone can put a file or a link at a name in /tmp, so the log is a file this process creates there.
        *s_log_path = ByteString::formatted("/tmp/ladybird-emacs-{}.log", getpid());
        (void)::unlink(s_log_path->characters());
        log_fd = TRY(Core::System::open(*s_log_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    }

    if (kind == Kind::Stdio) {
        s_stream_in_fd = TRY(Core::System::dup(STDIN_FILENO));
        s_stream_out_fd = TRY(Core::System::dup(STDOUT_FILENO));
        TRY(Core::System::fcntl(s_stream_in_fd, F_SETFD, FD_CLOEXEC));
        TRY(Core::System::fcntl(s_stream_out_fd, F_SETFD, FD_CLOEXEC));
    } else {
        auto message = ByteString::formatted("Ladybird logs to {}\n", *s_log_path);
        (void)Core::System::write(STDERR_FILENO, message.bytes());
    }

    // Helper processes inherit these, so what any of them prints goes to the log rather than into the stream.
    auto null_fd = TRY(Core::System::open("/dev/null"sv, O_RDONLY | O_CLOEXEC));
    if (::dup2(null_fd, STDIN_FILENO) < 0 || ::dup2(log_fd, STDOUT_FILENO) < 0 || ::dup2(log_fd, STDERR_FILENO) < 0)
        return Error::from_syscall("dup2"sv, errno);
    TRY(Core::System::close(null_fd));
    TRY(Core::System::close(log_fd));
    return {};
}

ByteString const& EmacsTransport::log_path()
{
    return *s_log_path;
}

ErrorOr<NonnullOwnPtr<EmacsTransport>> EmacsTransport::create(Kind kind, Optional<ByteString> const& socket_path)
{
    auto transport = adopt_own(*new EmacsTransport(kind));

    if (kind == Kind::Socket) {
        if (!socket_path.has_value())
            return Error::from_string_literal("The socket transport needs a socket path");
        TRY(transport->listen(*socket_path));
        return transport;
    }

    if (s_stream_in_fd < 0 || s_stream_out_fd < 0)
        return Error::from_string_literal("The standard streams must be taken over before the Emacs transport is created");
    TRY(transport->add_connection(s_stream_in_fd, s_stream_out_fd));
    s_stream_in_fd = -1;
    s_stream_out_fd = -1;
    return transport;
}

EmacsTransport::EmacsTransport(Kind kind)
    : m_kind(kind)
{
}

EmacsTransport::~EmacsTransport()
{
    shutdown();
}

EmacsTransport::Connection::~Connection()
{
    close();
}

void EmacsTransport::Connection::close()
{
    if (read_notifier)
        read_notifier->set_enabled(false);
    if (write_notifier)
        write_notifier->set_enabled(false);
    if (drain_timer)
        drain_timer->stop();

    if (out_fd >= 0 && out_fd != in_fd)
        ::close(out_fd);
    if (in_fd >= 0)
        ::close(in_fd);
    in_fd = -1;
    out_fd = -1;
}

ErrorOr<void> EmacsTransport::listen(ByteString const& socket_path)
{
    sockaddr_un address {};
    address.sun_family = AF_LOCAL;
    if (socket_path.length() >= sizeof(address.sun_path))
        return Error::from_string_literal("The Emacs socket path is too long for a Unix socket");
    socket_path.bytes().copy_to({ address.sun_path, sizeof(address.sun_path) });

    // Only a socket that no server answers on makes way for this one.
    struct stat existing {};
    if (::lstat(socket_path.characters(), &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode))
            return Error::from_string_literal("The Emacs socket path exists and is not a socket");

        auto probe_fd = TRY(Core::System::socket(AF_LOCAL, SOCK_STREAM, 0));
        auto connected = ::connect(probe_fd, reinterpret_cast<sockaddr const*>(&address), sizeof(address)) == 0;
        auto connect_errno = errno;
        ::close(probe_fd);
        if (connected)
            return Error::from_string_literal("Another server already listens on the Emacs socket path");
        if (connect_errno != ECONNREFUSED)
            return Error::from_syscall("connect"sv, connect_errno);
        TRY(Core::System::unlink(socket_path));
    } else if (errno != ENOENT) {
        return Error::from_syscall("lstat"sv, errno);
    }

    m_listen_fd = TRY(Core::System::socket(AF_LOCAL, SOCK_STREAM, 0));
    TRY(set_nonblocking_and_close_on_exec(m_listen_fd));
    TRY(Core::System::bind(m_listen_fd, reinterpret_cast<sockaddr const*>(&address), sizeof(address)));

    // The socket is this server's to remove for as long as the path names the one bound here.
    struct stat bound {};
    if (::lstat(socket_path.characters(), &bound) < 0)
        return Error::from_syscall("lstat"sv, errno);
    m_socket_path = socket_path;
    m_socket_device = bound.st_dev;
    m_socket_inode = bound.st_ino;

    TRY(Core::System::chmod(socket_path, 0600));
    TRY(Core::System::listen(m_listen_fd, listen_backlog));

    m_listen_notifier = Core::Notifier::construct(m_listen_fd, Core::Notifier::Type::Read);
    m_listen_notifier->on_activation = [this] {
        accept_connections();
    };
    return {};
}

void EmacsTransport::accept_connections()
{
    for (;;) {
        auto fd = ::accept(m_listen_fd, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (add_connection(fd, fd).is_error())
            ::close(fd);
    }
}

ErrorOr<void> EmacsTransport::add_connection(int in_fd, int out_fd)
{
    TRY(set_nonblocking_and_close_on_exec(in_fd));
    if (out_fd != in_fd)
        TRY(set_nonblocking_and_close_on_exec(out_fd));
#if defined(F_SETNOSIGPIPE)
    TRY(Core::System::fcntl(out_fd, F_SETNOSIGPIPE, 1));
#endif

    auto connection = make<Connection>();
    auto id = m_next_connection_id++;
    connection->id = id;
    connection->in_fd = in_fd;
    connection->out_fd = out_fd;
    connection->is_socket = is_socket(out_fd);

    connection->read_notifier = Core::Notifier::construct(in_fd, Core::Notifier::Type::Read);
    connection->read_notifier->on_activation = [this, id] {
        connection_readable(id);
    };

    connection->write_notifier = Core::Notifier::construct(out_fd, Core::Notifier::Type::Write);
    connection->write_notifier->set_enabled(false);
    connection->write_notifier->on_activation = [this, id] {
        connection_writable(id);
    };

    if (m_pending.size() >= maximum_pending_connections)
        discard(m_pending.take_first());
    m_pending.append(move(connection));
    return {};
}

EmacsTransport::Connection* EmacsTransport::find_connection(u64 id)
{
    if (m_client && m_client->id == id)
        return m_client.ptr();
    if (auto index = pending_index(id); index.has_value())
        return m_pending[*index].ptr();
    if (auto index = retiring_index(id); index.has_value())
        return m_retiring[*index].ptr();
    return nullptr;
}

Optional<size_t> EmacsTransport::pending_index(u64 id) const
{
    return m_pending.find_first_index_if([&](auto const& connection) { return connection->id == id; });
}

Optional<size_t> EmacsTransport::retiring_index(u64 id) const
{
    return m_retiring.find_first_index_if([&](auto const& connection) { return connection->id == id; });
}

// The connection is closed at once. What is left of it goes later, as this may run in one of its own callbacks.
void EmacsTransport::discard(NonnullOwnPtr<Connection> connection)
{
    connection->close();
    Core::deferred_invoke([connection = move(connection)] { });
}

void EmacsTransport::notify_client_disconnected()
{
    Core::deferred_invoke([weak_this = make_weak_ptr()] {
        // NB: A connection that became the client meanwhile has been announced as such.
        if (!weak_this || weak_this->m_client)
            return;
        if (weak_this->on_client_disconnected)
            weak_this->on_client_disconnected();
    });
}

void EmacsTransport::connection_lost(u64 id)
{
    if (m_client && m_client->id == id) {
        discard(m_client.release_nonnull());
        notify_client_disconnected();
        return;
    }

    if (auto index = pending_index(id); index.has_value()) {
        discard(m_pending.take(*index));
        // Nothing can connect to the standard streams again.
        if (m_kind == Kind::Stdio)
            notify_client_disconnected();
        return;
    }

    if (auto index = retiring_index(id); index.has_value())
        discard(m_retiring.take(*index));
}

void EmacsTransport::retire(NonnullOwnPtr<Connection> connection, JsonObject const& last_message)
{
    // A record that has been written in part is finished, so that the stream stays a stream of records.
    Optional<ByteBuffer> record_in_progress;
    if (connection->outbox_offset > 0)
        record_in_progress = connection->outbox.dequeue();
    connection->outbox.clear();
    connection->outbox_size = 0;
    if (record_in_progress.has_value()) {
        connection->outbox_size = record_in_progress->size() - connection->outbox_offset;
        connection->outbox.enqueue(record_in_progress.release_value());
    }

    auto result = WriteResult::Failed;
    if (auto record = EmacsProtocol::encode_control_record(last_message); !record.is_error())
        result = enqueue(*connection, record.release_value());
    if (result == WriteResult::Failed) {
        discard(move(connection));
        return;
    }
    if (result == WriteResult::Drained)
        finish_writing(*connection);

    auto id = connection->id;
    connection->drain_timer = Core::Timer::create_single_shot(drain_timeout, [this, id] {
        if (auto index = retiring_index(id); index.has_value())
            discard(m_retiring.take(*index));
    });
    connection->drain_timer->start();
    m_retiring.append(move(connection));
}

// The peer of a socket reads the end of the stream after its last message, while what it still writes goes somewhere
// until it closes the socket or its time is up.
void EmacsTransport::finish_writing(Connection& connection)
{
    connection.write_notifier->set_enabled(false);
    if (connection.is_socket)
        ::shutdown(connection.out_fd, SHUT_WR);
}

void EmacsTransport::reject_connection(u64 id, StringView reason)
{
    dbgln("Emacs: rejecting a connection: {}", reason);
    JsonObject reject;
    reject.set("type"sv, "reject"sv);
    reject.set("reason"sv, reason);

    if (m_client && m_client->id == id) {
        retire(m_client.release_nonnull(), reject);
        notify_client_disconnected();
        return;
    }

    auto index = pending_index(id);
    if (!index.has_value())
        return;
    retire(m_pending.take(*index), reject);
    if (m_kind == Kind::Stdio)
        notify_client_disconnected();
}

void EmacsTransport::handle_first_message(u64 id, JsonObject const& message)
{
    if (message.get_string("type"sv) != "hello"sv) {
        reject_connection(id, "the first message must be hello"sv);
        return;
    }
    if (on_validate_hello) {
        if (auto reason = on_validate_hello(message); reason.has_value()) {
            reject_connection(id, *reason);
            return;
        }
    }

    auto connection = m_pending.take(*pending_index(id));
    if (m_client) {
        dbgln("Emacs: a new client replaces the connected one");
        JsonObject replaced;
        replaced.set("type"sv, "replaced"sv);
        retire(m_client.release_nonnull(), replaced);
    }
    m_client = move(connection);

    if (on_client_connected)
        on_client_connected();
    if (on_control && m_client && m_client->id == id)
        on_control(message);
}

void EmacsTransport::connection_readable(u64 id)
{
    auto* connection = find_connection(id);
    if (!connection)
        return;

    u8 buffer[64 * KiB];
    auto read = Core::System::read(connection->in_fd, { buffer, sizeof(buffer) });
    if (read.is_error()) {
        if (read.error().code() != EINTR && read.error().code() != EAGAIN)
            connection_lost(id);
        return;
    }
    if (read.value() == 0) {
        connection_lost(id);
        return;
    }
    // NB: What a connection that is being closed still sends is not looked at.
    if (retiring_index(id).has_value())
        return;
    connection->reader.feed({ buffer, read.value() });

    for (;;) {
        // Handling a message can end the connection, or make it the client.
        auto is_client = m_client && m_client->id == id;
        auto is_pending = pending_index(id).has_value();
        if (!is_client && !is_pending)
            return;
        connection = find_connection(id);

        auto record = connection->reader.next();
        if (!record.has_value())
            break;
        if (record->kind != EmacsProtocol::RecordKind::Control)
            continue;

        auto message = JsonValue::from_string(StringView { record->payload.bytes() });
        if (message.is_error() || !message.value().is_object()) {
            dbgln("Emacs: ignoring a control record that is not a JSON object");
            continue;
        }

        if (is_pending)
            handle_first_message(id, message.value().as_object());
        else if (on_control)
            on_control(message.value().as_object());
    }

    if (connection->reader.has_error())
        reject_connection(id, "framing error"sv);
}

void EmacsTransport::connection_writable(u64 id)
{
    auto* connection = find_connection(id);
    if (!connection)
        return;

    switch (write_queued(*connection)) {
    case WriteResult::Blocked:
        break;
    case WriteResult::Failed:
        connection_lost(id);
        break;
    case WriteResult::Drained:
        if (auto index = retiring_index(id); index.has_value()) {
            if (connection->is_socket)
                finish_writing(*connection);
            else
                discard(m_retiring.take(*index));
        }
        break;
    }
}

ErrorOr<void> EmacsTransport::send_control(JsonObject const& message)
{
    send_record(TRY(EmacsProtocol::encode_control_record(message)));
    return {};
}

void EmacsTransport::send_record(ByteBuffer&& record)
{
    if (!m_client)
        return;

    auto id = m_client->id;
    if (enqueue(*m_client, move(record)) == WriteResult::Failed) {
        connection_lost(id);
        return;
    }
    // NB: What the client was sent meanwhile, from on_wrote, can have cost the connection already.
    if (m_client && m_client->id == id && m_client->outbox_size > maximum_queued_bytes) {
        dbgln("Emacs: the client does not read its stream, disconnecting it");
        connection_lost(id);
    }
}

EmacsTransport::WriteResult EmacsTransport::enqueue(Connection& connection, ByteBuffer&& record)
{
    auto was_empty = connection.outbox.is_empty();
    connection.outbox_size += record.size();
    connection.outbox.enqueue(move(record));
    return was_empty ? write_queued(connection) : WriteResult::Blocked;
}

EmacsTransport::WriteResult EmacsTransport::write_queued(Connection& connection)
{
    auto wrote = false;
    auto result = WriteResult::Drained;
    while (!connection.outbox.is_empty()) {
        auto bytes = connection.outbox.head().bytes().slice(connection.outbox_offset);
        auto written = write_without_sigpipe(connection.out_fd, bytes, connection.is_socket);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                connection.write_notifier->set_enabled(true);
                result = WriteResult::Blocked;
                break;
            }
            return WriteResult::Failed;
        }

        wrote = true;
        connection.bytes_written += written;
        connection.outbox_offset += written;
        connection.outbox_size -= written;
        if (connection.outbox_offset == connection.outbox.head().size()) {
            connection.outbox.dequeue();
            connection.outbox_offset = 0;
        }
    }
    if (result == WriteResult::Drained)
        connection.write_notifier->set_enabled(false);
    if (wrote && &connection == m_client.ptr() && on_wrote)
        on_wrote();
    return result;
}

void EmacsTransport::shutdown()
{
    // NB: This is not called from a connection's own callbacks, so the connections can go at once.
    m_client = nullptr;
    m_pending.clear();
    m_retiring.clear();

    if (m_listen_notifier)
        m_listen_notifier->set_enabled(false);
    m_listen_notifier = nullptr;
    if (m_listen_fd >= 0) {
        ::close(m_listen_fd);
        m_listen_fd = -1;
    }

    // NB: The path may by now name something another process put there.
    if (!m_socket_path.is_empty()) {
        struct stat current {};
        if (::lstat(m_socket_path.characters(), &current) == 0 && S_ISSOCK(current.st_mode) && current.st_dev == m_socket_device && current.st_ino == m_socket_inode)
            (void)::unlink(m_socket_path.characters());
        m_socket_path = {};
    }
}

}
