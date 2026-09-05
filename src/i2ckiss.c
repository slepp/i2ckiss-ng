/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Stephen Olesen
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/un.h>

#define PROGRAM_NAME "i2ckiss"
#ifndef PROGRAM_VERSION
#define PROGRAM_VERSION "2.0.0"
#endif
#define KISS_FEND 0xc0
#define KISS_FESC 0xdb
#define KISS_TFEND 0xdc
#define KISS_TFESC 0xdd
#define TNC_NO_DATA 0x0e
#define TNC_NAK 0x15
#define MAX_KISS_FRAME 4096U
#define PTY_QUEUE_SIZE 65536U
#define I2C_QUEUE_SIZE 65536U
#define READ_CHUNK 1024U

enum endpoint_mode {
    ENDPOINT_SYMLINK,
    ENDPOINT_KISSATTACH,
};

struct config {
    char i2c_path[PATH_MAX];
    char bus_name[128];
    int address;
    int poll_ms;
    int byte_delay_us;
    int retry_min_ms;
    int retry_max_ms;
    int i2c_timeout;
    int i2c_retries;
    int reset_delay_ms;
    int mtu;
    int diagnostics;
    bool use_syslog;
    bool daemonize;
    bool reset_tnc;
    enum endpoint_mode endpoint_mode;
    const char *endpoint_arg1;
    const char *endpoint_arg2;
    const char *lock_dir;
};

struct stats {
    unsigned long pty_rx_frames;
    unsigned long pty_rx_bytes;
    unsigned long pty_tx_frames;
    unsigned long pty_tx_bytes;
    unsigned long i2c_errors;
    unsigned long i2c_reconnects;
    unsigned long checksum_errors;
    unsigned long malformed_frames;
    unsigned long oversized_frames;
    unsigned long dropped_frames;
    unsigned long child_restarts;
};

struct kiss_decoder {
    uint8_t data[MAX_KISS_FRAME];
    size_t len;
    bool escaped;
    bool overflow;
    bool checksum;
};

enum decode_result {
    DECODE_NONE,
    DECODE_FRAME,
    DECODE_BAD_CHECKSUM,
    DECODE_MALFORMED,
    DECODE_OVERSIZED,
};

struct tx_frame {
    struct tx_frame *next;
    size_t len;
    size_t pos;
    uint8_t data[];
};

struct byte_queue {
    uint8_t data[PTY_QUEUE_SIZE];
    size_t head;
    size_t used;
};

struct pty_pair {
    int master_fd;
    int guard_fd;
    char slave_path[PATH_MAX];
};

struct app {
    struct config cfg;
    struct stats stats;
    struct pty_pair pty;
    int i2c_fd;
    int lock_fd;
    char lock_path[PATH_MAX];
    bool link_owned;
    char link_path[PATH_MAX];
    pid_t child_pid;
    int child_backoff_ms;
    int64_t child_restart_at;
    int reconnect_backoff_ms;
    int64_t reconnect_at;
    int64_t next_poll_at;
    struct kiss_decoder from_pty;
    struct kiss_decoder from_i2c;
    struct tx_frame *tx_head;
    struct tx_frame *tx_tail;
    size_t tx_queued_bytes;
    struct byte_queue pty_output;
};

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t report_requested;
static volatile sig_atomic_t child_changed;
static bool log_to_syslog;
static int log_verbosity;

static int64_t monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int milliseconds)
{
    struct timespec requested;

    if (milliseconds <= 0)
        return;
    requested.tv_sec = milliseconds / 1000;
    requested.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&requested, &requested) < 0 && errno == EINTR) {
        if (stop_requested)
            break;
    }
}

static void sleep_us(int microseconds)
{
    struct timespec requested;

    if (microseconds <= 0)
        return;
    requested.tv_sec = microseconds / 1000000;
    requested.tv_nsec = (long)(microseconds % 1000000) * 1000L;
    while (nanosleep(&requested, &requested) < 0 && errno == EINTR) {
        if (stop_requested)
            break;
    }
}

static void log_message(int priority, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    if (log_to_syslog) {
        va_list copy;
        va_copy(copy, ap);
        vsyslog(priority, format, copy);
        va_end(copy);
    }
    if (!log_to_syslog || priority <= LOG_ERR || log_verbosity > 0) {
        fprintf(stderr, "%s: ", PROGRAM_NAME);
        vfprintf(stderr, format, ap);
        fputc('\n', stderr);
    }
    va_end(ap);
}

static void signal_handler(int signal_number)
{
    if (signal_number == SIGUSR1)
        report_requested = 1;
    else if (signal_number == SIGCHLD)
        child_changed = 1;
    else
        stop_requested = 1;
}

