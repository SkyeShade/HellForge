// capture-relay: Sync Inventory's 127.0.0.1:443 -> 127.0.0.1:18443 relay on Linux.
//
// linux/capture-helper.sh runs the installed root-owned copy (/usr/local/lib/hellforge/capture-relay), never a copy from
// the installation: the installation sits in a Wine prefix that the game and its mods can write. The program binds the
// privileged port as root, drops to nobody with setgroups/setgid/setuid (each checked), and only then accepts. It takes
// no arguments and cannot be pointed at another address or port. A fixed table of connections with one fixed buffer per
// direction bounds the number of clients and the memory the root helper's relay can use.

#define _GNU_SOURCE 1

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define LISTEN_PORT 443
#define TARGET_PORT 18443
#define MAX_CONNECTIONS 32
#define BUFFER_SIZE 32768
#define LISTEN_BACKLOG 16
#define POLL_TIMEOUT_MS 500

struct connection
{
    int client;
    int target;
    bool client_open;
    bool target_open;
    bool target_write_shut;
    bool client_write_shut;
    unsigned char to_target[BUFFER_SIZE];
    size_t to_target_len;
    unsigned char to_client[BUFFER_SIZE];
    size_t to_client_len;
};

struct pollref
{
    int connection;
    int side;
};

static struct connection connections[MAX_CONNECTIONS];
static volatile sig_atomic_t stopping;

static void on_signal(int number)
{
    (void)number;
    stopping = 1;
}

static void close_connection(struct connection *connection)
{
    if (connection->client >= 0) close(connection->client);
    if (connection->target >= 0) close(connection->target);
    memset(connection, 0, sizeof(*connection));
    connection->client = -1;
    connection->target = -1;
}

static void close_connections(void)
{
    int index;
    for (index = 0; index < MAX_CONNECTIONS; index++) close_connection(&connections[index]);
}

// setgroups must run before setgid, and setgid before setuid; each failure is fatal. The second check catches a setuid
// that "succeeded" without changing the effective credentials, so the relay never handles a game connection as root.
static int drop_to_nobody(void)
{
    struct passwd *account = getpwnam("nobody");
    uid_t real_uid, effective_uid, saved_uid;
    gid_t real_gid, effective_gid, saved_gid;
    if (account == NULL)
    {
        fputs("capture-relay: the 'nobody' account was not found.\n", stderr);
        return -1;
    }
    if (setgroups(0, NULL) != 0)
    {
        perror("capture-relay: setgroups");
        return -1;
    }
    if (setgid(account->pw_gid) != 0)
    {
        perror("capture-relay: setgid");
        return -1;
    }
    if (setuid(account->pw_uid) != 0)
    {
        perror("capture-relay: setuid");
        return -1;
    }
    if (getresuid(&real_uid, &effective_uid, &saved_uid) != 0 ||
        getresgid(&real_gid, &effective_gid, &saved_gid) != 0 ||
        real_uid != account->pw_uid || effective_uid != account->pw_uid || saved_uid != account->pw_uid ||
        real_gid != account->pw_gid || effective_gid != account->pw_gid || saved_gid != account->pw_gid)
    {
        fputs("capture-relay: dropping to nobody did not take effect.\n", stderr);
        return -1;
    }
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    {
        perror("capture-relay: PR_SET_NO_NEW_PRIVS");
        return -1;
    }
    return 0;
}

static int listen_loopback(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
    {
        perror("capture-relay: socket");
        return -1;
    }
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0)
    {
        perror("capture-relay: SO_REUSEADDR");
        close(fd);
        return -1;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
    {
        perror("capture-relay: binding 127.0.0.1:443");
        close(fd);
        return -1;
    }
    if (listen(fd, LISTEN_BACKLOG) != 0)
    {
        perror("capture-relay: listen");
        close(fd);
        return -1;
    }
    return fd;
}

