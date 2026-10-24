/*
** Copyright 2021, Corellium, LLC
** Copyright 2010, Adam Shanks (@ChainsDD)
** Copyright 2008, Zinx Verituse (@zinxv)
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define _GNU_SOURCE /* for unshare() */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <stdint.h>
#include <pwd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <sys/types.h>
#include <pthread.h>
#include <sched.h>
#include <termios.h>
#include <signal.h>
#include <string.h>

#include "su.h"
#include "utils.h"
#include "pts.h"

int is_daemon = 0;
int daemon_from_uid = 0;
int daemon_from_pid = 0;

// Count only the listener's direct children (one worker per authorized request).
// A worker stays alive until its root command has exited.
#define MAX_ACTIVE_WORKERS 16
static volatile sig_atomic_t active_workers = 0;

// Called only in the listener. The worker restores SIGCHLD to SIG_DFL.
static void reap_workers(int signo) {
    (void)signo;
    int saved_errno = errno;
    for (;;) {
        pid_t pid = waitpid(-1, NULL, WNOHANG);
        if (pid > 0) {
            if (active_workers > 0)
                --active_workers;
            continue;
        }
        if (pid < 0 && errno == EINTR)
            continue;
        break;
    }
    errno = saved_errno;
}

// Constants for the atty bitfield
#define ATTY_IN     1
#define ATTY_OUT    2
#define ATTY_ERR    4

/*
 * Receive a file descriptor from a Unix socket.
 * Contributed by @mkasick
 *
 * Returns the file descriptor on success, or -1 if a file
 * descriptor was not actually included in the message
 *
 * On error the function terminates by calling exit(-1)
 */
static int recv_fd(int sockfd) {
    // Need to receive data from the message, otherwise don't care about it.
    char iovbuf;

    struct iovec iov = {
        .iov_base = &iovbuf,
        .iov_len  = 1,
    };

    char cmsgbuf[CMSG_SPACE(sizeof(int))];

    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsgbuf,
        .msg_controllen = sizeof(cmsgbuf),
    };

    if (recvmsg(sockfd, &msg, MSG_WAITALL | MSG_CMSG_CLOEXEC) != 1) {
        goto error;
    }

    if (msg.msg_flags & MSG_CTRUNC) {
        LOGE("truncated SCM_RIGHTS control message");
        goto error;
    }

    // Was a control message actually sent?
    switch (msg.msg_controllen) {
    case 0:
        // No, so the file descriptor was closed and won't be used.
        return -1;
    case sizeof(cmsgbuf):
        // Yes, grab the file descriptor from it.
        break;
    default:
        goto error;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

    if (cmsg             == NULL                  ||
        cmsg->cmsg_len   != CMSG_LEN(sizeof(int)) ||
        cmsg->cmsg_level != SOL_SOCKET            ||
        cmsg->cmsg_type  != SCM_RIGHTS) {
error:
        LOGE("unable to read fd");
        exit(-1);
    }

    return *(int *)CMSG_DATA(cmsg);
}

/*
 * Send a file descriptor through a Unix socket.
 * Contributed by @mkasick
 *
 * On error the function terminates by calling exit(-1)
 *
 * fd may be -1, in which case the dummy data is sent,
 * but no control message with the FD is sent.
 */
static void send_fd(int sockfd, int fd) {
    // Need to send some data in the message, this will do.
    struct iovec iov = {
        .iov_base = "",
        .iov_len  = 1,
    };

    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
    };

    char cmsgbuf[CMSG_SPACE(sizeof(int))];

    if (fd != -1) {
        // Is the file descriptor actually open?
        if (fcntl(fd, F_GETFD) == -1) {
            if (errno != EBADF) {
                goto error;
            }
            // It's closed, don't send a control message or sendmsg will EBADF.
        } else {
            // It's open, send the file descriptor in a control message.
            msg.msg_control    = cmsgbuf;
            msg.msg_controllen = sizeof(cmsgbuf);

            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

            cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type  = SCM_RIGHTS;

            *(int *)CMSG_DATA(cmsg) = fd;
        }
    }

    if (sendmsg(sockfd, &msg, MSG_NOSIGNAL) != 1) {
error:
        PLOGE("unable to send fd");
        exit(-1);
    }
}

