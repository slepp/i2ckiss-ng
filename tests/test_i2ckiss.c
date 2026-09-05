/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Stephen Olesen
 */

#define I2CKISS_TEST
#include "../src/i2ckiss.c"

#include <assert.h>

static void test_kiss_codec(void)
{
    static const uint8_t frame[] = { 0x00, 0x01, KISS_FEND, KISS_FESC, 0x7f };
    uint8_t encoded[MAX_KISS_FRAME * 2 + 4];
    struct kiss_decoder decoder;
    size_t encoded_length;
    size_t frame_length = 0;
    size_t i;
    enum decode_result result = DECODE_NONE;

    memset(&decoder, 0, sizeof(decoder));
    decoder.checksum = true;
    encoded_length = kiss_encode(frame, sizeof(frame), 0, true, encoded);
    for (i = 0; i < encoded_length; ++i) {
        result = decoder_feed(&decoder, encoded[i], &frame_length);
        if (result == DECODE_FRAME)
            break;
    }
    assert(result == DECODE_FRAME);
    assert(frame_length == sizeof(frame));
    assert(memcmp(decoder.data, frame, sizeof(frame)) == 0);

    encoded[encoded_length - 2] ^= 1;
    decoder_reset(&decoder);
    result = DECODE_NONE;
    for (i = 0; i < encoded_length; ++i)
        result = decoder_feed(&decoder, encoded[i], &frame_length);
    assert(result == DECODE_BAD_CHECKSUM);

    memset(&decoder, 0, sizeof(decoder));
    for (i = 0; i <= MAX_KISS_FRAME; ++i)
        assert(decoder_feed(&decoder, 0x01, &frame_length) == DECODE_NONE);
    assert(decoder_feed(&decoder, KISS_FEND, &frame_length) ==
           DECODE_OVERSIZED);
}

static void test_decoder_boundaries(void)
{
    struct kiss_decoder decoder = { .checksum = true };
    size_t length = 123;

    assert(decoder_feed(&decoder, KISS_FEND, &length) == DECODE_NONE);
    assert(length == 0);
    assert(decoder_feed(&decoder, 0, &length) == DECODE_NONE);
    assert(decoder_feed(&decoder, KISS_FEND, &length) == DECODE_MALFORMED);
    assert(length == 0);
    assert(decoder_feed(&decoder, KISS_FESC, &length) == DECODE_NONE);
    assert(decoder_feed(&decoder, KISS_FEND, &length) == DECODE_MALFORMED);

    /* A valid command-only frame still works after rejected frames. */
    assert(decoder_feed(&decoder, 0, &length) == DECODE_NONE);
    assert(decoder_feed(&decoder, 0, &length) == DECODE_NONE);
    assert(decoder_feed(&decoder, KISS_FEND, &length) == DECODE_FRAME);
    assert(length == 1 && decoder.data[0] == 0);

    decoder.checksum = false;
    for (size_t i = 0; i < MAX_KISS_FRAME; ++i)
        assert(decoder_feed(&decoder, 1, &length) == DECODE_NONE);
    assert(decoder_feed(&decoder, KISS_FEND, &length) == DECODE_FRAME);
    assert(length == MAX_KISS_FRAME);

    /* An invalid escape reports an error but retains preceding bytes. */
    assert(decoder_feed(&decoder, 0, &length) == DECODE_NONE);
    assert(decoder_feed(&decoder, KISS_FESC, &length) == DECODE_NONE);
    assert(decoder_feed(&decoder, 1, &length) == DECODE_MALFORMED);
    assert(decoder_feed(&decoder, KISS_FEND, &length) == DECODE_FRAME);
    assert(length == 1 && decoder.data[0] == 0);
}

static void test_pty_receive_queues_wire_frame(void)
{
    struct app app = {0};
    static const uint8_t input[] = { KISS_FEND, 0x70, KISS_FESC };
    static const uint8_t rest[] = { KISS_TFEND, KISS_FEND };
    static const uint8_t expected[] = {
        KISS_FEND, 0, KISS_FESC, KISS_TFEND,
        KISS_FESC, KISS_TFEND, KISS_FEND
    };
    struct pollfd descriptor;

    assert(pty_pair_open(&app.pty) == 0);
    descriptor = (struct pollfd){ .fd = app.pty.master_fd, .events = POLLIN };
    assert(service_pty_receive(&app) == 0);
    assert(write(app.pty.guard_fd, input, sizeof(input)) == (ssize_t)sizeof(input));
    assert(poll(&descriptor, 1, 1000) == 1);
    assert(service_pty_receive(&app) == 0);
    assert(app.tx_head == NULL);
    assert(write(app.pty.guard_fd, rest, sizeof(rest)) == (ssize_t)sizeof(rest));
    assert(poll(&descriptor, 1, 1000) == 1);
    assert(service_pty_receive(&app) == 0);
    assert(app.tx_head != NULL && app.tx_head == app.tx_tail);
    assert(app.tx_head->len == sizeof(expected));
    assert(memcmp(app.tx_head->data, expected, sizeof(expected)) == 0);
    free_tx_queue(&app);
    pty_pair_close(&app.pty);
}

