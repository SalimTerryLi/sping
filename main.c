#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <poll.h>
#include <time.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>

#define log(format, ...) fprintf (stderr, format, ##__VA_ARGS__)
#define output(format, ...) fprintf (stdout, format, ##__VA_ARGS__)

// compile-time configuration
#define MAX_ICMP_DELAY_MS 3000

// run-time configuration
static uint16_t parallel_count = 16;
static uint16_t ignore_first_num = 3;
static uint16_t calc_avg_num = 5;
static uint16_t max_retry_count = 3;

int test_ping();

void print_help(const char *execname) {
    log("%s [options]\n", execname);
    log("A tool capable doing ICMP Ping test with multiple IPs at the same time\n");
    log("IPs should be fed through stdin, one per line\n");
    log("EOF on pipe will stop the program from waiting for new IPs\n");
    log("Test results will be printed to stdout\n");
    log("Options:\n");
    log("  -h       show this page\n");
    log("  -p n     max parallel task count, default 16\n");
    log("  -i n     ignore first n packets, default 3\n");
    log("  -a n     calculate average result from n packets, default 5\n");
    log("  -r n     maximum retry count, default 3\n\n");
}

char *readline(char *recv_buf, size_t size, int fd) {
    size_t buf_idx = 0;
    while (buf_idx < size) {
        long rd_len = read(0, recv_buf + buf_idx, 1);
        if (rd_len == 0) {
            errno = 0;
            return NULL;
        } else if (rd_len == -1) {
            if (errno == EAGAIN) {
                // no data available in buffer
                return NULL;
            } else {
                printf("failed to read from fd: %u %s", errno, strerror(errno));
                return NULL;
            }
        } else {
            if (recv_buf[buf_idx] == '\n' || recv_buf[buf_idx] == '\0') {
                recv_buf[buf_idx] = '\0';
                return recv_buf;
            } else {
                ++buf_idx;
            }
        }
    }
    log("buf too small\n");
    return NULL;
}

int main(int argc, char** argv) {
    int c;
    char *endptr;
    while ((c = getopt (argc, argv, "hp:i:a:r:")) != -1) {
        switch (c) {
            case 'h':
                print_help(*argv);
                exit(0);

            case 'p':
                errno = 0;
                parallel_count = (uint16_t)strtol(optarg, &endptr, 10);
                if (errno != 0 || endptr == optarg || *endptr != '\0') {
                    log("failed to parse argument of -p: %s\n", optarg);
                    exit(-1);
                }
                if (parallel_count == 0) {
                    log("set parallel to 1...\n");
                    parallel_count = 1;
                }
                break;

            case 'i':
                errno = 0;
                ignore_first_num = (uint16_t)strtol(optarg, &endptr, 10);
                if (errno != 0 || endptr == optarg || *endptr != '\0') {
                    log("failed to parse argument of -p: %s\n", optarg);
                    exit(-1);
                }
                break;

            case 'a':
                errno = 0;
                calc_avg_num = (uint16_t)strtol(optarg, &endptr, 10);
                if (errno != 0 || endptr == optarg || *endptr != '\0') {
                    log("failed to parse argument of -p: %s\n", optarg);
                    exit(-1);
                }
                if (calc_avg_num == 0) {
                    log("set avg_num to 1...\n");
                    calc_avg_num = 1;
                }
                break;

            case 'r':
                errno = 0;
                max_retry_count = (uint16_t)strtol(optarg, &endptr, 10);
                if (errno != 0 || endptr == optarg || *endptr != '\0') {
                    log("failed to parse argument of -p: %s\n", optarg);
                    exit(-1);
                }
                break;

            case '?':
                log("unrecognized option: -%c\nrun with -h to see help page\n", optopt);
                exit(-1);

            default:
                abort();
        }
    }

    // test support of net.ipv4.ping_group_range
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (sock == -1) {
        log("failed to create IPPROTO_ICMP socket.\n");
        log("try to enable it by `sudo sysctl -w net.ipv4.ping_group_range='0 2147483647'`\n");
        exit(-2);
    }
    close(sock);

    log("start ICMP Ping test with %u tasks in parallel\n", parallel_count);
    // make stdin non-blocking
    int fd_stat = fcntl(0, F_GETFL);
    assert(fcntl(0, F_SETFL, fd_stat | O_NONBLOCK) == 0);
    int ret = test_ping();
    // reset stdin
    fcntl(0, F_SETFL, fd_stat);
    return ret;
}