// Unix SOCK_STREAM does not guarantee that one read/write transfers
// the entire requested buffer. Handle short operations and EINTR.
static ssize_t read_full(int fd, void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, (char *)buf + done, len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return 0; // EOF, including a truncated message
        done += (size_t)n;
    }
    return (ssize_t)done;
}

static ssize_t write_full(int fd, const void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = send(fd, (const char *)buf + done, len - done,
                         MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO; // Prevent an infinite loop
            return -1;
        }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

// Per-call protection for both sides of the client/daemon socket.
// A separate alarm below limits the entire request-protocol phase.
#define SOCKET_IO_TIMEOUT_SEC 15
#define REQUEST_PROTOCOL_TIMEOUT_SEC 15

static int set_socket_io_timeouts(int fd) {
    const struct timeval timeout = {
        .tv_sec = SOCKET_IO_TIMEOUT_SEC,
        .tv_usec = 0,
    };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) != 0) {
        PLOGE("SO_RCVTIMEO");
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) != 0) {
        PLOGE("SO_SNDTIMEO");
        return -1;
    }
    return 0;
}

static int read_int(int fd) {
    int val;
    ssize_t len = read_full(fd, &val, sizeof(val));
    if (len != (ssize_t)sizeof(val)) {
        LOGE("unable to read int: %zd", len);
        exit(-1);
    }
    return val;
}

static void write_int(int fd, int val) {
    ssize_t written = write_full(fd, &val, sizeof(val));
    if (written != (ssize_t)sizeof(val)) {
        PLOGE("unable to write int");
        exit(-1);
    }
}

static char* read_string(int fd) {
    int len = read_int(fd);
    if (len > PATH_MAX || len < 0) {
        LOGE("invalid string length %d", len);
        exit(-1);
    }
    char* val = malloc(sizeof(char) * (len + 1));
    if (val == NULL) {
        LOGE("unable to malloc string");
        exit(-1);
    }
    val[len] = '\0';
    ssize_t amount = read_full(fd, val, (size_t)len);
    if (amount != len) {
        LOGE("unable to read string");
        exit(-1);
    }
    return val;
}

static void write_string(int fd, char* val) {
    int len = strlen(val);
    write_int(fd, len);
    ssize_t written = write_full(fd, val, (size_t)len);
    if (written != len) {
        PLOGE("unable to write string");
        exit(-1);
    }
}

static int run_daemon_child(int infd, int outfd, int errfd, int argc, char** argv) {
    if (-1 == dup2(outfd, STDOUT_FILENO)) {
        PLOGE("dup2 child outfd");
        exit(-1);
    }

    if (-1 == dup2(errfd, STDERR_FILENO)) {
        PLOGE("dup2 child errfd");
        exit(-1);
    }

    if (-1 == dup2(infd, STDIN_FILENO)) {
        PLOGE("dup2 child infd");
        exit(-1);
    }

    close(infd);
    close(outfd);
    close(errfd);

    return su_main(argc, argv, 0);
}