static void test_i2c_queue_is_bounded(void)
{
    struct app app;
    uint8_t frame[MAX_KISS_FRAME];
    unsigned int queued = 0;

    memset(&app, 0, sizeof(app));
    memset(frame, KISS_FEND, sizeof(frame));
    frame[0] = 0;
    while (enqueue_i2c_frame(&app, frame, sizeof(frame)) == 0)
        queued++;
    assert(queued > 0);
    assert(errno == ENOBUFS);
    assert(app.tx_queued_bytes <= I2C_QUEUE_SIZE);
    free_tx_queue(&app);
    assert(app.tx_queued_bytes == 0);
}

static void test_pty_is_unique_and_usable(void)
{
    struct pty_pair first;
    struct pty_pair second;
    uint8_t sent[] = { 1, 2, 3, 4 };
    uint8_t received[sizeof(sent)];
    struct stat metadata;
    int transient_client;
    ssize_t count;

    assert(pty_pair_open(&first) == 0);
    assert(pty_pair_open(&second) == 0);
    assert(strcmp(first.slave_path, second.slave_path) != 0);
    assert(write(first.guard_fd, sent, sizeof(sent)) == (ssize_t)sizeof(sent));
    count = read(first.master_fd, received, sizeof(received));
    assert(count == (ssize_t)sizeof(received));
    assert(memcmp(sent, received, sizeof(sent)) == 0);
    transient_client = open(first.slave_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    assert(transient_client >= 0);
    close(transient_client);
    assert(stat(first.slave_path, &metadata) == 0);
    assert(write(first.guard_fd, sent, sizeof(sent)) == (ssize_t)sizeof(sent));
    assert(read(first.master_fd, received, sizeof(received)) ==
           (ssize_t)sizeof(received));
    pty_pair_close(&second);
    pty_pair_close(&first);
}

static void test_symlink_collision_rules(void)
{
    struct app first;
    struct app second;
    char directory[] = "/tmp/i2ckiss-test-XXXXXX";
    char link_path[PATH_MAX];
    char target[PATH_MAX];
    ssize_t length;
    int file_fd;

    assert(mkdtemp(directory) != NULL);
    assert(snprintf(link_path, sizeof(link_path), "%s/com1", directory) > 0);
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    first.pty.master_fd = first.pty.guard_fd = -1;
    second.pty.master_fd = second.pty.guard_fd = -1;
    assert(pty_pair_open(&first.pty) == 0);
    assert(pty_pair_open(&second.pty) == 0);
    assert(claim_symlink(&first, link_path) == 0);
    errno = 0;
    assert(claim_symlink(&second, link_path) < 0);
    assert(errno == EEXIST);
    length = readlink(link_path, target, sizeof(target) - 1);
    assert(length > 0);
    target[length] = '\0';
    assert(strcmp(target, first.pty.slave_path) == 0);
    release_symlink(&first);

    assert(symlink("/dev/pts/99999999", link_path) == 0);
    assert(claim_symlink(&second, link_path) == 0);
    release_symlink(&second);

    file_fd = open(link_path, O_CREAT | O_WRONLY | O_EXCL, 0600);
    assert(file_fd >= 0);
    close(file_fd);
    errno = 0;
    assert(claim_symlink(&first, link_path) < 0);
    assert(errno == EEXIST);
    errno = 0;
    assert(claim_symlink(&first, "/dev/pts/99999999") < 0);
    assert(errno == EPERM);
    assert(unlink(link_path) == 0);
    pty_pair_close(&second.pty);
    pty_pair_close(&first.pty);
    assert(rmdir(directory) == 0);
}

static void test_instance_lock(void)
{
    struct app first;
    struct app second;
    char directory[] = "/tmp/i2ckiss-lock-XXXXXX";

    assert(mkdtemp(directory) != NULL);
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    first.lock_fd = second.lock_fd = -1;
    strcpy(first.cfg.bus_name, "1");
    strcpy(second.cfg.bus_name, "1");
    first.cfg.address = second.cfg.address = 0x10;
    first.cfg.lock_dir = second.cfg.lock_dir = directory;
    assert(acquire_instance_lock(&first) == 0);
    errno = 0;
    assert(acquire_instance_lock(&second) < 0);
    assert(errno == EWOULDBLOCK || errno == EAGAIN);
    close(first.lock_fd);
    assert(unlink(first.lock_path) == 0);
    assert(rmdir(directory) == 0);
}

int main(void)
{
    test_kiss_codec();
    test_decoder_boundaries();
    test_pty_receive_queues_wire_frame();
    test_i2c_queue_is_bounded();
    test_pty_is_unique_and_usable();
    test_symlink_collision_rules();
    test_instance_lock();
    puts("all tests passed");
    return 0;
}