// The proxy listens on loopback, so a blocking connect finishes immediately even when nothing accepts (ECONNREFUSED).
// The connection is nonblocking afterwards; the relay never waits on one client while another needs service.
static int connect_target(void)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(TARGET_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
    {
        close(fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int free_slot(void)
{
    int index;
    for (index = 0; index < MAX_CONNECTIONS; index++)
        if (connections[index].client < 0) return index;
    return -1;
}

static void add_client(int client)
{
    int slot = free_slot();
    if (slot < 0)
    {
        // The table is the bound: more clients are closed immediately instead of growing the relay.
        close(client);
        return;
    }
    int target = connect_target();
    if (target < 0)
    {
        close(client);
        return;
    }
    struct connection *connection = &connections[slot];
    memset(connection, 0, sizeof(*connection));
    connection->client = client;
    connection->target = target;
    connection->client_open = true;
    connection->target_open = true;
}

static void accept_clients(int listener)
{
    for (;;)
    {
        int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            perror("capture-relay: accept");
            return;
        }
        add_client(client);
    }
}

static void compact(unsigned char *buffer, size_t *length, size_t used)
{
    if (used >= *length)
    {
        *length = 0;
        return;
    }
    memmove(buffer, buffer + used, *length - used);
    *length -= used;
}

// 0: done, -1: this connection must close. The read side is marked closed at EOF; the buffered bytes still go out.
static int receive_client(struct connection *connection)
{
    if (!connection->client_open || connection->to_target_len >= BUFFER_SIZE) return 0;
    ssize_t got = recv(connection->client, connection->to_target + connection->to_target_len,
        BUFFER_SIZE - connection->to_target_len, 0);
    if (got > 0)
    {
        connection->to_target_len += (size_t)got;
        return 0;
    }
    if (got == 0)
    {
        connection->client_open = false;
        return 0;
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
}

static int receive_target(struct connection *connection)
{
    if (!connection->target_open || connection->to_client_len >= BUFFER_SIZE) return 0;
    ssize_t got = recv(connection->target, connection->to_client + connection->to_client_len,
        BUFFER_SIZE - connection->to_client_len, 0);
    if (got > 0)
    {
        connection->to_client_len += (size_t)got;
        return 0;
    }
    if (got == 0)
    {
        connection->target_open = false;
        return 0;
    }
    return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
}

static int send_pending(int fd, unsigned char *buffer, size_t *length)
{
    ssize_t sent = send(fd, buffer, *length, MSG_NOSIGNAL);
    if (sent > 0)
    {
        compact(buffer, length, (size_t)sent);
        return 0;
    }
    return sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR ? -1 : 0;
}

static int shutdown_write(int fd, bool *done)
{
    if (shutdown(fd, SHUT_WR) != 0 && errno != ENOTCONN && errno != EINVAL) return -1;
    *done = true;
    return 0;
}

// A read side that reached EOF is passed on only after its buffered bytes are delivered, so a client that sent a
// request and closed gets its whole response instead of a half-close racing the last bytes.
static int update_shutdowns(struct connection *connection)
{
    if (!connection->client_open && connection->to_target_len == 0 && !connection->target_write_shut &&
        shutdown_write(connection->target, &connection->target_write_shut) != 0) return -1;
    if (!connection->target_open && connection->to_client_len == 0 && !connection->client_write_shut &&
        shutdown_write(connection->client, &connection->client_write_shut) != 0) return -1;
    return 0;
}

static void service_connection(struct connection *connection, short client_events, short target_events)
{
    if (connection->client < 0) return;
    if ((client_events | target_events) & (POLLERR | POLLNVAL))
    {
        close_connection(connection);
        return;
    }
    if ((client_events & (POLLIN | POLLHUP)) && receive_client(connection) != 0)
    {
        close_connection(connection);
        return;
    }
    if ((target_events & (POLLIN | POLLHUP)) && receive_target(connection) != 0)
    {
        close_connection(connection);
        return;
    }
    if ((client_events & POLLOUT) && send_pending(connection->client, connection->to_client,
            &connection->to_client_len) != 0)
    {
        close_connection(connection);
        return;
    }
    if ((target_events & POLLOUT) && send_pending(connection->target, connection->to_target,
            &connection->to_target_len) != 0)
    {
        close_connection(connection);
        return;
    }
    if (update_shutdowns(connection) != 0)
    {
        close_connection(connection);
        return;
    }
    if (!connection->client_open && !connection->target_open &&
        connection->to_target_len == 0 && connection->to_client_len == 0)
        close_connection(connection);
}

static void add_connection_pollfds(int index, struct pollfd *fds, struct pollref *refs, nfds_t *count)
{
    struct connection *connection = &connections[index];

    refs[*count].connection = index;
    refs[*count].side = 0;
    fds[*count].fd = connection->client;
    fds[*count].events = 0;
    fds[*count].revents = 0;
    if (connection->client_open && connection->to_target_len < BUFFER_SIZE) fds[*count].events |= POLLIN;
    if (connection->to_client_len > 0) fds[*count].events |= POLLOUT;
    (*count)++;

    refs[*count].connection = index;
    refs[*count].side = 1;
    fds[*count].fd = connection->target;
    fds[*count].events = 0;
    fds[*count].revents = 0;
    if (connection->target_open && connection->to_client_len < BUFFER_SIZE) fds[*count].events |= POLLIN;
    if (connection->to_target_len > 0) fds[*count].events |= POLLOUT;
    (*count)++;
}

static nfds_t build_pollfds(int listener, struct pollfd *fds, struct pollref *refs)
{
    nfds_t count = 1;
    int index;
    fds[0].fd = listener;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    for (index = 0; index < MAX_CONNECTIONS; index++)
    {
        if (connections[index].client < 0) continue;
        add_connection_pollfds(index, fds, refs, &count);
    }
    return count;
}

static void collect_events(const struct pollfd *fds, const struct pollref *refs, nfds_t count,
    short *client_events, short *target_events)
{
    nfds_t at;
    for (at = 1; at < count; at++)
    {
        if (refs[at].side == 0) client_events[refs[at].connection] = fds[at].revents;
        else target_events[refs[at].connection] = fds[at].revents;
    }
}

static void service_connections(const short *client_events, const short *target_events)
{
    int index;
    for (index = 0; index < MAX_CONNECTIONS; index++)
        if (connections[index].client >= 0)
            service_connection(&connections[index], client_events[index], target_events[index]);
}

static int run(int listener)
{
    while (!stopping)
    {
        struct pollfd fds[1 + MAX_CONNECTIONS * 2];
        struct pollref refs[1 + MAX_CONNECTIONS * 2];
        short client_events[MAX_CONNECTIONS];
        short target_events[MAX_CONNECTIONS];
        memset(client_events, 0, sizeof(client_events));
        memset(target_events, 0, sizeof(target_events));
        nfds_t count = build_pollfds(listener, fds, refs);
        int ready = poll(fds, count, POLL_TIMEOUT_MS);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            perror("capture-relay: poll");
            return 1;
        }
        if (fds[0].revents & POLLIN) accept_clients(listener);
        collect_events(fds, refs, count, client_events, target_events);
        service_connections(client_events, target_events);
    }
    return 0;
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc != 1)
    {
        fputs("usage: capture-relay (it takes no arguments: 127.0.0.1:443 -> 127.0.0.1:18443)\n", stderr);
        return 2;
    }
    int index;
    for (index = 0; index < MAX_CONNECTIONS; index++)
    {
        connections[index].client = -1;
        connections[index].target = -1;
    }
    signal(SIGPIPE, SIG_IGN);
    if (signal(SIGTERM, on_signal) == SIG_ERR || signal(SIGINT, on_signal) == SIG_ERR)
    {
        perror("capture-relay: signal");
        return 1;
    }
    // Do not leave a root-started process with its working directory in the Wine-writable installation.
    if (chdir("/") != 0)
    {
        perror("capture-relay: chdir /");
        return 1;
    }
    int listener = listen_loopback(LISTEN_PORT);
    if (listener < 0) return 1;
    if (drop_to_nobody() != 0)
    {
        close(listener);
        return 1;
    }
    fprintf(stderr, "capture-relay: 127.0.0.1:%d -> 127.0.0.1:%d as nobody\n", LISTEN_PORT, TARGET_PORT);
    int status = run(listener);
    close(listener);
    close_connections();
    return status;
}
