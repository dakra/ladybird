/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonValue.h>
#include <AK/NeverDestroyed.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibWebView/EmacsTransport.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#if defined(AK_OS_MACOS)
#    include <launch.h>
#endif

namespace WebView {

static int s_stream_in_fd = -1;
static int s_stream_out_fd = -1;
static int s_starter_fd = -1;
static NeverDestroyed<ByteString> s_log_path;
// The directory of the default socket, once default_socket_path() has made sure of it.
static NeverDestroyed<Optional<ByteString>> s_default_socket_directory;

// The socket that take_over_stdio() listens on, until the transport takes it.
static int s_listen_fd = -1;
static NeverDestroyed<ByteString> s_socket_path;
static dev_t s_socket_device = 0;
static ino_t s_socket_inode = 0;

// A client that reads nothing while this much waits for it is taken to be stuck.
static constexpr size_t maximum_queued_bytes = 256 * MiB;
// The oldest connection that has yet to say hello goes when there are more than this many.
static constexpr size_t maximum_pending_connections = 8;
static constexpr int listen_backlog = 32;
// How long a connection that is being closed gets to read its last message.
static constexpr auto drain_timeout = AK::Duration::from_seconds(3);
// How long a server waits for another one that is starting on the same socket path.
static constexpr auto socket_lock_timeout = AK::Duration::from_seconds(10);
// How long a process that starts a server in the background waits for it.
static constexpr auto background_start_timeout = AK::Duration::from_seconds(60);

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

// Each message to the starter ends in a NUL, which no path and no error message contains.
static void tell_starter(StringView message)
{
    auto record = ByteString::formatted("{}\0", message);
    auto bytes = record.bytes();
    while (s_starter_fd >= 0 && !bytes.is_empty()) {
        auto written = write_without_sigpipe(s_starter_fd, bytes, false);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return;
        bytes = bytes.slice(written);
    }
}

// $XDG_RUNTIME_DIR without trailing slashes, unless it is unset, empty or relative.
static Optional<ByteString> runtime_directory()
{
    auto directory = Core::Environment::get("XDG_RUNTIME_DIR"sv);
    if (!directory.has_value() || !directory->starts_with('/'))
        return {};
    return directory->trim("/"sv, TrimMode::Right);
}

// $TMPDIR without trailing slashes, or /tmp where it is unset, empty or relative.
static ByteString temporary_directory()
{
    auto directory = Core::Environment::get("TMPDIR"sv);
    if (!directory.has_value() || !directory->starts_with('/'))
        return "/tmp"sv;
    return directory->trim("/"sv, TrimMode::Right);
}

static ByteString default_socket_directory()
{
    if (auto directory = runtime_directory(); directory.has_value())
        return directory.release_value();
    return ByteString::formatted("{}/ladybird-emacs-{}", temporary_directory(), getuid());
}

// Says on stderr, and to the process that waits for a server started in the background, what failed and why.
static Error report_error(StringView context, Error error)
{
    auto message = ByteString::formatted("{}: {}", context, error);
    warnln("{}", message);
    EmacsTransport::notify_starter(ByteString::formatted("Unable to start the Emacs server: {}", message));
    return error;
}

// NB: Anyone can put a file or a link at a name in /tmp, so the log there has a name nobody can tell beforehand.
static ErrorOr<int> open_temporary_log()
{
    auto pattern = ByteString::formatted("/tmp/ladybird-emacs-{}-XXXXXX.log", getpid());
    Vector<char> name;
    name.append(pattern.characters(), pattern.length() + 1);
    auto fd = ::mkstemps(name.data(), 4);
    if (fd < 0)
        return report_error(ByteString::formatted("Unable to open the log {}", pattern), Error::from_syscall("mkstemps"sv, errno));
    *s_log_path = name.data();
    TRY(Core::System::fcntl(fd, F_SETFD, FD_CLOEXEC));
    return fd;
}

// The default log is in a directory that only the user can write to where there is one: that of the default socket
// when the server listens there, else $XDG_RUNTIME_DIR. Where it cannot be opened there, it is in /tmp.
static ErrorOr<int> open_default_log()
{
    auto directory = s_default_socket_directory->has_value() ? *s_default_socket_directory : runtime_directory();
    if (!directory.has_value())
        return open_temporary_log();

    auto path = ByteString::formatted("{}/ladybird-emacs-{}.log", *directory, getpid());
    (void)::unlink(path.characters());
    auto fd = Core::System::open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd.is_error())
        return open_temporary_log();
    *s_log_path = move(path);
    return fd.release_value();
}