static int daemon_accept(int fd, uid_t peer_uid, pid_t peer_pid) {
    is_daemon = 1;

    // The listener already verified the kernel-provided peer credentials.
    // Never use the client-supplied UID or PID for authorization.
    daemon_from_uid = peer_uid;
    daemon_from_pid = peer_pid;

    // A hard wall-clock limit, not reset by trickled client data.
    // This process handles only the request protocol until the alarm is
    // cancelled below; long-running root commands must remain unaffected.
    alarm(REQUEST_PROTOCOL_TIMEOUT_SEC);

    // Authorize before reading even the first client-controlled field.
    write_int(fd, 1);

    int reported_pid = read_int(fd);
    LOGD("reported client pid: %d", reported_pid);
    char *pts_slave = read_string(fd);
    LOGD("remote pts_slave: %s", pts_slave);
    int reported_uid = read_int(fd);
    int reported_request_pid = read_int(fd);
    LOGD("reported uid: %d, request pid: %d",
         reported_uid, reported_request_pid);

    // The FDs for each of the streams
    int infd  = recv_fd(fd);
    int outfd = recv_fd(fd);
    int errfd = recv_fd(fd);

    int argc = read_int(fd);
    if (argc < 0 || argc > 512) {
        LOGE("unable to allocate args: %d", argc);
        exit(-1);
    }
    LOGD("remote args: %d", argc);
    char** argv = (char**)malloc(sizeof(char*) * (argc + 1));
    if (argv == NULL) {
        LOGE("unable to allocate argv");
        exit(-1);
    }
    argv[argc] = NULL;
    int i;
    for (i = 0; i < argc; i++) {
        argv[i] = read_string(fd);
    }

    // ack
    write_int(fd, 1);

    // Protocol complete. Do not kill legitimate long-running commands.
    alarm(0);

    // Fork the child process. The fork has to happen before calling
    // setsid() and opening the pseudo-terminal so that the parent
    // is not affected
    int child = fork();
    if (child < 0) {
        // fork failed, send a return code and bail out
        PLOGE("unable to fork");
        if (write_full(fd, &child, sizeof(child)) != sizeof(child))
            PLOGE("unable to send fork failure");
        close(fd);
        return child;
    }

    if (child != 0) {
        // In parent, wait for the child to exit, and send the exit code
        // across the wire.
        int status, code;

        free(pts_slave);

        LOGD("waiting for child exit");
        if (waitpid(child, &status, 0) > 0) {
            if (WIFEXITED(status)) {
                code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                code = 128 + WTERMSIG(status);
            } else {
                code = -1;
            }
        } else {
            code = -1;
        }

        // Pass the return code back to the client
        LOGD("sending code");
        if (write_full(fd, &code, sizeof(code)) != sizeof(code)) {
            PLOGE("unable to write exit code");
        }

        close(fd);
        LOGD("child exited");
        return code;
    }

    // We are in the child now
    // Close the unix socket file descriptor
    close (fd);

    // Become session leader
    if (setsid() == (pid_t) -1) {
        PLOGE("setsid");
    }

    int ptsfd;
    if (pts_slave[0]) {
        // Opening the TTY has to occur after the
        // fork() and setsid() so that it becomes
        // our controlling TTY and not the daemon's
        ptsfd = open(pts_slave, O_RDWR);
        if (ptsfd == -1) {
            PLOGE("open(pts_slave) daemon");
            exit(-1);
        }

        if (infd < 0)  {
            LOGD("daemon: stdin using PTY");
            infd  = ptsfd;
        }
        if (outfd < 0) {
            LOGD("daemon: stdout using PTY");
            outfd = ptsfd;
        }
        if (errfd < 0) {
            LOGD("daemon: stderr using PTY");
            errfd = ptsfd;
        }
    } else {
        // If a TTY was sent directly, make it the CTTY.
        if (isatty(infd)) {
            ioctl(infd, TIOCSCTTY, 1);
        }
    }
    free(pts_slave);

    return run_daemon_child(infd, outfd, errfd, argc, argv);
}

