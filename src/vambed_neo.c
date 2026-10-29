/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * vambed-neo: AMBEd-compatible software transcoder for reflector testing.
 *
 * One thread, one poll() loop. The control socket (UDP base port, 10100 by
 * default) answers PING, CAPS, legacy and extended OPEN and CS; every open
 * stream gets its own data socket on base port + stream id. A data request
 * is decoded to PCM and encoded to every output at once, so each request
 * gets exactly one reply.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "vn_codec.h"

#define VERSION             "0.1.0"
#define CAPS_VERSION        1
#define MAX_OUTPUTS         4
#define LEGACY_OUTPUTS      3
#define MAX_PEERS           16
#define MAX_STREAMS_LIMIT   256
#define IDLE_TIMEOUT_S      3.0
#define PKT_MAX             2048

typedef struct {
    int used;
    uint16_t id;
    uint16_t port;
    int fd;
    struct sockaddr_in peer;        /* control address of the opener */
    int extended;
    uint32_t tx;
    uint8_t in;
    uint8_t outs[MAX_OUTPUTS];      /* open order, 0 padded */
    int nouts;
    int legacy_width[LEGACY_OUTPUTS];
    uint8_t legacy_fill[LEGACY_OUTPUTS];
    vn_codec *codec;
    double last;
    unsigned long frames;
} stream_t;

static struct {
    struct in_addr bind;
    int base_port;
    int max_streams;
    struct in_addr peers[MAX_PEERS];
    int npeers;
    uint32_t mask;                  /* codecs offered to extended opens */
    int legacy_only;                /* behave like a pre-extension ambed */
    int verbose;
} cfg;

static stream_t *streams;
static int ctl_fd = -1;
static uint16_t last_id;
static volatile sig_atomic_t stop_flag;

/* ------------------------------------------------------------------ utils */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

static const char *addr_str(const struct sockaddr_in *a)
{
    static char buf[32];
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    snprintf(buf, sizeof(buf), "%s:%u", ip, ntohs(a->sin_port));
    return buf;
}

static int peer_allowed(const struct sockaddr_in *a)
{
    if (cfg.npeers == 0)
        return 1;
    for (int i = 0; i < cfg.npeers; i++)
        if (cfg.peers[i].s_addr == a->sin_addr.s_addr)
            return 1;
    return 0;
}

static int udp_open(struct in_addr ip, int port)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr = ip;
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void send_to(int fd, const uint8_t *buf, size_t len, const struct sockaddr_in *to)
{
    if (sendto(fd, buf, len, 0, (const struct sockaddr *)to, sizeof(*to)) < 0 && cfg.verbose)
        logf_("sendto %s: %s", addr_str(to), strerror(errno));
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Stream id and port are host-order uint16 in the reference; every known
 * deployment is little endian, so they are little endian on the wire. */
static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Reference callsign rule: 3 leading [A-Z0-9] not all digits, then
 * [A-Z0-9 ]; NUL bytes count as spaces. */
static int callsign_valid(const uint8_t *cs)
{
    int digits = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = cs[i] ? cs[i] : ' ';
        int letter = c >= 'A' && c <= 'Z';
        int digit = c >= '0' && c <= '9';
        if (i < 3) {
            if (!letter && !digit)
                return 0;
            digits += digit;
        } else if (!letter && !digit && c != ' ') {
            return 0;
        }
    }
    return digits < 3;
}

static int legacy_codec(int c)
{
    return c >= VN_CODEC_AMBEP && c <= VN_CODEC_IMBE;
}

static int ext_codec(int c)
{
    return c >= VN_CODEC_AMBEP && c <= VN_CODEC_MAX && (cfg.mask & (1u << c));
}

static int is_ambe(int c)
{
    return c == VN_CODEC_AMBEP || c == VN_CODEC_AMBE2P;
}

/* ---------------------------------------------------------------- streams */

static void stream_close(stream_t *s, const char *why)
{
    if (!s->used)
        return;
    logf_("stream %u closed (%s), %lu frames", s->id, why, s->frames);
    close(s->fd);
    vn_codec_close(s->codec);
    memset(s, 0, sizeof(*s));
    s->fd = -1;
}