ErrorOr<void> EmacsTransport::take_over_stdio(Kind kind, Optional<ByteString> const& socket_path, Optional<ByteString> const& log_path)
{
    if (kind == Kind::Socket) {
        if (socket_path.has_value())
            TRY(listen(*socket_path));
        else
            TRY(take_activated_socket());
    }
    ArmedScopeGuard stop_listening = [&] {
        if (s_listen_fd < 0)
            return;
        ::close(exchange(s_listen_fd, -1));
        if (!s_socket_path->is_empty())
            (void)::unlink(s_socket_path->characters());
    };

    int log_fd = -1;
    if (log_path.has_value()) {
        *s_log_path = *log_path;
        auto fd = Core::System::open(*s_log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd.is_error())
            return report_error(ByteString::formatted("Unable to open the log {}", *s_log_path), fd.release_error());
        log_fd = fd.release_value();
    } else {
        log_fd = TRY(open_default_log());
    }

    if (kind == Kind::Stdio) {
        s_stream_in_fd = TRY(Core::System::dup(STDIN_FILENO));
        s_stream_out_fd = TRY(Core::System::dup(STDOUT_FILENO));
        TRY(Core::System::fcntl(s_stream_in_fd, F_SETFD, FD_CLOEXEC));
        TRY(Core::System::fcntl(s_stream_out_fd, F_SETFD, FD_CLOEXEC));
    } else {
        auto message = ByteString::formatted("Ladybird logs to {}\n", *s_log_path);
        (void)Core::System::write(STDERR_FILENO, message.bytes());
        tell_starter(ByteString::formatted("log {}", *s_log_path));
    }

    // Helper processes inherit these, so what any of them prints goes to the log rather than into the stream.
    auto null_fd = TRY(Core::System::open("/dev/null"sv, O_RDONLY | O_CLOEXEC));
    if (::dup2(null_fd, STDIN_FILENO) < 0 || ::dup2(log_fd, STDOUT_FILENO) < 0 || ::dup2(log_fd, STDERR_FILENO) < 0)
        return Error::from_syscall("dup2"sv, errno);
    TRY(Core::System::close(null_fd));
    TRY(Core::System::close(log_fd));
    stop_listening.disarm();
    return {};
}

ByteString const& EmacsTransport::log_path()
{
    return *s_log_path;
}

// Anyone may create names in the temporary directory, so the default socket there is in a directory that only the user
// can enter, in a directory where nobody else can rename it. A directory that fails is refused rather than repaired.
static ErrorOr<void> make_private_directory(ByteString const& path, StringView parent_path)
{
    auto parent = TRY(Core::System::stat(parent_path));
    if (parent.st_uid != 0 && parent.st_uid != getuid())
        return Error::from_string_literal("The directory of the default Emacs socket is in a directory of another user");
    if ((parent.st_mode & (S_IWGRP | S_IWOTH)) != 0 && (parent.st_mode & S_ISVTX) == 0)
        return Error::from_string_literal("The directory of the default Emacs socket is in a directory that others can write to");

    // NB: The mode given to mkdir() is limited by the umask.
    if (auto made = Core::System::mkdir(path, 0700); !made.is_error())
        TRY(Core::System::chmod(path, 0700));
    else if (made.error().code() != EEXIST)
        return made.release_error();

    auto directory = TRY(Core::System::lstat(path));
    if (S_ISLNK(directory.st_mode))
        return Error::from_string_literal("The directory of the default Emacs socket is a symbolic link");
    if (!S_ISDIR(directory.st_mode))
        return Error::from_string_literal("The directory of the default Emacs socket exists and is not a directory");
    if (directory.st_uid != getuid())
        return Error::from_string_literal("The directory of the default Emacs socket belongs to another user");
    if ((directory.st_mode & 077) != 0)
        return Error::from_string_literal("The directory of the default Emacs socket grants permissions to its group or others");
    return {};
}