int run_daemon() {
    if (getuid() != 0 || getgid() != 0) {
        PLOGE("daemon requires root. uid/gid not root");
        return -1;
    }

    int fd;
    struct sockaddr_un sun;

    fd = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (fd < 0) {
        PLOGE("socket");
        return -1;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC)) {
        PLOGE("fcntl FD_CLOEXEC");
        goto err;
    }

    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_LOCAL;
    memcpy(sun.sun_path, "\0" "SUPERUSER", strlen("SUPERUSER") + 1);

    if (bind(fd, (struct sockaddr*)&sun, sizeof(sun)) < 0) {
        PLOGE("daemon bind");
        goto err;
    }

    if (listen(fd, 10) < 0) {
        PLOGE("daemon listen");
        goto err;
    }

    // Only the listener reaps workers. Workers must wait for their own
    // command child using waitpid() in daemon_accept().
    struct sigaction chld_action = {0};
    chld_action.sa_handler = reap_workers;
    chld_action.sa_flags = SA_NOCLDSTOP; // accept already handles EINTR
    sigemptyset(&chld_action.sa_mask);
    if (sigaction(SIGCHLD, &chld_action, NULL) < 0) {
        PLOGE("sigaction SIGCHLD");
        goto err;
    }

    sigset_t chld_mask;
    sigemptyset(&chld_mask);
    sigaddset(&chld_mask, SIGCHLD);
    if (sigprocmask(SIG_UNBLOCK, &chld_mask, NULL) < 0) {
        PLOGE("unblock SIGCHLD");
        goto err;
    }

    for (;;) {
        int client = accept(fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR)
                continue;
            PLOGE("accept");
            break;
        }

        // Deny slow or non-reading peers before allocating a worker.
        if (set_socket_io_timeouts(client) != 0) {
            close(client);
            continue;
        }

        // Check kernel credentials BEFORE forking or reading client data.
        struct ucred credentials;
        socklen_t credentials_len = sizeof(credentials);
        if (getsockopt(client, SOL_SOCKET, SO_PEERCRED,
                       &credentials, &credentials_len) != 0 ||
            credentials_len != sizeof(credentials)) {
            LOGE("unable to obtain complete SO_PEERCRED credentials");
            close(client);
            continue;
        }

        if (!uid_allowed(credentials.uid)) {
            // Avoid blocking the main listener on an untrusted peer.
            const int denied = 0;
            LOGW("sud: rejected uid %u before fork", (unsigned)credentials.uid);
            (void)send(client, &denied, sizeof(denied),
                       MSG_DONTWAIT | MSG_NOSIGNAL);
            close(client);
            continue;
        }

        // Block SIGCHLD until the new worker is recorded: no counter race.
        sigset_t old_mask;
        if (sigprocmask(SIG_BLOCK, &chld_mask, &old_mask) < 0) {
            PLOGE("block SIGCHLD");
            close(client);
            break;
        }

        if (active_workers >= MAX_ACTIVE_WORKERS) {
            const int busy = -1;
            LOGW("sud: worker limit reached (%d)", MAX_ACTIVE_WORKERS);
            (void)send(client, &busy, sizeof(busy), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (sigprocmask(SIG_SETMASK, &old_mask, NULL) < 0) {
                PLOGE("restore signal mask");
                close(client);
                break;
            }
            close(client);
            continue;
        }

        pid_t worker = fork();
        if (worker == 0) {
            // Never inherit the listener's reaper: it could consume the
            // exit status that daemon_accept() needs from waitpid().
            struct sigaction default_chld = {0};
            default_chld.sa_handler = SIG_DFL;
            sigemptyset(&default_chld.sa_mask);
            if (sigaction(SIGCHLD, &default_chld, NULL) < 0 ||
                sigprocmask(SIG_SETMASK, &old_mask, NULL) < 0) {
                _exit(EXIT_FAILURE);
            }
            close(fd);
            return daemon_accept(client, credentials.uid, credentials.pid);
        }
        if (worker > 0)
            ++active_workers;
        if (sigprocmask(SIG_SETMASK, &old_mask, NULL) < 0) {
            PLOGE("restore signal mask");
            close(client);
            break;
        }
        if (worker < 0)
            PLOGE("fork worker");
        close(client);
    }

    LOGE("daemon exiting");
err:
    close(fd);
    return -1;
}

// List of signals which cause process termination
static int quit_signals[] = { SIGALRM, SIGHUP, SIGPIPE, SIGQUIT, SIGTERM, SIGINT, 0 };

static void sighandler(int sig) {
    LOGE("Caught sig %d", sig);
    restore_stdin();

    // Assume we'll only be called before death
    // See note before sigaction() in set_stdin_raw()
    //
    // Now, close all standard I/O to cause the pumps
    // to exit so we can continue and retrieve the exit
    // code
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // Put back all the default handlers
    struct sigaction act;
    int i;

    memset(&act, '\0', sizeof(act));
    act.sa_handler = SIG_DFL;
    for (i = 0; quit_signals[i]; i++) {
        if (sigaction(quit_signals[i], &act, NULL) < 0) {
            PLOGE("Error removing signal handler");
            continue;
        }
    }
}

/**
 * Setup signal handlers trap signals which should result in program termination
 * so that we can restore the terminal to its normal state and retrieve the 
 * return code.
 */
static void setup_sighandlers(void) {
    struct sigaction act;
    int i;

    // Install the termination handlers
    // Note: we're assuming that none of these signal handlers are already trapped.
    // If they are, we'll need to modify this code to save the previous handler and
    // call it after we restore stdin to its previous state.
    memset(&act, '\0', sizeof(act));
    act.sa_handler = &sighandler;
    for (i = 0; quit_signals[i]; i++) {
        if (sigaction(quit_signals[i], &act, NULL) < 0) {
            PLOGE("Error installing signal handler");
            continue;
        }
    }
}