static void notify_systemd(const char *message)
{
    const char *path = getenv("NOTIFY_SOCKET");
    struct sockaddr_un address;
    socklen_t address_length;
    size_t path_length;
    int fd;

    if (path == NULL || *path == '\0')
        return;
    path_length = strlen(path);
    if (path_length >= sizeof(address.sun_path))
        return;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (path[0] == '@') {
        address.sun_path[0] = '\0';
        memcpy(address.sun_path + 1, path + 1, path_length - 1);
        address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                         path_length);
    } else {
        memcpy(address.sun_path, path, path_length + 1);
        address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                         path_length + 1);
    }
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    (void)sendto(fd, message, strlen(message), MSG_NOSIGNAL,
                 (const struct sockaddr *)&address, address_length);
    close(fd);
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) < 0 ||
        sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGHUP, &action, NULL) < 0 ||
        sigaction(SIGUSR1, &action, NULL) < 0 ||
        sigaction(SIGCHLD, &action, NULL) < 0)
        return -1;

    signal(SIGPIPE, SIG_IGN);
    return 0;
}

static void decoder_reset(struct kiss_decoder *decoder)
{
    decoder->len = 0;
    decoder->escaped = false;
    decoder->overflow = false;
}

static enum decode_result decoder_finish(const struct kiss_decoder *decoder,
                                         size_t *frame_length)
{
    if (decoder->overflow)
        return DECODE_OVERSIZED;
    if (decoder->escaped)
        return DECODE_MALFORMED;
    if (decoder->len == 0)
        return DECODE_NONE;

    if (decoder->checksum) {
        uint8_t checksum = 0;

        if (decoder->len < 2)
            return DECODE_MALFORMED;
        for (size_t i = 0; i < decoder->len; ++i)
            checksum ^= decoder->data[i];
        if (checksum != 0)
            return DECODE_BAD_CHECKSUM;
    }
    *frame_length = decoder->len - (decoder->checksum ? 1U : 0U);
    return DECODE_FRAME;
}

static enum decode_result decoder_feed(struct kiss_decoder *decoder,
                                       uint8_t byte, size_t *frame_length)
{
    uint8_t value = byte;

    *frame_length = 0;
    if (byte == KISS_FEND) {
        enum decode_result result = decoder_finish(decoder, frame_length);

        decoder_reset(decoder);
        return result;
    }

    if (decoder->overflow)
        return DECODE_NONE;

    if (decoder->escaped) {
        decoder->escaped = false;
        if (byte == KISS_TFEND)
            value = KISS_FEND;
        else if (byte == KISS_TFESC)
            value = KISS_FESC;
        else
            return DECODE_MALFORMED;
    } else if (byte == KISS_FESC) {
        decoder->escaped = true;
        return DECODE_NONE;
    }

    if (decoder->len == MAX_KISS_FRAME) {
        decoder->overflow = true;
        return DECODE_NONE;
    }
    decoder->data[decoder->len++] = value;
    return DECODE_NONE;
}

static size_t append_escaped(uint8_t *output, size_t offset, uint8_t byte)
{
    if (byte == KISS_FEND) {
        output[offset++] = KISS_FESC;
        output[offset++] = KISS_TFEND;
    } else if (byte == KISS_FESC) {
        output[offset++] = KISS_FESC;
        output[offset++] = KISS_TFESC;
    } else {
        output[offset++] = byte;
    }
    return offset;
}

static size_t kiss_encode(const uint8_t *frame, size_t frame_length,
                          unsigned int port, bool checksum, uint8_t *output)
{
    uint8_t sum = 0;
    uint8_t command;
    size_t offset = 0;
    size_t i;

    if (frame_length == 0)
        return 0;
    command = (uint8_t)((frame[0] & 0x0fU) | ((port & 0x0fU) << 4));
    output[offset++] = KISS_FEND;
    offset = append_escaped(output, offset, command);
    sum ^= command;
    for (i = 1; i < frame_length; ++i) {
        offset = append_escaped(output, offset, frame[i]);
        sum ^= frame[i];
    }
    if (checksum)
        offset = append_escaped(output, offset, sum);
    output[offset++] = KISS_FEND;
    return offset;
}

static int byte_queue_push(struct byte_queue *queue, const uint8_t *data,
                           size_t length)
{
    size_t tail;
    size_t first;

    if (length > PTY_QUEUE_SIZE - queue->used) {
        errno = ENOBUFS;
        return -1;
    }
    tail = (queue->head + queue->used) % PTY_QUEUE_SIZE;
    first = PTY_QUEUE_SIZE - tail;
    if (first > length)
        first = length;
    memcpy(queue->data + tail, data, first);
    memcpy(queue->data, data + first, length - first);
    queue->used += length;
    return 0;
}

static int byte_queue_flush(struct byte_queue *queue, int fd)
{
    size_t available;
    ssize_t written;

    if (queue->used == 0)
        return 0;
    available = PTY_QUEUE_SIZE - queue->head;
    if (available > queue->used)
        available = queue->used;
    written = write(fd, queue->data + queue->head, available);
    if (written < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK ||
            errno == EIO)
            return 0;
        return -1;
    }
    queue->head = (queue->head + (size_t)written) % PTY_QUEUE_SIZE;
    queue->used -= (size_t)written;
    return 0;
}

static int enqueue_i2c_frame(struct app *app, const uint8_t *frame,
                             size_t frame_length)
{
    size_t maximum = frame_length * 2 + 4;
    struct tx_frame *queued;

    if (maximum > I2C_QUEUE_SIZE - app->tx_queued_bytes) {
        errno = ENOBUFS;
        return -1;
    }
    queued = malloc(sizeof(*queued) + maximum);
    if (queued == NULL)
        return -1;
    queued->next = NULL;
    queued->pos = 0;
    queued->len = kiss_encode(frame, frame_length, 0, true, queued->data);
    if (app->tx_tail != NULL)
        app->tx_tail->next = queued;
    else
        app->tx_head = queued;
    app->tx_tail = queued;
    app->tx_queued_bytes += queued->len;
    return 0;
}