ErrorOr<ByteString> EmacsTransport::default_socket_path()
{
    auto directory = default_socket_directory();
    auto socket_path = ByteString::formatted("{}/ladybird-emacs.sock", directory);
    if (auto valid = validate_socket_path(socket_path); valid.is_error())
        return report_error(ByteString::formatted("Unable to use {} as the default Emacs socket", socket_path), valid.release_error());

    if (!runtime_directory().has_value()) {
        auto temporary = temporary_directory();
        if (auto result = make_private_directory(directory, temporary.is_empty() ? "/"sv : temporary.view()); result.is_error())
            return report_error(ByteString::formatted("Unable to use {} for the default Emacs socket", directory), result.release_error());
    }
    *s_default_socket_directory = move(directory);
    return socket_path;
}

ErrorOr<void> EmacsTransport::validate_socket_path(StringView socket_path)
{
    if (socket_path.is_empty())
        return Error::from_string_literal("The Emacs socket path is empty");
    if (socket_path.length() >= sizeof(sockaddr_un::sun_path))
        return Error::from_string_literal("The Emacs socket path is longer than a Unix socket address can hold");
    return {};
}

// Moves a descriptor above the standard streams, so that giving the server its standard streams cannot replace it.
static ErrorOr<int> move_above_standard_streams(int fd)
{
    if (fd > STDERR_FILENO)
        return fd;
    auto moved_fd = TRY(Core::System::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1));
    TRY(Core::System::close(fd));
    return moved_fd;
}

ErrorOr<int> EmacsTransport::start_in_background(ReadonlySpan<StringView> arguments)
{
    auto pipe_fds = TRY(Core::System::pipe2(O_CLOEXEC));
    auto read_fd = TRY(move_above_standard_streams(pipe_fds[0]));
    auto write_fd = TRY(move_above_standard_streams(pipe_fds[1]));
    ScopeGuard close_read_end = [&] { (void)Core::System::close(read_fd); };

    Vector<ByteString> server_arguments;
    for (auto argument : arguments.slice(1))
        server_arguments.append(argument);

    // NB: The pipe is named in the environment rather than among the arguments, where after a "--" it would be a URL.
    Vector<ByteString> environment;
    for (auto** variable = Core::Environment::raw_environ(); *variable; ++variable) {
        auto entry = Core::Environment::Entry::from_chars(*variable);
        if (entry.name != starter_fd_variable)
            environment.append(entry.full_entry);
    }
    environment.append(ByteString::formatted("{}={}", starter_fd_variable, write_fd));

    // NB: The server must not hold on to our standard streams, or whoever reads them would wait for it to exit.
    auto server_or_error = Core::Process::spawn({
        .executable = TRY(Core::System::current_executable_path()),
        .new_session = true,
        .arguments = server_arguments,
        .environment = move(environment),
        .file_actions = {
            Core::FileAction::OpenFile { .path = "/dev/null", .mode = Core::File::OpenMode::Read, .fd = STDIN_FILENO },
            Core::FileAction::OpenFile { .path = "/dev/null", .mode = Core::File::OpenMode::Write, .fd = STDOUT_FILENO },
            Core::FileAction::OpenFile { .path = "/dev/null", .mode = Core::File::OpenMode::Write, .fd = STDERR_FILENO },
            Core::FileAction::DupFd { .write_fd = write_fd, .fd = write_fd },
        },
    });
    (void)Core::System::close(write_fd);
    auto server = TRY(move(server_or_error));

    // The server says where it logs to, then that it is ready or why it failed, and closes the pipe.
    ByteBuffer report;
    auto deadline = MonotonicTime::now() + background_start_timeout;
    bool timed_out = false;
    for (;;) {
        auto remaining = deadline - MonotonicTime::now();
        if (remaining <= AK::Duration::zero()) {
            timed_out = true;
            break;
        }
        struct pollfd poll_fd { .fd = read_fd, .events = POLLIN, .revents = 0 };
        auto ready_count = Core::System::poll({ &poll_fd, 1 }, static_cast<int>(remaining.to_milliseconds()) + 1);
        if (ready_count.is_error() && ready_count.error().code() == EINTR)
            continue;
        if (TRY(move(ready_count)) == 0)
            continue;

        u8 buffer[1024];
        auto read_bytes = TRY(Core::System::read(read_fd, { buffer, sizeof(buffer) }));
        if (read_bytes == 0)
            break;
        report.append(buffer, read_bytes);
    }

    Optional<StringView> server_log_path;
    for (auto message : StringView { report.bytes() }.split_view('\0')) {
        if (message.starts_with("log "sv)) {
            server_log_path = message.substring_view(4);
        } else if (message == "ready"sv) {
            if (server_log_path.has_value())
                warnln("Ladybird logs to {}", *server_log_path);
            return 0;
        } else if (message.starts_with("error "sv)) {
            warnln("{}", message.substring_view(6));
            return 1;
        }
    }

    auto log_note = server_log_path.has_value() ? ByteString::formatted(", see {}", *server_log_path) : ByteString {};
    if (timed_out) {
        (void)Core::System::kill(server.pid(), SIGTERM);
        warnln("The Emacs server did not start within {} s and was stopped{}", background_start_timeout.to_seconds(), log_note);
        return 1;
    }
    auto status = TRY(server.wait_for_termination());
    warnln("The Emacs server ended with status {} before it accepted connections{}", status, log_note);
    return 1;
}