int connect_daemon(int argc, char *argv[], int ppid) {
    int uid = getuid();
    int ptmx;
    char pts_slave[PATH_MAX];

    struct sockaddr_un sun;

    // Open a socket to the daemon
    int socketfd = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (socketfd < 0) {
        PLOGE("socket");
        exit(-1);
    }
    if (fcntl(socketfd, F_SETFD, FD_CLOEXEC)) {
        PLOGE("fcntl FD_CLOEXEC");
        exit(-1);
    }

    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_LOCAL;
    memcpy(sun.sun_path, "\0" "SUPERUSER", strlen("SUPERUSER") + 1);

    if (0 != connect(socketfd, (struct sockaddr*)&sun, sizeof(sun))) {
        PLOGE("connect");
        exit(-1);
    }

    if (set_socket_io_timeouts(socketfd) != 0) {
        close(socketfd);
        return EXIT_FAILURE;
    }

    // Wait for the listener's authorization before sending any client data.
    int authorized = read_int(socketfd);
    if (authorized != 1) {
        // -1 means capacity exhausted; all other values mean denied.
        fprintf(stderr, "%s\n", strerror(authorized == -1 ? EAGAIN : EACCES));
        close(socketfd);
        return EXIT_FAILURE;
    }

    LOGD("connecting client %d", getpid());

    // Determine which one of our streams are attached to a TTY
    int atty = 0;

    // Send TTYs directly (instead of proxying with a PTY) if
    // the SUPERUSER_SEND_TTY environment variable is set.
    if (getenv("SUPERUSER_SEND_TTY") == NULL) {
        if (isatty(STDIN_FILENO))  atty |= ATTY_IN;
        if (isatty(STDOUT_FILENO)) atty |= ATTY_OUT;
        if (isatty(STDERR_FILENO)) atty |= ATTY_ERR;
    }

    if (atty) {
        // We need a PTY. Get one.
        ptmx = pts_open(pts_slave, sizeof(pts_slave));
        if (ptmx < 0) {
            PLOGE("pts_open");
            exit(-1);
        }
    } else {
        pts_slave[0] = '\0';
    }

    // Send some info to the daemon, starting with our PID
    write_int(socketfd, getpid());
    // Send the slave path to the daemon
    // (This is "" if we're not using PTYs)
    write_string(socketfd, pts_slave);
    // User ID
    write_int(socketfd, uid);
    // Parent PID
    write_int(socketfd, ppid);

    // Send stdin
    if (atty & ATTY_IN) {
        // Using PTY
        send_fd(socketfd, -1);
    } else {
        send_fd(socketfd, STDIN_FILENO);
    }

    // Send stdout
    if (atty & ATTY_OUT) {
        // Forward SIGWINCH
        watch_sigwinch_async(STDOUT_FILENO, ptmx);

        // Using PTY
        send_fd(socketfd, -1);
    } else {
        send_fd(socketfd, STDOUT_FILENO);
    }

    // Send stderr
    if (atty & ATTY_ERR) {
        // Using PTY
        send_fd(socketfd, -1);
    } else {
        send_fd(socketfd, STDERR_FILENO);
    }

    // Number of command line arguments
    write_int(socketfd, argc);

    // Command line arguments
    int i;
    for (i = 0; i < argc; i++) {
        write_string(socketfd, argv[i]);
    }

    // Wait for acknowledgement from daemon
    read_int(socketfd);

    // The request protocol is complete. Waiting for the exit status can
    // legitimately take hours; do not time out long-running root commands.
    const struct timeval no_recv_timeout = { .tv_sec = 0, .tv_usec = 0 };
    if (setsockopt(socketfd, SOL_SOCKET, SO_RCVTIMEO,
                   &no_recv_timeout, sizeof(no_recv_timeout)) != 0) {
        PLOGE("clear SO_RCVTIMEO");
        close(socketfd);
        return EXIT_FAILURE;
    }

    if (atty & ATTY_IN) {
        setup_sighandlers();
        pump_stdin_async(ptmx);
    }
    if (atty & ATTY_OUT) {
        pump_stdout_blocking(ptmx);
    }

    // Get the exit code
    int code = read_int(socketfd);
    close(socketfd);
    LOGD("client exited %d", code);

    return code;
}