static void free_tx_queue(struct app *app)
{
    while (app->tx_head != NULL) {
        struct tx_frame *next = app->tx_head->next;
        free(app->tx_head);
        app->tx_head = next;
    }
    app->tx_tail = NULL;
    app->tx_queued_bytes = 0;
}

static int pty_pair_open(struct pty_pair *pair)
{
    struct termios settings;

    pair->master_fd = -1;
    pair->guard_fd = -1;
    pair->master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (pair->master_fd < 0)
        return -1;
    if (grantpt(pair->master_fd) < 0 || unlockpt(pair->master_fd) < 0 ||
        ptsname_r(pair->master_fd, pair->slave_path,
                  sizeof(pair->slave_path)) != 0)
        goto fail;

    /* Holding the slave open keeps the PTY valid while clients restart. */
    pair->guard_fd = open(pair->slave_path,
                          O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (pair->guard_fd < 0)
        goto fail;
    if (tcgetattr(pair->guard_fd, &settings) < 0)
        goto fail;
    cfmakeraw(&settings);
    settings.c_cflag |= CLOCAL | CREAD;
    if (tcsetattr(pair->guard_fd, TCSANOW, &settings) < 0)
        goto fail;
    return 0;

fail:
    {
        int saved_errno = errno;
        if (pair->guard_fd >= 0)
            close(pair->guard_fd);
        if (pair->master_fd >= 0)
            close(pair->master_fd);
        pair->guard_fd = -1;
        pair->master_fd = -1;
        errno = saved_errno;
        return -1;
    }
}

static void pty_pair_close(struct pty_pair *pair)
{
    if (pair->guard_fd >= 0)
        close(pair->guard_fd);
    if (pair->master_fd >= 0)
        close(pair->master_fd);
    pair->guard_fd = -1;
    pair->master_fd = -1;
}

static bool pts_target(const char *target)
{
    const char *number;

    if (strncmp(target, "/dev/pts/", 9) != 0)
        return false;
    number = target + 9;
    if (*number == '\0')
        return false;
    while (*number != '\0') {
        if (!isdigit((unsigned char)*number))
            return false;
        ++number;
    }
    return true;
}

static int claim_symlink(struct app *app, const char *link_path)
{
    struct stat metadata;
    char old_target[PATH_MAX];
    char absolute_path[PATH_MAX];
    char working_directory[PATH_MAX];
    const char *claimed_path = link_path;
    ssize_t length;

    if (link_path[0] != '/') {
        if (getcwd(working_directory, sizeof(working_directory)) == NULL)
            return -1;
        if (snprintf(absolute_path, sizeof(absolute_path), "%s/%s",
                     working_directory, link_path) >= (int)sizeof(absolute_path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        claimed_path = absolute_path;
    }
    if (strncmp(claimed_path, "/dev/pts/", 9) == 0) {
        errno = EPERM;
        return -1;
    }
    if (strlen(claimed_path) >= sizeof(app->link_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (lstat(claimed_path, &metadata) == 0) {
        if (!S_ISLNK(metadata.st_mode)) {
            errno = EEXIST;
            return -1;
        }
        length = readlink(claimed_path, old_target, sizeof(old_target) - 1);
        if (length < 0)
            return -1;
        old_target[length] = '\0';
        if (strcmp(old_target, app->pty.slave_path) == 0) {
            app->link_owned = true;
            strcpy(app->link_path, claimed_path);
            return 0;
        }
        if (!pts_target(old_target) || stat(claimed_path, &metadata) == 0 ||
            errno != ENOENT) {
            errno = EEXIST;
            return -1;
        }

        /* Only an unresolvable /dev/pts/N link is safe to reclaim. */
        length = readlink(claimed_path, old_target, sizeof(old_target) - 1);
        if (length < 0)
            return -1;
        old_target[length] = '\0';
        if (!pts_target(old_target)) {
            errno = EEXIST;
            return -1;
        }
        if (unlink(claimed_path) < 0)
            return -1;
    } else if (errno != ENOENT) {
        return -1;
    }

    if (symlink(app->pty.slave_path, claimed_path) < 0)
        return -1;
    strcpy(app->link_path, claimed_path);
    app->link_owned = true;
    return 0;
}

static void release_symlink(struct app *app)
{
    char target[PATH_MAX];
    ssize_t length;

    if (!app->link_owned)
        return;
    length = readlink(app->link_path, target, sizeof(target) - 1);
    if (length >= 0) {
        target[length] = '\0';
        if (strcmp(target, app->pty.slave_path) == 0)
            (void)unlink(app->link_path);
    }
    app->link_owned = false;
}

static void sanitize_component(const char *input, char *output, size_t size)
{
    size_t used = 0;

    while (*input != '\0' && used + 1 < size) {
        unsigned char byte = (unsigned char)*input++;
        output[used++] = isalnum(byte) ? (char)byte : '_';
    }
    output[used] = '\0';
}

static int acquire_instance_lock(struct app *app)
{
    char bus[128];
    int length;

    sanitize_component(app->cfg.bus_name, bus, sizeof(bus));
    length = snprintf(app->lock_path, sizeof(app->lock_path),
                      "%s/i2ckiss-%s-%02x.lock", app->cfg.lock_dir, bus,
                      app->cfg.address);
    if (length < 0 || (size_t)length >= sizeof(app->lock_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    app->lock_fd = open(app->lock_path,
                        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (app->lock_fd < 0)
        return -1;
    if (flock(app->lock_fd, LOCK_EX | LOCK_NB) < 0) {
        int saved_errno = errno;
        close(app->lock_fd);
        app->lock_fd = -1;
        errno = saved_errno;
        return -1;
    }
    if (ftruncate(app->lock_fd, 0) == 0) {
        char pid[64];
        int pid_length = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
        if (pid_length > 0) {
            ssize_t ignored = write(app->lock_fd, pid, (size_t)pid_length);
            (void)ignored;
        }
    }
    return 0;
}

static int smbus_access(int fd, __u8 read_write, uint8_t command, __u32 size,
                        union i2c_smbus_data *data)
{
    struct i2c_smbus_ioctl_data request;

    request.read_write = read_write;
    request.command = command;
    request.size = size;
    request.data = data;
    return ioctl(fd, I2C_SMBUS, &request);
}

static int smbus_read_byte(int fd)
{
    union i2c_smbus_data data;

    if (smbus_access(fd, I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &data) < 0)
        return -1;
    return data.byte & 0xff;
}

static int smbus_write_byte(int fd, uint8_t value)
{
    return smbus_access(fd, I2C_SMBUS_WRITE, value, I2C_SMBUS_BYTE, NULL);
}

static void schedule_reconnect(struct app *app, const char *reason)
{
    int saved_errno = errno;

    if (app->i2c_fd >= 0) {
        close(app->i2c_fd);
        app->i2c_fd = -1;
    }
    decoder_reset(&app->from_i2c);
    if (app->tx_head != NULL)
        app->tx_head->pos = 0;
    app->stats.i2c_errors++;
    app->reconnect_at = monotonic_ms() + app->reconnect_backoff_ms;
    log_message(LOG_WARNING, "%s: %s; retrying in %d ms", reason,
                strerror(saved_errno), app->reconnect_backoff_ms);
    notify_systemd("STATUS=I2C unavailable; retrying\n");
    if (app->reconnect_backoff_ms < app->cfg.retry_max_ms) {
        app->reconnect_backoff_ms *= 2;
        if (app->reconnect_backoff_ms > app->cfg.retry_max_ms)
            app->reconnect_backoff_ms = app->cfg.retry_max_ms;
    }
}

static int reset_tnc(struct app *app)
{
    static const uint8_t reset_frame[] = { KISS_FEND, 0x0f, 0x02 };
    size_t i;

    if (!app->cfg.reset_tnc)
        return 0;
    for (i = 0; i < sizeof(reset_frame); ++i) {
        if (smbus_write_byte(app->i2c_fd, reset_frame[i]) < 0)
            return -1;
        sleep_us(app->cfg.byte_delay_us);
    }
    sleep_ms(app->cfg.reset_delay_ms);
    return 0;
}

static int connect_i2c(struct app *app)
{
    int fd;
    int probe;

    fd = open(app->cfg.i2c_path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    if (ioctl(fd, I2C_SLAVE, app->cfg.address) < 0)
        goto fail;
    if (app->cfg.i2c_timeout > 0 &&
        ioctl(fd, I2C_TIMEOUT, app->cfg.i2c_timeout) < 0 &&
        errno != ENOTTY && errno != EINVAL)
        goto fail;
    if (app->cfg.i2c_retries >= 0 &&
        ioctl(fd, I2C_RETRIES, app->cfg.i2c_retries) < 0 &&
        errno != ENOTTY && errno != EINVAL)
        goto fail;
    probe = smbus_read_byte(fd);
    if (probe < 0)
        goto fail;

    app->i2c_fd = fd;
    if (reset_tnc(app) < 0) {
        fd = app->i2c_fd;
        app->i2c_fd = -1;
        goto fail;
    }
    app->reconnect_backoff_ms = app->cfg.retry_min_ms;
    app->reconnect_at = 0;
    app->next_poll_at = monotonic_ms();
    app->stats.i2c_reconnects++;
    log_message(LOG_INFO, "connected to %s at address 0x%02x",
                app->cfg.i2c_path, app->cfg.address);
    notify_systemd("STATUS=I2C connected\n");
    return 0;

fail:
    {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
}

static void account_decode_error(struct app *app, enum decode_result result,
                                 const char *source)
{
    if (result == DECODE_BAD_CHECKSUM)
        app->stats.checksum_errors++;
    else if (result == DECODE_OVERSIZED)
        app->stats.oversized_frames++;
    else
        app->stats.malformed_frames++;
    if (log_verbosity > 0)
        log_message(LOG_WARNING, "discarded %s KISS frame (%s)", source,
                    result == DECODE_BAD_CHECKSUM ? "checksum" :
                    result == DECODE_OVERSIZED ? "too large" : "malformed");
}

static void consume_pty_bytes(struct app *app, const uint8_t *data, size_t length)
{
    size_t i;

    for (i = 0; i < length; ++i) {
        size_t frame_length;
        enum decode_result result = decoder_feed(&app->from_pty, data[i],
                                                  &frame_length);
        if (result == DECODE_FRAME) {
            if (enqueue_i2c_frame(app, app->from_pty.data, frame_length) < 0) {
                app->stats.dropped_frames++;
                log_message(LOG_ERR, "cannot queue KISS frame: %s",
                            strerror(errno));
            } else {
                app->stats.pty_rx_frames++;
                app->stats.pty_rx_bytes += frame_length;
            }
        } else if (result != DECODE_NONE) {
            account_decode_error(app, result, "PTY");
        }
    }
}

static int queue_frame_for_pty(struct app *app, const uint8_t *frame,
                               size_t frame_length)
{
    uint8_t encoded[MAX_KISS_FRAME * 2 + 4];
    size_t encoded_length = kiss_encode(frame, frame_length, 0, false, encoded);

    if (byte_queue_push(&app->pty_output, encoded, encoded_length) < 0) {
        app->stats.dropped_frames++;
        log_message(LOG_WARNING,
                    "PTY consumer is not keeping up; dropping a KISS frame");
        return -1;
    }
    app->stats.pty_tx_frames++;
    app->stats.pty_tx_bytes += frame_length;
    return 0;
}

static int consume_i2c_byte(struct app *app, uint8_t byte)
{
    size_t frame_length;
    enum decode_result result = decoder_feed(&app->from_i2c, byte,
                                              &frame_length);

    if (result == DECODE_FRAME)
        (void)queue_frame_for_pty(app, app->from_i2c.data, frame_length);
    else if (result != DECODE_NONE)
        account_decode_error(app, result, "I2C");
    return result == DECODE_FRAME ? 1 : 0;
}

static void service_i2c_receive(struct app *app)
{
    bool packet_started = app->from_i2c.len != 0 || app->from_i2c.escaped;
    unsigned int drained = 0;

    while (drained++ < MAX_KISS_FRAME * 2 + 4) {
        int value = smbus_read_byte(app->i2c_fd);

        if (value < 0) {
            schedule_reconnect(app, "I2C read failed");
            return;
        }
        if (!packet_started && value == TNC_NO_DATA)
            return;
        if (!packet_started && value == TNC_NAK) {
            log_message(LOG_WARNING, "TNC returned NAK");
            return;
        }
        packet_started = true;
        if (consume_i2c_byte(app, (uint8_t)value) != 0)
            return;
        sleep_us(app->cfg.byte_delay_us);
    }
    decoder_reset(&app->from_i2c);
    app->stats.oversized_frames++;
    log_message(LOG_WARNING, "unterminated I2C KISS frame discarded");
}

static void service_i2c_transmit(struct app *app)
{
    unsigned int budget = 64;

    while (budget-- > 0 && app->tx_head != NULL && app->i2c_fd >= 0) {
        struct tx_frame *frame = app->tx_head;

        if (smbus_write_byte(app->i2c_fd, frame->data[frame->pos]) < 0) {
            schedule_reconnect(app, "I2C write failed");
            return;
        }
        frame->pos++;
        sleep_us(app->cfg.byte_delay_us);
        if (frame->pos == frame->len) {
            app->tx_head = frame->next;
            app->tx_queued_bytes -= frame->len;
            if (app->tx_head == NULL)
                app->tx_tail = NULL;
            free(frame);
        }
    }
}

static void report_stats(const struct app *app)
{
    log_message(LOG_INFO,
                "PTY rx=%lu frames/%lu bytes tx=%lu frames/%lu bytes; "
                "I2C errors=%lu reconnects=%lu; checksum=%lu malformed=%lu "
                "oversized=%lu dropped=%lu child-restarts=%lu",
                app->stats.pty_rx_frames, app->stats.pty_rx_bytes,
                app->stats.pty_tx_frames, app->stats.pty_tx_bytes,
                app->stats.i2c_errors, app->stats.i2c_reconnects,
                app->stats.checksum_errors, app->stats.malformed_frames,
                app->stats.oversized_frames, app->stats.dropped_frames,
                app->stats.child_restarts);
}

static int spawn_kissattach(struct app *app)
{
    pid_t pid;
    char mtu[32];
    char *arguments[10];
    size_t count = 0;

    arguments[count++] = (char *)"kissattach";
    arguments[count++] = app->pty.slave_path;
    arguments[count++] = (char *)app->cfg.endpoint_arg1;
    arguments[count++] = (char *)app->cfg.endpoint_arg2;
    if (app->cfg.mtu > 0) {
        snprintf(mtu, sizeof(mtu), "%d", app->cfg.mtu);
        arguments[count++] = (char *)"-m";
        arguments[count++] = mtu;
    }
    if (app->cfg.use_syslog)
        arguments[count++] = (char *)"-l";
    arguments[count] = NULL;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        struct sigaction action;

        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGHUP, &action, NULL);
        sigaction(SIGCHLD, &action, NULL);
        signal(SIGPIPE, SIG_DFL);
        close(app->pty.master_fd);
        close(app->pty.guard_fd);
        if (app->i2c_fd >= 0)
            close(app->i2c_fd);
        if (app->lock_fd >= 0)
            close(app->lock_fd);
        execvp(arguments[0], arguments);
        dprintf(STDERR_FILENO, "%s: cannot execute kissattach: %s\n",
                PROGRAM_NAME, strerror(errno));
        _exit(127);
    }
    app->child_pid = pid;
    log_message(LOG_INFO, "started kissattach as pid %ld", (long)pid);
    return 0;
}

static void reap_children(struct app *app)
{
    int status;
    pid_t pid;

    child_changed = 0;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid != app->child_pid)
            continue;
        app->child_pid = -1;
        app->stats.child_restarts++;
        app->child_restart_at = monotonic_ms() + app->child_backoff_ms;
        if (WIFEXITED(status))
            log_message(LOG_WARNING,
                        "kissattach exited with status %d; restarting in %d ms",
                        WEXITSTATUS(status), app->child_backoff_ms);
        else if (WIFSIGNALED(status))
            log_message(LOG_WARNING,
                        "kissattach was killed by signal %d; restarting in %d ms",
                        WTERMSIG(status), app->child_backoff_ms);
        if (app->child_backoff_ms < app->cfg.retry_max_ms) {
            app->child_backoff_ms *= 2;
            if (app->child_backoff_ms > app->cfg.retry_max_ms)
                app->child_backoff_ms = app->cfg.retry_max_ms;
        }
    }
}

static int event_timeout(const struct app *app, int64_t now)
{
    int64_t deadline = now + 1000;

    if (app->i2c_fd < 0 && app->reconnect_at < deadline)
        deadline = app->reconnect_at;
    if (app->i2c_fd >= 0 && app->next_poll_at < deadline)
        deadline = app->next_poll_at;
    if (app->cfg.endpoint_mode == ENDPOINT_KISSATTACH &&
        app->child_pid < 0 && app->child_restart_at < deadline)
        deadline = app->child_restart_at;
    if (deadline <= now)
        return 0;
    return (int)(deadline - now);
}

static int service_pty_receive(struct app *app)
{
    uint8_t input[READ_CHUNK];

    for (;;) {
        ssize_t count = read(app->pty.master_fd, input, sizeof(input));

        if (count > 0) {
            consume_pty_bytes(app, input, (size_t)count);
            continue;
        }
        if (count == 0)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO)
            return 0;
        log_message(LOG_ERR, "PTY read failed: %s", strerror(errno));
        return -1;
    }
}

static int run_event_loop(struct app *app)
{
    while (!stop_requested) {
        struct pollfd descriptor;
        int64_t now = monotonic_ms();
        int result;

        if (child_changed)
            reap_children(app);
        if (report_requested) {
            report_requested = 0;
            report_stats(app);
        }
        if (app->cfg.endpoint_mode == ENDPOINT_KISSATTACH &&
            app->child_pid < 0 && now >= app->child_restart_at) {
            if (spawn_kissattach(app) < 0) {
                log_message(LOG_ERR, "cannot start kissattach: %s",
                            strerror(errno));
                app->child_restart_at = now + app->child_backoff_ms;
            }
        }
        if (app->i2c_fd < 0 && now >= app->reconnect_at) {
            if (connect_i2c(app) < 0)
                schedule_reconnect(app, "cannot connect to TNC");
        }
        if (app->i2c_fd >= 0) {
            service_i2c_transmit(app);
            now = monotonic_ms();
            if (app->i2c_fd >= 0 && now >= app->next_poll_at) {
                service_i2c_receive(app);
                app->next_poll_at = monotonic_ms() + app->cfg.poll_ms;
            }
        }

        descriptor.fd = app->pty.master_fd;
        descriptor.events = POLLIN;
        if (app->pty_output.used > 0)
            descriptor.events |= POLLOUT;
        descriptor.revents = 0;
        result = poll(&descriptor, 1, event_timeout(app, monotonic_ms()));
        if (result < 0) {
            if (errno == EINTR)
                continue;
            log_message(LOG_ERR, "poll failed: %s", strerror(errno));
            return -1;
        }
        if (descriptor.revents & POLLOUT) {
            if (byte_queue_flush(&app->pty_output, app->pty.master_fd) < 0) {
                log_message(LOG_ERR, "PTY write failed: %s", strerror(errno));
                return -1;
            }
        }
        if ((descriptor.revents & POLLIN) && service_pty_receive(app) < 0)
            return -1;
        if (descriptor.revents & POLLNVAL) {
            log_message(LOG_ERR, "PTY descriptor became invalid");
            return -1;
        }
        /* POLLHUP is expected while applications close and reopen the slave. */
    }
    return 0;
}

static int daemonize_process(void)
{
    pid_t pid;
    int null_fd;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);
    if (setsid() < 0)
        return -1;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);
    umask(022);
    if (chdir("/") < 0)
        return -1;
    null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0)
        return -1;
    if (dup2(null_fd, STDIN_FILENO) < 0 ||
        dup2(null_fd, STDOUT_FILENO) < 0 ||
        dup2(null_fd, STDERR_FILENO) < 0) {
        close(null_fd);
        return -1;
    }
    if (null_fd > STDERR_FILENO)
        close(null_fd);
    return 0;
}

static void terminate_child(struct app *app)
{
    int waited;

    if (app->child_pid <= 0)
        return;
    kill(app->child_pid, SIGTERM);
    for (waited = 0; waited < 20; ++waited) {
        pid_t result = waitpid(app->child_pid, NULL, WNOHANG);
        if (result == app->child_pid || (result < 0 && errno == ECHILD)) {
            app->child_pid = -1;
            return;
        }
        sleep_ms(50);
    }
    kill(app->child_pid, SIGKILL);
    (void)waitpid(app->child_pid, NULL, 0);
    app->child_pid = -1;
}

static void cleanup(struct app *app)
{
    notify_systemd("STOPPING=1\nSTATUS=Stopping\n");
    terminate_child(app);
    release_symlink(app);
    if (app->i2c_fd >= 0)
        close(app->i2c_fd);
    pty_pair_close(&app->pty);
    free_tx_queue(app);
    if (app->lock_fd >= 0)
        close(app->lock_fd);
    if (log_to_syslog)
        closelog();
}

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: i2ckiss [options] i2cbus i2cdevice symlink linkname\n"
            "       i2ckiss [options] i2cbus i2cdevice port inetaddr\n\n"
            "Bridge a TNC-Pi/Black I2C endpoint to a newly allocated KISS PTY.\n\n"
            "  -p N, --poll-ticks N     poll every N * 100 ms (default: 1)\n"
            "      --poll-ms N          poll interval in milliseconds\n"
            "  -m N, --mtu N            pass MTU to kissattach\n"
            "  -d, --debug              increase diagnostic output\n"
            "  -l, --syslog             log through syslog\n"
            "  -D, --daemon             detach (foreground is the default)\n"
            "  -f, --foreground         stay in foreground\n"
            "      --no-reset           do not reset TNC after reconnect\n"
            "      --retry-min-ms N     initial reconnect delay (default: 250)\n"
            "      --retry-max-ms N     maximum reconnect delay (default: 30000)\n"
            "      --lock-dir PATH      instance-lock directory (default: /run/lock)\n"
            "  -v, --version            print version and exit\n"
            "  -h, --help               show this help\n");
}