/* Legacy reply layout as in the reference: three slots whose widths follow
 * the input codec (the slots a DVSI channel pair plus the software codecs
 * fill), each carrying the open's output byte. A slot holds the encoded
 * frame of fill, or zeros when fill is 0. */
static void legacy_layout(stream_t *s)
{
    if (is_ambe(s->in)) {
        s->legacy_width[0] = 9;
        s->legacy_fill[0] = s->outs[0];
        s->legacy_width[1] = 8;
        s->legacy_fill[1] = s->outs[1] == VN_CODEC_CODEC2 ? VN_CODEC_CODEC2 : 0;
        s->legacy_width[2] = 11;
        s->legacy_fill[2] = s->outs[2] == VN_CODEC_IMBE ? VN_CODEC_IMBE : 0;
    } else {
        int third = s->in == VN_CODEC_CODEC2 ? VN_CODEC_IMBE : VN_CODEC_CODEC2;
        s->legacy_width[0] = 9;
        s->legacy_fill[0] = s->outs[0];
        s->legacy_width[1] = 9;
        s->legacy_fill[1] = s->outs[1];
        s->legacy_width[2] = vn_codec_width(third);
        s->legacy_fill[2] = (uint8_t)third;
    }
}

static stream_t *stream_open(const struct sockaddr_in *peer, int in, const uint8_t *outs, int nouts,
                             int extended, uint32_t tx)
{
    stream_t *s = NULL;
    uint8_t enc[MAX_OUTPUTS];
    int nenc = 0;

    for (int i = 0; i < cfg.max_streams; i++)
        if (!streams[i].used) {
            s = &streams[i];
            break;
        }
    if (s == NULL) {
        logf_("max streams reached (%d), rejecting", cfg.max_streams);
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->peer = *peer;
    s->extended = extended;
    s->tx = tx;
    s->in = (uint8_t)in;
    s->nouts = nouts;
    memcpy(s->outs, outs, (size_t)nouts);
    if (extended) {
        memcpy(enc, outs, (size_t)nouts);
        nenc = nouts;
    } else {
        legacy_layout(s);
        for (int i = 0; i < LEGACY_OUTPUTS; i++)
            if (s->legacy_fill[i])
                enc[nenc++] = s->legacy_fill[i];
    }

    /* next free id after the last one, wrapping at max_streams like the
     * reference; the data port is base port + id */
    for (int tries = 0; tries < cfg.max_streams; tries++) {
        uint16_t id = (uint16_t)(last_id + 1);
        int busy = 0;
        last_id = id > cfg.max_streams ? 1 : id;
        for (int i = 0; i < cfg.max_streams; i++)
            busy |= streams[i].used && streams[i].id == last_id;
        if (!busy)
            break;
    }
    s->id = last_id;
    s->port = (uint16_t)(cfg.base_port + s->id);
    s->fd = udp_open(cfg.bind, s->port);
    if (s->fd < 0) {
        logf_("stream %u: cannot bind UDP %d: %s", s->id, s->port, strerror(errno));
        memset(s, 0, sizeof(*s));
        return NULL;
    }
    s->codec = vn_codec_open(in, enc, nenc);
    if (s->codec == NULL) {
        logf_("stream %u: codec state allocation failed", s->id);
        close(s->fd);
        memset(s, 0, sizeof(*s));
        return NULL;
    }
    s->used = 1;
    s->last = now_s();
    logf_("stream %u opened (%s) from %s in=%d out=%d,%d,%d,%d port %u", s->id,
          extended ? "extended" : "legacy", addr_str(peer), in, s->outs[0], s->outs[1],
          s->outs[2], s->outs[3], s->port);
    return s;
}

static stream_t *stream_by_tx(const struct sockaddr_in *peer, uint32_t tx, int in, const uint8_t *outs)
{
    for (int i = 0; i < cfg.max_streams; i++) {
        stream_t *s = &streams[i];
        if (s->used && s->extended && s->tx == tx && s->in == in &&
            s->peer.sin_addr.s_addr == peer->sin_addr.s_addr && s->peer.sin_port == peer->sin_port &&
            memcmp(s->outs, outs, MAX_OUTPUTS) == 0)
            return s;
    }
    return NULL;
}

/* ---------------------------------------------------------------- control */

static size_t encode_std(uint8_t *buf, const stream_t *s)
{
    memcpy(buf, "AMBEDSTD", 8);
    put_le16(buf + 8, s->id);
    put_le16(buf + 10, s->port);
    buf[12] = s->in;
    buf[13] = s->outs[0];
    return 14;
}

static void on_ext_open(const uint8_t *p, const struct sockaddr_in *from)
{
    uint8_t reply[32];
    size_t len;
    int in = p[15];
    const uint8_t *outs = p + 16;
    uint32_t tx = get_be32(p + 20);
    int n = 0, ok;
    stream_t *s = NULL;

    /* at least one output, no gaps, distinct, none equal to the input */
    while (n < MAX_OUTPUTS && outs[n] != VN_CODEC_NONE)
        n++;
    ok = callsign_valid(p + 7) && ext_codec(in) && n > 0;
    for (int i = 0; i < MAX_OUTPUTS && ok; i++)
        ok = i >= n ? outs[i] == VN_CODEC_NONE
                    : ext_codec(outs[i]) && outs[i] != in && memchr(outs, outs[i], (size_t)i) == NULL;
    if (ok) {
        s = stream_by_tx(from, tx, in, outs);
        if (s != NULL && cfg.verbose)
            logf_("retransmitted open tx=0x%08x from %s -> stream %u", tx, addr_str(from), s->id);
        if (s == NULL)
            s = stream_open(from, in, outs, n, 1, tx);
    } else {
        logf_("invalid extended open from %s tx=0x%08x", addr_str(from), tx);
    }
    if (s != NULL) {
        len = encode_std(reply, s);
    } else {
        memcpy(reply, "AMBEDBUSY", 9);
        len = 9;
    }
    put_be32(reply + len, tx);
    send_to(ctl_fd, reply, len + 4, from);
}

static void on_legacy_open(const uint8_t *p, size_t len, const struct sockaddr_in *from)
{
    uint8_t reply[16];
    uint8_t outs[MAX_OUTPUTS] = {0};
    int in = p[15];
    int n = (int)len - 16;
    int valid, served;
    stream_t *s = NULL;

    memcpy(outs, p + 16, (size_t)n);
    /* reference validity: 17 B one output, 18 B two, 19 B two plus an
     * optional third; invalid opens get no reply */
    valid = callsign_valid(p + 7) && legacy_codec(in) && legacy_codec(outs[0]);
    if (n >= 2)
        valid = valid && legacy_codec(outs[1]);
    if (n == 3 && outs[2] != VN_CODEC_NONE)
        valid = valid && legacy_codec(outs[2]);
    if (!valid) {
        if (cfg.verbose)
            logf_("invalid legacy open from %s ignored", addr_str(from));
        return;
    }
    /* the combinations a reference ambed with DVSI channels serves: AMBE
     * input pairs with the other AMBE codec as output 1, Codec2 and IMBE
     * input need AMBE codecs as outputs 1 and 2 */
    if (is_ambe(in))
        served = outs[0] == (in == VN_CODEC_AMBEP ? VN_CODEC_AMBE2P : VN_CODEC_AMBEP);
    else
        served = is_ambe(outs[0]) && is_ambe(outs[1]);
    if (served)
        s = stream_open(from, in, outs, n, 0, 0);
    else
        logf_("legacy open %d->%d,%d,%d from %s not served (reference rule), BUSY", in, outs[0],
              outs[1], outs[2], addr_str(from));
    if (s != NULL) {
        send_to(ctl_fd, reply, encode_std(reply, s), from);
    } else {
        memcpy(reply, "AMBEDBUSY", 9);
        send_to(ctl_fd, reply, 9, from);
    }
}

static void on_control(const uint8_t *p, size_t len, const struct sockaddr_in *from)
{
    uint8_t reply[32];

    if (len == 9 && memcmp(p, "AMBEDCAPS", 9) == 0) {
        /* a transcoder without the extension never answers CAPS */
        if (cfg.legacy_only)
            return;
        memcpy(reply, "AMBEDCAPS", 9);
        reply[9] = CAPS_VERSION;
        reply[10] = MAX_OUTPUTS;
        put_be32(reply + 11, cfg.mask);
        send_to(ctl_fd, reply, 15, from);
    } else if (len == 24 && memcmp(p, "AMBEDOS", 7) == 0) {
        /* ... and ignores the 24 B open */
        if (!cfg.legacy_only)
            on_ext_open(p, from);
        else if (cfg.verbose)
            logf_("legacy only: extended open from %s ignored", addr_str(from));
    } else if (len >= 17 && len <= 19 && memcmp(p, "AMBEDOS", 7) == 0) {
        on_legacy_open(p, len, from);
    } else if (len >= 9 && memcmp(p, "AMBEDCS", 7) == 0) {
        uint16_t id = (uint16_t)(p[7] | (p[8] << 8));
        for (int i = 0; i < cfg.max_streams; i++)
            if (streams[i].used && streams[i].id == id)
                stream_close(&streams[i], "CS");
    } else if (len == 17 && memcmp(p, "AMBEDPING", 9) == 0) {
        if (callsign_valid(p + 9))
            send_to(ctl_fd, (const uint8_t *)"AMBEDPONG", 9, from);
    } else if (cfg.verbose) {
        logf_("unknown %zu B control packet from %s", len, addr_str(from));
    }
}

/* ------------------------------------------------------------------- data */

static void on_data(stream_t *s, const uint8_t *p, size_t len)
{
    uint8_t reply[2 + MAX_OUTPUTS * (1 + VN_FRAME_MAX)];
    int16_t pcm[VN_PCM_SAMPLES];
    struct sockaddr_in to;
    size_t off = 0;

    if (len != (size_t)(2 + vn_codec_width(s->in)) || p[0] != s->in) {
        if (cfg.verbose)
            logf_("stream %u: unexpected %zu B data packet", s->id, len);
        return;
    }
    s->last = now_s();
    s->frames++;
    vn_codec_decode(s->codec, p + 2, pcm);

    /* entries in open order, the pid after the first codec byte */
    if (s->extended) {
        for (int i = 0; i < s->nouts; i++) {
            reply[off++] = s->outs[i];
            if (i == 0)
                reply[off++] = p[1];
            vn_codec_encode(s->codec, s->outs[i], pcm, reply + off);
            off += (size_t)vn_codec_width(s->outs[i]);
        }
    } else {
        for (int i = 0; i < LEGACY_OUTPUTS; i++) {
            reply[off++] = s->outs[i];
            if (i == 0)
                reply[off++] = p[1];
            if (s->legacy_fill[i])
                vn_codec_encode(s->codec, s->legacy_fill[i], pcm, reply + off);
            else
                memset(reply + off, 0, (size_t)s->legacy_width[i]);
            off += (size_t)s->legacy_width[i];
        }
    }

    /* the reply goes to the opener's address at the stream port */
    to = s->peer;
    to.sin_port = htons(s->port);
    send_to(s->fd, reply, off, &to);
}

/* ------------------------------------------------------------------- main */

static void on_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

static void usage(FILE *f, const char *prog)
{
    fprintf(f,
            "vambed-neo %s - AMBEd-compatible software transcoder for reflector testing\n"
            "\n"
            "usage: %s [options]\n"
            "  -b, --bind IP          listen address (default 0.0.0.0)          env VAMBED_NEO_BIND\n"
            "  -p, --base-port N      control port; stream N uses base+N (10100) env VAMBED_NEO_BASE_PORT\n"
            "  -m, --max-streams N    concurrent streams (default 16)            env VAMBED_NEO_MAX_STREAMS\n"
            "  -P, --peer IP          accept only this peer (repeatable; default any)\n"
            "                                                                     env VAMBED_NEO_PEERS (space separated)\n"
            "  -c, --codecs LIST      codecs offered by CAPS / extended opens, e.g. 1,2,3,4\n"
            "                         (default all: 1,2,3,4,5)                  env VAMBED_NEO_CODECS\n"
            "  -L, --legacy-only      no CAPS reply, 24 B opens ignored (pre-extension ambed)\n"
            "                                                                     env VAMBED_NEO_LEGACY_ONLY=1\n"
            "  -v, --verbose          log every packet decision                  env VAMBED_NEO_VERBOSE=1\n"
            "      --check-rng        exit 0 if Codec2 output is deterministic per stream\n"
            "  -h, --help             this text\n"
            "\n"
            "Codecs: 1 AMBE+ (D-STAR), 2 AMBE+2 (DMR/YSF), 3 Codec2 3200 (M17), 4 IMBE (P25),\n"
            "5 PCM S16LE 8 kHz (extended opens only). Test tool: audio quality is not a goal.\n",
            VERSION, prog);
}

static int parse_int(const char *s, int lo, int hi, const char *what)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi) {
        fprintf(stderr, "invalid %s: %s (%d..%d)\n", what, s, lo, hi);
        exit(2);
    }
    return (int)v;
}