ErrorOr<bool> EmacsTransport::take_starter_fd()
{
    auto value = Core::Environment::get(starter_fd_variable);
    if (!value.has_value())
        return false;
    auto fd = value->to_number<int>();
    TRY(Core::Environment::unset(starter_fd_variable));
    if (!fd.has_value() || *fd <= STDERR_FILENO)
        return Error::from_string_literal("The pipe to the process that starts the Emacs server is not a valid descriptor");

    TRY(Core::System::fcntl(*fd, F_SETFD, FD_CLOEXEC));
#if defined(F_SETNOSIGPIPE)
    TRY(Core::System::fcntl(*fd, F_SETNOSIGPIPE, 1));
#endif
    s_starter_fd = *fd;
    return true;
}

void EmacsTransport::notify_starter(Optional<ByteString> const& failure)
{
    if (s_starter_fd < 0)
        return;
    if (failure.has_value())
        tell_starter(ByteString::formatted("error {}", *failure));
    else
        tell_starter("ready"sv);
    ::close(s_starter_fd);
    s_starter_fd = -1;
}

// Two servers that start on one path at once take turns to look for a server there and to bind, so that neither removes
// the socket that the other has just bound. The lock is on a file next to the socket, which its holder removes when it is
// done. Whoever gets the lock on a file that was removed in the meantime tries again.
static ErrorOr<int> lock_socket_path(ByteString const& lock_path)
{
    auto deadline = MonotonicTime::now() + socket_lock_timeout;
    for (;;) {
        auto fd_or_error = Core::System::open(lock_path, O_RDONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd_or_error.is_error())
            return report_error(ByteString::formatted("Unable to open the lock file {}", lock_path), fd_or_error.release_error());
        auto fd = fd_or_error.release_value();
        if (::flock(fd, LOCK_EX | LOCK_NB) < 0) {
            auto flock_errno = errno;
            ::close(fd);
            if (flock_errno != EWOULDBLOCK && flock_errno != EINTR)
                return Error::from_syscall("flock"sv, flock_errno);
            if (MonotonicTime::now() >= deadline)
                return Error::from_string_literal("Another server is still starting on the Emacs socket path");
            TRY(Core::System::sleep_ms(20));
            continue;
        }

        auto locked = Core::System::fstat(fd);
        auto current = Core::System::lstat(lock_path);
        if (!locked.is_error() && !current.is_error() && locked.value().st_dev == current.value().st_dev && locked.value().st_ino == current.value().st_ino)
            return fd;
        ::close(fd);
    }
}