typedef struct {
    struct in_addr addr;    // ipv4 addr, 0 for not in used
    uint16_t icmpseq;       // next available seq, packet num already sent
    uint32_t min_delay_ms;  // recorded minimum delay in ms
    uint32_t max_delay_ms;  // recorded maximum delay in ms
    uint32_t avg_delay_ms;  // calculated average delay in ms
    double std_deviation;   // calculated standard deviation
    // below are private members
    int32_t _last_success_seq;
    uint16_t _total_success_count;
    uint16_t _polled_timer_ms;      // how long did last ping packet sent, in ms
} icmptest_unit_ctx_t;

uint64_t get_timestamp() {
    struct timespec spec;
    clock_gettime(CLOCK_REALTIME, &spec);
    return spec.tv_nsec / 1000000 + spec.tv_sec * 1000;
}

static char recv_buf[1024];

int test_ping() {
    // summary
    size_t total_input_lines = 0;
    size_t total_valid_targets = 0;
    size_t total_success_targets = 0;
    size_t total_failed_targets = 0;
    // alloc space for test units
    icmptest_unit_ctx_t *units = malloc(sizeof(icmptest_unit_ctx_t) * parallel_count);
    if (units == NULL) {
        log("failed to alloc units: %s\n", strerror(errno));
        return -2;
    }
    memset(units, 0, sizeof(icmptest_unit_ctx_t) * parallel_count);

    // use one single socket for all traffic
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    assert(fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK) == 0);

    // socket payload
    union {
        char buf[0];
        struct icmphdr icmp_hdr;
    } icmp_packet = {
            .icmp_hdr = {
                    .type = ICMP_ECHO,
                    .code = 0,
                    .un.echo.id = 0,
                    .un.echo.sequence = 0           // should be updated before request is sent
            }
    };
    // socket addr
    struct sockaddr_in s_addr = {
            .sin_family = AF_INET,
            .sin_addr.s_addr = 0,                   // should be updated before request is sent
    };

    // prepare pollfd
    struct pollfd fds[] = {
            {
                    .events = POLLIN,
                    .fd = sock,
            },
            {
                .events = POLLIN,
                .fd = 0,
            },
    };

    // print table header
    output("IP\tAVG\tMIN\tMAX\tERR\tTOTAL\tLOST\t\n");

    bool is_input_stopped = false;              // set when eof is received or stdin is closed
    size_t free_task_slots = parallel_count;      // number of spare slots
    icmptest_unit_ctx_t *next_free_slot = NULL;  // optimise: speed up finding next available slot
    uint64_t timestamp_last = get_timestamp();
    uint64_t timestamp_curr = get_timestamp();
    while (true) {
        // do poll
        fds[0].revents = 0;
        fds[1].revents = 0;
        // do not poll stdin if there is no free slots or stdin is already shutdown
        int pollret = poll(fds, is_input_stopped || free_task_slots == 0?
            1:
            sizeof(fds) / sizeof(struct pollfd),
        MAX_ICMP_DELAY_MS);
        timestamp_last = timestamp_curr;
        timestamp_curr = get_timestamp();
        uint64_t timestamp_delta = timestamp_curr - timestamp_last;
        if (pollret < 0) {
            log("poll() error: %s", strerror(errno));
            break;
        } else {
            // store received information here
            struct sockaddr_in sender_addr = {
                    .sin_addr.s_addr = 0,
            };
            uint16_t sender_seq;
            bool has_echoreply_received = false;
            // check if socket received something
            if (fds[0].revents & POLLIN) {
                socklen_t sender_addr_len;
                long recv_num = recvfrom(sock, recv_buf, sizeof(recv_buf), 0, (struct sockaddr *)&sender_addr, &sender_addr_len);
                if (recv_num < 0) {
                    log("socket recvfrom() error: %s\n", strerror(errno));
                    break;
                }
                //log("received_len: %ld, from %s\n", recv_num, inet_ntoa(sender_addr.sin_addr));
                struct icmphdr *hdr = (struct icmphdr *) recv_buf;
                if (hdr->type == ICMP_ECHOREPLY && hdr->code == 0) {
                    sender_seq = hdr->un.echo.sequence;
                    has_echoreply_received = true;
                }
            } else if (fds[0].revents & (POLLERR | POLLHUP)) {
                log("socket error: POLLERR=%d, POLLHUP=%d\n", fds[0].revents & POLLERR, fds[0].revents & POLLHUP);
                break;
            }

            // reverse iterate though all sessions
            free_task_slots = 0;                // temporarily set
            for (icmptest_unit_ctx_t *unit = units + parallel_count - 1; unit >= units; --unit) {
                if (unit->addr.s_addr == 0) {
                    // this slot is not in use, add to spare count
                    ++free_task_slots;
                    // due to reversed lookup ,this will always contain the first available slot
                    next_free_slot = unit;
                    continue;
                } else {
                    // update timeout fields
                    unit->_polled_timer_ms += timestamp_delta;

                    // deal with timeout
                    if (unit->_polled_timer_ms > MAX_ICMP_DELAY_MS) {
                        // this seq timeout
                        if (unit->icmpseq - (unit->_last_success_seq + 1) > max_retry_count) {
                            // total count = one normal send + n retries
                            // all retry expire, stop now
                            // mark this slot as spare
                            ++total_failed_targets;
                            log("failed to test target:%s\n", inet_ntoa(unit->addr));
                            output("%s\t%d\t%d\t%d\t%.0f\t%d\t%u\n",
                                   inet_ntoa(unit->addr),
                                   -1,
                                   -1,
                                   -1,
                                   -1.0,
                                   unit->icmpseq + 1,
                                   unit->icmpseq + 1 - unit->_total_success_count
                            );
                            unit->addr.s_addr = 0;
                            ++free_task_slots;
                            next_free_slot = unit;
                        } else {
                            // perform another attempt
                            ++unit->icmpseq;
                            icmp_packet.icmp_hdr.un.echo.sequence = unit->icmpseq;
                            s_addr.sin_addr.s_addr = unit->addr.s_addr;
                            sendto(sock, icmp_packet.buf, sizeof(icmp_packet), 0, (struct sockaddr*)&s_addr, sizeof(s_addr));
                            unit->_polled_timer_ms = 0;
                            log("send to: %s, type: retry=%d, seq=%u\n", inet_ntoa(unit->addr), unit->icmpseq - (unit->_last_success_seq + 1), unit->icmpseq);
                        }
                        continue;
                    }

                    // see if any of those matches
                    if (has_echoreply_received &&
                        sender_addr.sin_addr.s_addr == unit->addr.s_addr &&
                        sender_seq == unit->icmpseq
                        ) {
                        unit->_last_success_seq = unit->icmpseq;
                        ++(unit->_total_success_count);
                        log("recv from: %s, seq=%u, RTT=%u, total_success_num=%u\n", inet_ntoa(unit->addr), sender_seq, unit->_polled_timer_ms, unit->_total_success_count);

                        if (unit->_total_success_count < ignore_first_num) {
                            // skip first n packets
                            ++unit->icmpseq;
                            icmp_packet.icmp_hdr.un.echo.sequence = unit->icmpseq;
                            s_addr.sin_addr.s_addr = unit->addr.s_addr;
                            sendto(sock, icmp_packet.buf, sizeof(icmp_packet), 0, (struct sockaddr*)&s_addr, sizeof(s_addr));
                            unit->_polled_timer_ms = 0;
                            log("send to: %s, type=warmup, seq=%u\n", inet_ntoa(unit->addr), unit->icmpseq);
                        } else if (unit->_total_success_count <= calc_avg_num + ignore_first_num) {
                            if (unit->_total_success_count == ignore_first_num) {
                                // this is the callback event of last one first-n packet,
                                // so make sure it is not calculated into result
                            } else {
                                // calc
                                if (unit->min_delay_ms > unit->_polled_timer_ms) {
                                    unit->min_delay_ms = unit->_polled_timer_ms;
                                }
                                if (unit->max_delay_ms < unit->_polled_timer_ms) {
                                    unit->max_delay_ms = unit->_polled_timer_ms;
                                }
                                uint16_t accounted_sample_num = unit->_total_success_count - ignore_first_num;
                                unit->avg_delay_ms = (uint32_t)((double)unit->avg_delay_ms * (accounted_sample_num - 1) / accounted_sample_num + (double)unit->_polled_timer_ms / accounted_sample_num);
                                // TODO: calc standard deviation
                            }
                            // check if should stop
                            if (unit->_total_success_count == calc_avg_num + ignore_first_num) {
                                ++total_success_targets;
                                log("ping test result:\n");
                                output("%s\t%u\t%u\t%u\t%.0f\t%d\t%u\n",
                                       inet_ntoa(unit->addr),
                                       unit->avg_delay_ms,
                                       unit->min_delay_ms,
                                       unit->max_delay_ms,
                                       unit->std_deviation,
                                       unit->icmpseq + 1,
                                       unit->icmpseq + 1 - unit->_total_success_count
                                       );
                                unit->addr.s_addr = 0;
                                ++free_task_slots;
                                next_free_slot = unit;
                            } else {
                                ++unit->icmpseq;
                                icmp_packet.icmp_hdr.un.echo.sequence = unit->icmpseq;
                                s_addr.sin_addr.s_addr = unit->addr.s_addr;
                                sendto(sock, icmp_packet.buf, sizeof(icmp_packet), 0, (struct sockaddr*)&s_addr, sizeof(s_addr));
                                unit->_polled_timer_ms = 0;
                                log("send to: %s, type=avg, seq=%u\n", inet_ntoa(unit->addr), unit->icmpseq);
                            }
                        }
                    }
                }
            }

            // check stdin
            if (fds[1].revents & POLLIN) {
                // if there is available slots then accept new tasks
                while (free_task_slots > 0) {
                    char *line = readline(recv_buf, sizeof(recv_buf), 0);
                    if (line == NULL) {
                        if (errno == EWOULDBLOCK) {
                            // buffer empty
                        } else {
                            log("EOF received. exiting\n");
                            is_input_stopped = true;
                        }
                        break;
                    } else {
                        ++total_input_lines;
                        if(inet_aton(line, &next_free_slot->addr) == 0) {
                            log("failed to parse input as ip: %s\n", line);
                            next_free_slot->addr.s_addr=0;
                            continue;
                        } else {
                            ++total_valid_targets;
                            log("start pinging target %s\n", inet_ntoa(next_free_slot->addr));
                            // occupy current slot
                            next_free_slot->icmpseq = 0;
                            next_free_slot->max_delay_ms = 0;
                            next_free_slot->min_delay_ms = UINT32_MAX;
                            next_free_slot->avg_delay_ms = 0;
                            next_free_slot->std_deviation = 0.0;
                            next_free_slot->_last_success_seq = -1;
                            next_free_slot->_total_success_count = 0;
                            next_free_slot->_polled_timer_ms = 0;
                            --free_task_slots;
                            // do ping
                            icmp_packet.icmp_hdr.un.echo.sequence = next_free_slot->icmpseq;
                            s_addr.sin_addr.s_addr = next_free_slot->addr.s_addr;
                            sendto(sock, icmp_packet.buf, sizeof(icmp_packet), 0, (struct sockaddr*)&s_addr, sizeof(s_addr));
                            log("send to: %s, type=first, seq=%u\n", inet_ntoa(next_free_slot->addr), next_free_slot->icmpseq);
                            // find next available slot
                            while (next_free_slot < units + parallel_count) {
                                ++next_free_slot;
                                if (next_free_slot->addr.s_addr == 0) {
                                    break;
                                }
                            }
                            if (free_task_slots == 0) {
                                log("task slots are full\n");
                            }
                        }
                    }
                }

            } else if (fds[1].revents & POLLERR) {
                log("stdin error: POLLERR=%d, POLLHUP=%d\n", fds[1].revents & POLLERR, fds[1].revents & POLLHUP);
            } else if (fds[1].revents & POLLHUP) {
                log("pipe closed without EOF received, exiting\n");
                is_input_stopped = true;
            }

            // check exit: only if there is no pending tasks and input is already stopped
            if (free_task_slots == parallel_count && is_input_stopped) {
                break;
            }
        }
    }
    log("total_input: %lu, valid_input: %lu, success: %lu, failed: %lu\n",
        total_input_lines,
        total_valid_targets,
        total_success_targets,
        total_failed_targets
        );
    log("exited\n");
    close(sock);
    free(units);
    return 0;
}