static void add_peer(const char *ip)
{
    if (cfg.npeers >= MAX_PEERS || inet_pton(AF_INET, ip, &cfg.peers[cfg.npeers]) != 1) {
        fprintf(stderr, "invalid or too many --peer: %s\n", ip);
        exit(2);
    }
    cfg.npeers++;
}

static void parse_codecs(const char *list)
{
    char buf[64];
    uint32_t mask = 0;
    snprintf(buf, sizeof(buf), "%s", list);
    for (char *tok = strtok(buf, " ,"); tok != NULL; tok = strtok(NULL, " ,"))
        mask |= 1u << parse_int(tok, VN_CODEC_AMBEP, VN_CODEC_MAX, "codec");
    if ((mask & vn_codec_mask()) == 0) {
        fprintf(stderr, "invalid --codecs: %s\n", list);
        exit(2);
    }
    cfg.mask = mask & vn_codec_mask();
}

static void parse_bind(const char *ip)
{
    if (inet_pton(AF_INET, ip, &cfg.bind) != 1) {
        fprintf(stderr, "invalid --bind: %s\n", ip);
        exit(2);
    }
}

static void parse_env(void)
{
    const char *v;
    if ((v = getenv("VAMBED_NEO_BIND")) != NULL && *v)
        parse_bind(v);
    if ((v = getenv("VAMBED_NEO_BASE_PORT")) != NULL && *v)
        cfg.base_port = parse_int(v, 1, 65535 - MAX_STREAMS_LIMIT, "base port");
    if ((v = getenv("VAMBED_NEO_MAX_STREAMS")) != NULL && *v)
        cfg.max_streams = parse_int(v, 1, MAX_STREAMS_LIMIT, "max streams");
    if ((v = getenv("VAMBED_NEO_CODECS")) != NULL && *v)
        parse_codecs(v);
    if ((v = getenv("VAMBED_NEO_LEGACY_ONLY")) != NULL && *v && strcmp(v, "0") != 0)
        cfg.legacy_only = 1;
    if ((v = getenv("VAMBED_NEO_VERBOSE")) != NULL && *v && strcmp(v, "0") != 0)
        cfg.verbose = 1;
    if ((v = getenv("VAMBED_NEO_PEERS")) != NULL && *v) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", v);
        for (char *tok = strtok(buf, " ,"); tok != NULL; tok = strtok(NULL, " ,"))
            add_peer(tok);
    }
}