ErrorOr<NonnullOwnPtr<EmacsTransport>> EmacsTransport::create(Kind kind)
{
    auto transport = adopt_own(*new EmacsTransport(kind));

    if (kind == Kind::Socket) {
        if (s_listen_fd < 0)
            return Error::from_string_literal("The socket must be listened on before the Emacs transport is created");
        transport->m_listen_fd = exchange(s_listen_fd, -1);
        transport->m_socket_path = move(*s_socket_path);
        transport->m_socket_device = s_socket_device;
        transport->m_socket_inode = s_socket_inode;
        transport->m_listen_notifier = Core::Notifier::construct(transport->m_listen_fd, Core::Notifier::Type::Read);
        transport->m_listen_notifier->on_activation = [self = transport.ptr()] {
            self->accept_connections();
        };
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
    TRY(validate_socket_path(socket_path));
    sockaddr_un address {};
    address.sun_family = AF_LOCAL;
    socket_path.bytes().copy_to({ address.sun_path, sizeof(address.sun_path) });

    auto lock_path = ByteString::formatted("{}.lock", socket_path);
    auto lock_fd = TRY(lock_socket_path(lock_path));
    ScopeGuard unlock = [&] {
        (void)::unlink(lock_path.characters());
        ::close(lock_fd);
    };

    // Only a socket that no server answers on makes way for this one.
    if (auto existing = Core::System::lstat(socket_path); !existing.is_error()) {
        if (!S_ISSOCK(existing.value().st_mode))
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
    } else if (existing.error().code() != ENOENT) {
        return existing.release_error();
    }

    auto listen_fd = TRY(Core::System::socket(AF_LOCAL, SOCK_STREAM, 0));
    ArmedScopeGuard close_listen_fd = [&] { ::close(listen_fd); };
    TRY(set_nonblocking_and_close_on_exec(listen_fd));
    if (auto bound = Core::System::bind(listen_fd, reinterpret_cast<sockaddr const*>(&address), sizeof(address)); bound.is_error())
        return report_error(ByteString::formatted("Unable to bind the socket {}", socket_path), bound.release_error());
    ArmedScopeGuard remove_socket = [&] { (void)::unlink(socket_path.characters()); };

    // The socket is this server's to remove for as long as the path names the one bound here.
    auto bound = TRY(Core::System::lstat(socket_path));
    TRY(Core::System::chmod(socket_path, 0600));
    TRY(Core::System::listen(listen_fd, listen_backlog));

    close_listen_fd.disarm();
    remove_socket.disarm();
    s_listen_fd = listen_fd;
    *s_socket_path = socket_path;
    s_socket_device = bound.st_dev;
    s_socket_inode = bound.st_ino;
    return {};
}

// The descriptor of the first socket that systemd hands to a process, SD_LISTEN_FDS_START.
static constexpr int systemd_first_socket_fd = 3;
#if defined(AK_OS_MACOS)
// The key of the socket in the Sockets dictionary of the launchd job.
static constexpr char const* launchd_socket_name = "Listeners";
#endif

static ErrorOr<int> take_systemd_socket(StringView listen_pid, StringView listen_fds)
{
    if (listen_pid.to_number<pid_t>() != getpid())
        return Error::from_string_literal("LISTEN_PID does not name this process");
    if (listen_fds != "1"sv)
        return Error::from_string_literal("LISTEN_FDS does not count exactly one socket");
    return systemd_first_socket_fd;
}

#if defined(AK_OS_MACOS)
static ErrorOr<int> take_launchd_socket()
{
    int* fds = nullptr;
    size_t count = 0;
    auto result = launch_activate_socket(launchd_socket_name, &fds, &count);
    if (result == ESRCH)
        return Error::from_string_literal("Neither LISTEN_FDS nor a launchd job hands this process a socket");
    if (result == ENOENT)
        return Error::from_string_literal("The launchd job of this process has no socket named Listeners");
    if (result != 0)
        return Error::from_errno(result);

    ScopeGuard free_fds = [&] { free(fds); };
    if (count == 1)
        return fds[0];
    for (size_t i = 0; i < count; ++i)
        ::close(fds[i]);
    return Error::from_string_literal("The socket named Listeners in the launchd job is not exactly one socket");
}
#endif

// A socket that systemd or launchd made listens already. Where the system cannot be asked whether a socket listens, as
// on macOS, one that is not connected is made to listen, which fails for one that is not bound.
static ErrorOr<void> check_activated_socket(int fd)
{
    auto status = TRY(Core::System::fstat(fd));
    if (!S_ISSOCK(status.st_mode))
        return Error::from_string_literal("It is not a socket");

    int type = 0;
    socklen_t type_size = sizeof(type);
    TRY(Core::System::getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_size));
    if (type != SOCK_STREAM)
        return Error::from_string_literal("It is not a stream socket");

    sockaddr_un address {};
    socklen_t address_size = sizeof(address);
    TRY(Core::System::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size));
    if (address.sun_family != AF_LOCAL)
        return Error::from_string_literal("It is not a Unix socket");

    int listening = 0;
    socklen_t listening_size = sizeof(listening);
    auto asked = Core::System::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &listening_size);
    if (!asked.is_error()) {
        if (listening == 0)
            return Error::from_string_literal("It does not listen");
        return {};
    }
    if (asked.error().code() != ENOPROTOOPT)
        return asked.release_error();

    sockaddr_un peer {};
    socklen_t peer_size = sizeof(peer);
    // NB: A socket whose connection has ended fails with EINVAL.
    auto connected = Core::System::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peer_size);
    if (!connected.is_error() || connected.error().code() == EINVAL)
        return Error::from_string_literal("It is or was connected");
    if (connected.error().code() != ENOTCONN)
        return connected.release_error();
    if (auto listened = Core::System::listen(fd, listen_backlog); listened.is_error()) {
        if (listened.error().code() == EINVAL)
            return Error::from_string_literal("It is not bound to an address and cannot listen");
        return listened.release_error();
    }
    return {};
}