static int parse_integer(const char *text, int minimum, int maximum, int *value)
{
    char *end;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 0);
    if (errno != 0 || *text == '\0' || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        errno = EINVAL;
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static int parse_arguments(int argc, char **argv, struct config *cfg)
{
    enum {
        OPT_POLL_MS = 1000,
        OPT_NO_RESET,
        OPT_RETRY_MIN,
        OPT_RETRY_MAX,
        OPT_LOCK_DIR,
        OPT_BYTE_DELAY,
        OPT_RESET_DELAY,
    };
    static const struct option options[] = {
        { "poll-ticks", required_argument, NULL, 'p' },
        { "poll-ms", required_argument, NULL, OPT_POLL_MS },
        { "mtu", required_argument, NULL, 'm' },
        { "debug", no_argument, NULL, 'd' },
        { "syslog", no_argument, NULL, 'l' },
        { "daemon", no_argument, NULL, 'D' },
        { "foreground", no_argument, NULL, 'f' },
        { "no-reset", no_argument, NULL, OPT_NO_RESET },
        { "retry-min-ms", required_argument, NULL, OPT_RETRY_MIN },
        { "retry-max-ms", required_argument, NULL, OPT_RETRY_MAX },
        { "lock-dir", required_argument, NULL, OPT_LOCK_DIR },
        { "byte-delay-us", required_argument, NULL, OPT_BYTE_DELAY },
        { "reset-delay-ms", required_argument, NULL, OPT_RESET_DELAY },
        { "version", no_argument, NULL, 'v' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    int option;
    int ticks;
    const char *bus;
    int length;

    memset(cfg, 0, sizeof(*cfg));
    cfg->poll_ms = 100;
    cfg->byte_delay_us = 1000;
    cfg->retry_min_ms = 250;
    cfg->retry_max_ms = 30000;
    cfg->i2c_timeout = 100;
    cfg->i2c_retries = 3;
    cfg->reset_delay_ms = 2000;
    cfg->reset_tnc = true;
    cfg->lock_dir = "/run/lock";

    while ((option = getopt_long(argc, argv, "p:m:dlDfvh", options, NULL)) != -1) {
        switch (option) {
        case 'p':
            if (parse_integer(optarg, 1, 36000, &ticks) < 0)
                return -1;
            cfg->poll_ms = ticks * 100;
            break;
        case OPT_POLL_MS:
            if (parse_integer(optarg, 1, 3600000, &cfg->poll_ms) < 0)
                return -1;
            break;
        case 'm':
            if (parse_integer(optarg, 1, 65535, &cfg->mtu) < 0)
                return -1;
            break;
        case 'd':
            cfg->diagnostics++;
            break;
        case 'l':
            cfg->use_syslog = true;
            break;
        case 'D':
            cfg->daemonize = true;
            cfg->use_syslog = true;
            break;
        case 'f':
            cfg->daemonize = false;
            break;
        case OPT_NO_RESET:
            cfg->reset_tnc = false;
            break;
        case OPT_RETRY_MIN:
            if (parse_integer(optarg, 10, 3600000, &cfg->retry_min_ms) < 0)
                return -1;
            break;
        case OPT_RETRY_MAX:
            if (parse_integer(optarg, 10, 3600000, &cfg->retry_max_ms) < 0)
                return -1;
            break;
        case OPT_LOCK_DIR:
            cfg->lock_dir = optarg;
            break;
        case OPT_BYTE_DELAY:
            if (parse_integer(optarg, 0, 1000000, &cfg->byte_delay_us) < 0)
                return -1;
            break;
        case OPT_RESET_DELAY:
            if (parse_integer(optarg, 0, 60000, &cfg->reset_delay_ms) < 0)
                return -1;
            break;
        case 'v':
            printf("%s %s\n", PROGRAM_NAME, PROGRAM_VERSION);
            exit(EXIT_SUCCESS);
        case 'h':
            usage(stdout);
            exit(EXIT_SUCCESS);
        default:
            return -1;
        }
    }
    if (cfg->retry_max_ms < cfg->retry_min_ms || argc - optind != 4) {
        errno = EINVAL;
        return -1;
    }

    bus = argv[optind];
    if (strlen(bus) >= sizeof(cfg->bus_name)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(cfg->bus_name, bus);
    if (strncmp(bus, "/dev/i2c-", 9) == 0)
        length = snprintf(cfg->i2c_path, sizeof(cfg->i2c_path), "%s", bus);
    else
        length = snprintf(cfg->i2c_path, sizeof(cfg->i2c_path), "/dev/i2c-%s", bus);
    if (length < 0 || (size_t)length >= sizeof(cfg->i2c_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (parse_integer(argv[optind + 1], 0x03, 0x77, &cfg->address) < 0)
        return -1;
    if (strcmp(argv[optind + 2], "symlink") == 0) {
        cfg->endpoint_mode = ENDPOINT_SYMLINK;
        cfg->endpoint_arg1 = argv[optind + 3];
    } else {
        cfg->endpoint_mode = ENDPOINT_KISSATTACH;
        cfg->endpoint_arg1 = argv[optind + 2];
        cfg->endpoint_arg2 = argv[optind + 3];
    }
    return 0;
}

#ifndef I2CKISS_TEST
int main(int argc, char **argv)
{
    struct app app;
    int exit_status = EXIT_FAILURE;

    memset(&app, 0, sizeof(app));
    app.pty.master_fd = -1;
    app.pty.guard_fd = -1;
    app.i2c_fd = -1;
    app.lock_fd = -1;
    app.child_pid = -1;

    if (parse_arguments(argc, argv, &app.cfg) < 0) {
        fprintf(stderr, "%s: invalid arguments: %s\n", PROGRAM_NAME,
                strerror(errno));
        usage(stderr);
        return EXIT_FAILURE;
    }
    log_to_syslog = app.cfg.use_syslog;
    log_verbosity = app.cfg.diagnostics;
    if (log_to_syslog)
        openlog(PROGRAM_NAME, LOG_PID | LOG_NDELAY, LOG_DAEMON);
    if (install_signal_handlers() < 0) {
        log_message(LOG_ERR, "cannot install signal handlers: %s",
                    strerror(errno));
        goto done;
    }
    if (acquire_instance_lock(&app) < 0) {
        log_message(LOG_ERR,
                    "cannot lock I2C bus/address instance %s/0x%02x: %s",
                    app.cfg.bus_name, app.cfg.address, strerror(errno));
        goto done;
    }
    if (pty_pair_open(&app.pty) < 0) {
        log_message(LOG_ERR, "cannot allocate PTY: %s", strerror(errno));
        goto done;
    }
    if (app.cfg.endpoint_mode == ENDPOINT_SYMLINK) {
        if (claim_symlink(&app, app.cfg.endpoint_arg1) < 0) {
            log_message(LOG_ERR,
                        "refusing to replace endpoint %s: %s",
                        app.cfg.endpoint_arg1, strerror(errno));
            goto done;
        }
        log_message(LOG_INFO, "published PTY %s as %s", app.pty.slave_path,
                    app.cfg.endpoint_arg1);
    } else {
        log_message(LOG_INFO, "allocated PTY %s for kissattach",
                    app.pty.slave_path);
    }
    if (app.cfg.daemonize && daemonize_process() < 0) {
        log_message(LOG_ERR, "cannot become a daemon: %s", strerror(errno));
        goto done;
    }
    app.reconnect_backoff_ms = app.cfg.retry_min_ms;
    app.reconnect_at = monotonic_ms();
    app.child_backoff_ms = app.cfg.retry_min_ms;
    app.child_restart_at = monotonic_ms();
    app.from_pty.checksum = false;
    app.from_i2c.checksum = true;

    log_message(LOG_INFO, "starting %s %s", PROGRAM_NAME, PROGRAM_VERSION);
    notify_systemd("READY=1\nSTATUS=Bridge endpoint ready\n");
    exit_status = run_event_loop(&app) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    report_stats(&app);

done:
    cleanup(&app);
    return exit_status;
}
#endif