static void parse_args(int argc, char **argv)
{
    static const struct option opts[] = {
        {"bind", required_argument, NULL, 'b'},
        {"base-port", required_argument, NULL, 'p'},
        {"max-streams", required_argument, NULL, 'm'},
        {"peer", required_argument, NULL, 'P'},
        {"codecs", required_argument, NULL, 'c'},
        {"legacy-only", no_argument, NULL, 'L'},
        {"verbose", no_argument, NULL, 'v'},
        {"check-rng", no_argument, NULL, 'R'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int c, cli_peers = 0;

    while ((c = getopt_long(argc, argv, "b:p:m:P:c:Lvh", opts, NULL)) != -1) {
        switch (c) {
        case 'b': parse_bind(optarg); break;
        case 'p': cfg.base_port = parse_int(optarg, 1, 65535 - MAX_STREAMS_LIMIT, "base port"); break;
        case 'm': cfg.max_streams = parse_int(optarg, 1, MAX_STREAMS_LIMIT, "max streams"); break;
        case 'P':
            /* command line peers replace the environment list */
            if (!cli_peers++)
                cfg.npeers = 0;
            add_peer(optarg);
            break;
        case 'c': parse_codecs(optarg); break;
        case 'L': cfg.legacy_only = 1; break;
        case 'v': cfg.verbose = 1; break;
        case 'h': usage(stdout, argv[0]); exit(0);
        case 'R':
            /* build gate: Codec2 output must not depend on other streams */
            if (vn_codec_rng_per_stream()) {
                puts("codec2 random phases: per stream");
                exit(0);
            }
            puts("codec2 random phases: process wide (codec2_rand not interposed)");
            exit(1);
        default: usage(stderr, argv[0]); exit(2);
        }
    }
    if (optind < argc) {
        fprintf(stderr, "unexpected argument: %s\n", argv[optind]);
        exit(2);
    }
}

int main(int argc, char **argv)
{
    struct pollfd *pfd;
    stream_t **pstream;
    struct sigaction sa;
    char ip[INET_ADDRSTRLEN];

    setvbuf(stdout, NULL, _IOLBF, 0);
    cfg.bind.s_addr = htonl(INADDR_ANY);
    cfg.base_port = 10100;
    cfg.max_streams = 16;
    cfg.mask = vn_codec_mask();
    parse_env();
    parse_args(argc, argv);

    streams = calloc((size_t)cfg.max_streams, sizeof(*streams));
    pfd = calloc((size_t)cfg.max_streams + 1, sizeof(*pfd));
    pstream = calloc((size_t)cfg.max_streams + 1, sizeof(*pstream));
    if (streams == NULL || pfd == NULL || pstream == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    ctl_fd = udp_open(cfg.bind, cfg.base_port);
    if (ctl_fd < 0) {
        inet_ntop(AF_INET, &cfg.bind, ip, sizeof(ip));
        fprintf(stderr, "cannot bind UDP %s:%d: %s\n", ip, cfg.base_port, strerror(errno));
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    inet_ntop(AF_INET, &cfg.bind, ip, sizeof(ip));
    logf_("vambed-neo %s listening on %s:%d, max %d streams, %s, peers: %s", VERSION, ip,
          cfg.base_port, cfg.max_streams, cfg.legacy_only ? "legacy only" : "extended",
          cfg.npeers ? "restricted" : "any");
    if (!cfg.legacy_only)
        logf_("  CAPS codec mask 0x%02x", cfg.mask);
    logf_("  codec2 random phases: %s", vn_codec_rng_per_stream() ? "per stream (deterministic)"
                                                                  : "process wide (library internal)");
    for (int i = 0; i < cfg.npeers; i++) {
        inet_ntop(AF_INET, &cfg.peers[i], ip, sizeof(ip));
        logf_("  peer %s", ip);
    }

    while (!stop_flag) {
        uint8_t buf[PKT_MAX];
        struct sockaddr_in from;
        socklen_t fromlen;
        int n = 0;
        double t;

        pfd[n].fd = ctl_fd;
        pfd[n].events = POLLIN;
        pstream[n++] = NULL;
        for (int i = 0; i < cfg.max_streams; i++)
            if (streams[i].used) {
                pfd[n].fd = streams[i].fd;
                pfd[n].events = POLLIN;
                pstream[n++] = &streams[i];
            }

        if (poll(pfd, (nfds_t)n, 100) < 0 && errno != EINTR) {
            perror("poll");
            break;
        }
        for (int i = 0; i < n && !stop_flag; i++) {
            ssize_t len;
            if (!(pfd[i].revents & POLLIN))
                continue;
            /* a CS earlier in this pass may have closed this stream */
            if (pstream[i] != NULL && (!pstream[i]->used || pstream[i]->fd != pfd[i].fd))
                continue;
            fromlen = sizeof(from);
            len = recvfrom(pfd[i].fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
            if (len <= 0 || !peer_allowed(&from))
                continue;
            if (pstream[i] == NULL)
                on_control(buf, (size_t)len, &from);
            else
                on_data(pstream[i], buf, (size_t)len);
        }

        t = now_s();
        for (int i = 0; i < cfg.max_streams; i++)
            if (streams[i].used && t - streams[i].last > IDLE_TIMEOUT_S)
                stream_close(&streams[i], "activity timeout");
    }

    for (int i = 0; i < cfg.max_streams; i++)
        stream_close(&streams[i], "shutdown");
    close(ctl_fd);
    logf_("vambed-neo stopped");
    return 0;
}