ErrorOr<void> EmacsTransport::take_activated_socket()
{
    // NB: The values point into the environment, which unsetting the variables changes.
    auto listen_pid = Core::Environment::get("LISTEN_PID"sv).map([](auto value) { return ByteString { value }; });
    auto listen_fds = Core::Environment::get("LISTEN_FDS"sv).map([](auto value) { return ByteString { value }; });
    TRY(Core::Environment::unset("LISTEN_PID"sv));
    TRY(Core::Environment::unset("LISTEN_FDS"sv));
    TRY(Core::Environment::unset("LISTEN_FDNAMES"sv));
    TRY(Core::Environment::unset("LISTEN_PIDFDID"sv));

    auto context = "Unable to take the socket handed to the Emacs server"sv;
    ErrorOr<int> fd_or_error = Error::from_string_literal("Neither LISTEN_PID nor LISTEN_FDS is set");
    if (listen_pid.has_value() || listen_fds.has_value())
        fd_or_error = take_systemd_socket(listen_pid.value_or({}), listen_fds.value_or({}));
#if defined(AK_OS_MACOS)
    else
        fd_or_error = take_launchd_socket();
#endif
    if (fd_or_error.is_error())
        return report_error(context, fd_or_error.release_error());

    auto fd = fd_or_error.release_value();
    if (auto checked = check_activated_socket(fd); checked.is_error()) {
        auto error = checked.release_error();
        if (error.is_errno() && error.code() == EBADF)
            error = Error::from_string_literal("There is no open descriptor 3");
        else
            ::close(fd);
        return report_error(context, move(error));
    }
    if (auto result = set_nonblocking_and_close_on_exec(fd); result.is_error()) {
        ::close(fd);
        return report_error(context, result.release_error());
    }

    s_listen_fd = fd;
    *s_socket_path = {};
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
