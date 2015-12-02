/*
 * @f ccn-lite-repo256.c
 * @b user space repo, access via SHA256 digest as well as exact name match
 *
 * Copyright (C) 2015, Christian Tschudin, University of Basel
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * File history:
 * 2015-11-26 created
 */


/*

OKset = hash table (set) of verified file content
NMmap = hash table (map) of verified names, pointing to the hash value
ERset = hash table (set) of files know to have wrong name (for the hash val)
NOset = hash table (set) of files know to not exist (for the given hash val)
CAmap = hash table (map) of learned content hashes, pointing to the hash val

File system structure:

  <dirpath>
  <dirpath>/XY/<62 hex digits>
           where XY are the two topmost hex digits of the file's digest

Todo:

a) put the loading of file content as back ground task
   in order to increase startup time.

b) longest prefix match (and use of balanced tree?)

c) refactor with ccn-lite-relay.c code

d) enable content store caching, or rely on OS paging?

*/

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>

#include "lib-khash.h"

#define CCNL_UNIX

#define USE_CCNxDIGEST
#define USE_DEBUG                      // must select this for USE_MGMT
#define USE_DEBUG_MALLOC
// #define USE_ETHERNET
#define USE_HMAC256
#define USE_IPV4
//#define USE_IPV6
#define USE_NAMELESS

// #define USE_SUITE_CCNB                 // must select this for USE_MGMT
#define USE_SUITE_CCNTLV
// #define USE_SUITE_CISTLV
// #define USE_SUITE_IOTTLV
#define USE_SUITE_NDNTLV
// #define USE_SUITE_LOCALRPC
#define USE_UNIXSOCKET

#define NEEDS_PREFIX_MATCHING

#include "ccnl-os-includes.h"
#include "ccnl-defs.h"
#include "ccnl-core.h"

#include "ccnl-ext.h"
#include "ccnl-ext-debug.c"
#include "ccnl-os-time.c"
#include "ccnl-ext-logging.c"

#define ccnl_app_RX(x,y)                do{}while(0)
#define local_producer(...)             0

#include "ccnl-core.c"

// ----------------------------------------------------------------------

struct ccnl_relay_s theRepo;
char *theDirPath;
#ifdef USE_LOGGING
  char prefixBuf[CCNL_PREFIX_BUFSIZE];
#endif
unsigned char iobuf[64*2014];

enum {
    MODE_FILE,   // for each interest visit directly the file system
    MODE_INDEX   // build an internal index, check there before read()
};
int repo_mode = MODE_INDEX;

// ----------------------------------------------------------------------
// khash.h specifics

#undef kcalloc
#undef kmalloc
#undef krealloc
#undef kfree
#define kcalloc(N,Z)  ccnl_calloc(N,Z)
#define kmalloc(Z)    ccnl_malloc(Z)
#define krealloc(P,Z) ccnl_realloc(P,Z)
#define kfree(P)      ccnl_free(P)

khint_t
sha256toInt(unsigned char *md)
{
    khint_t *ip = (khint_t*) (md+1), val = *md ^ *ip++;
    int i;
    for (i = SHA256_DIGEST_LENGTH / sizeof(khint_t); i > 1; i--)
        val ^= *ip++;

    return val;
}

#define sha256equal(a, b) (!memcmp(a, b, SHA256_DIGEST_LENGTH+1))
typedef unsigned char* kh256_t;


struct khPFX_s {
    unsigned short len;
    unsigned char mem[1];
};
typedef struct khPFX_s* khPFX_t;

khint_t
khPFXtoInt(khPFX_t pfx)
{
    unsigned char *s = pfx->mem;
    int i = pfx->len;
    khint_t h = *s;

    while (--i > 0)
        h = (h << 5) - h + *++s;
    return h;
}

#define khPFXequal(a, b) ((a->len == b->len) && !memcmp(a->mem, b->mem, a->len))

KHASH_INIT(256, kh256_t, char, 0, sha256toInt, sha256equal)
khash_t(256) *OKset;
khash_t(256) *ERset;
khash_t(256) *NOset;

KHASH_INIT(PFX, khPFX_t, unsigned char*, 1, khPFXtoInt, khPFXequal)
khash_t(PFX) *NMmap;

// ----------------------------------------------------------------------

void
assertDir(char *dirpath, char *cp)
{
    char *fn;

    asprintf(&fn, "%s/%c%c", dirpath, cp[0], cp[1]);
    if (mkdir(fn, 0777) && errno != EEXIST) {
        DEBUGMSG(FATAL, "could not create directory %s\n", fn);
        exit(-1);
    }
    free(fn);
}

static char*
digest2str(unsigned char *md)
{
    static char tmp[80];
    int i;
    for (i = 0; i < SHA256_DIGEST_LENGTH; i++)
        sprintf(tmp + 2*i, "%02x", md[i]);
    return tmp;
}

int
file2iobuf(char *path)
{
    int f = open(path, O_RDONLY), len;
    if (f < 0)
        return -1;
    len = read(f, iobuf, sizeof(iobuf));
    close(f);

    return len;
}

char*
digest2fname(char *dirpath, unsigned char *md)
{
    char *cp, *hex = digest2str(md);

    asprintf(&cp, "%s/%02x/%s", dirpath, (unsigned) md[0], hex+2);

    return cp;
}

unsigned char*
digest2key(int suite, unsigned char *digest)
{
    static unsigned char md[SHA256_DIGEST_LENGTH + 1];
    md[0] = suite;
    memcpy(md + 1, digest, SHA256_DIGEST_LENGTH);
    return md;
}

// ----------------------------------------------------------------------

#ifdef USE_UNIXSOCKET
int
ccnl_open_unixpath(char *path, struct sockaddr_un *ux)
{
    int sock, bufsize;

    sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("opening datagram socket");
        return -1;
    }

    unlink(path);
    ccnl_setUnixSocketPath(ux, path);

    if (bind(sock, (struct sockaddr *) ux, sizeof(struct sockaddr_un))) {
        perror("binding name to datagram socket");
        close(sock);
        return -1;
    }

    bufsize = CCNL_MAX_SOCK_SPACE;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    return sock;

}
#endif // USE_UNIXSOCKET

#ifdef USE_IPV4
int
ccnl_open_udpdev(int port, struct sockaddr_in *si)
{
    int s;
    unsigned int len;

    s = socket(PF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        perror("udp socket");
        return -1;
    }

    si->sin_addr.s_addr = INADDR_ANY;
    si->sin_port = htons(port);
    si->sin_family = PF_INET;
    if(bind(s, (struct sockaddr *)si, sizeof(*si)) < 0) {
        perror("udp sock bind");
        return -1;
    }
    len = sizeof(*si);
    getsockname(s, (struct sockaddr*) si, &len);

    return s;
}
#elif defined(USE_IPV6)
int
ccnl_open_udpdev(int port, struct sockaddr_in6 *sin)
{
    int s;
    unsigned int len;

    s = socket(PF_INET6, SOCK_DGRAM, 0);
    if (s < 0) {
        perror("udp socket");
        return -1;
    }

    sin->sin6_addr = in6addr_any;
    sin->sin6_port = htons(port);
    sin->sin6_family = PF_INET6;
    if(bind(s, (struct sockaddr *)sin, sizeof(*sin)) < 0) {
        perror("udp sock bind");
        return -1;
    }
    len = sizeof(*sin);
    getsockname(s, (struct sockaddr*) sin, &len);

    return s;
}
#endif

void
ccnl_ll_TX(struct ccnl_relay_s *ccnl, struct ccnl_if_s *ifc,
           sockunion *dest, struct ccnl_buf_s *buf)
{
    int rc;

    switch(dest->sa.sa_family) {
#ifdef USE_IPV4
    case AF_INET:
        rc = sendto(ifc->sock,
                    buf->data, buf->datalen, 0,
                    (struct sockaddr*) &dest->ip4, sizeof(struct sockaddr_in));
        DEBUGMSG(DEBUG, "udp sendto(%d Bytes) to %s/%d returned %d/%d\n",
                 (int) buf->datalen,
                 inet_ntoa(dest->ip4.sin_addr), ntohs(dest->ip4.sin_port),
                 rc, errno);
        /*
        {
            int fd = open("t.bin", O_WRONLY | O_CREAT | O_TRUNC, 0666);
            write(fd, buf->data, buf->datalen);
            close(fd);
        }
        */

        break;
#endif
#ifdef USE_ETHERNET
    case AF_PACKET:
        rc = ccnl_eth_sendto(ifc->sock,
                             dest->eth.sll_addr,
                             ifc->addr.eth.sll_addr,
                             buf->data, buf->datalen);
        DEBUGMSG(DEBUG, "eth_sendto %s returned %d\n",
                 eth2ascii(dest->eth.sll_addr), rc);
        break;
#endif
#ifdef USE_UNIXSOCKET
    case AF_UNIX:
        rc = sendto(ifc->sock,
                    buf->data, buf->datalen, 0,
                    (struct sockaddr*) &dest->ux, sizeof(struct sockaddr_un));
        DEBUGMSG(DEBUG, "unix sendto(%d Bytes) to %s returned %d\n",
                 (int) buf->datalen, dest->ux.sun_path, rc);
        break;
#endif
    default:
        DEBUGMSG(WARNING, "unknown transport\n");
        break;
    }
    (void) rc; // just to silence a compiler warning (if USE_DEBUG is not set)
}

void
ccnl_close_socket(int s)
{
    struct sockaddr_un su;
    socklen_t len = sizeof(su);

    if (!getsockname(s, (struct sockaddr*) &su, &len) &&
                                        su.sun_family == AF_UNIX) {
        unlink(su.sun_path);
    }
    close(s);
}

// ----------------------------------------------------------------------

#ifdef USE_IPV4
void
ccnl_repo256_udp(struct ccnl_relay_s *relay, int port)
{
    struct ccnl_if_s *i;

    if (port < 0)
        return;
    i = &relay->ifs[relay->ifcount];
    i->sock = ccnl_open_udpdev(port, &i->addr.ip4);
    if (i->sock <= 0) {
        DEBUGMSG(WARNING, "sorry, could not open udp device (port %d)\n",
                 port);
        return;
    }

    relay->ifcount++;
    DEBUGMSG(INFO, "UDP interface (%s) configured\n",
             ccnl_addr2ascii(&i->addr));
    if (relay->defaultInterfaceScheduler)
        i->sched = relay->defaultInterfaceScheduler(relay,
                                                        ccnl_interface_CTS);
}
#endif

void
ccnl_repo256_config(struct ccnl_relay_s *relay, char *ethdev, int udpport,
                    char *uxpath, int max_cache_entries)
{
#if defined(USE_ETHERNET) || defined(USE_UNIXSOCKET)
    struct ccnl_if_s *i;
#endif

    DEBUGMSG(INFO, "configuring repo in '%s mode'\n",
             repo_mode == MODE_FILE ? "file" : "index");

    relay->max_cache_entries = max_cache_entries;
#ifdef USE_SCHEDULER
    relay->defaultFaceScheduler = ccnl_relay_defaultFaceScheduler;
    relay->defaultInterfaceScheduler = ccnl_relay_defaultInterfaceScheduler;
#endif

#ifdef USE_ETHERNET
    // add (real) eth0 interface with index 0:
    if (ethdev) {
        i = &relay->ifs[relay->ifcount];
        i->sock = ccnl_open_ethdev(ethdev, &i->addr.eth, CCNL_ETH_TYPE);
        i->mtu = 1500;
        i->reflect = 1;
        i->fwdalli = 1;
        if (i->sock >= 0) {
            relay->ifcount++;
            DEBUGMSG(INFO, "ETH interface (%s %s) configured\n",
                     ethdev, ccnl_addr2ascii(&i->addr));
            if (relay->defaultInterfaceScheduler)
                i->sched = relay->defaultInterfaceScheduler(relay,
                                                        ccnl_interface_CTS);
        } else
            DEBUGMSG(WARNING, "sorry, could not open eth device\n");
    }
#endif // USE_ETHERNET

#ifdef USE_IPV4
    ccnl_repo256_udp(relay, udpport);
#endif

#ifdef USE_UNIXSOCKET
    if (uxpath) {
        i = &relay->ifs[relay->ifcount];
        i->sock = ccnl_open_unixpath(uxpath, &i->addr.ux);
        i->mtu = 4096;
        if (i->sock >= 0) {
            relay->ifcount++;
            DEBUGMSG(INFO, "UNIX interface (%s) configured\n",
                     ccnl_addr2ascii(&i->addr));
            if (relay->defaultInterfaceScheduler)
                i->sched = relay->defaultInterfaceScheduler(relay,
                                                        ccnl_interface_CTS);
        } else
            DEBUGMSG(WARNING, "sorry, could not open unix datagram device\n");
    }
#endif // USE_UNIXSOCKET

//    ccnl_set_timer(1000000, ccnl_ageing, relay, 0);
}

// ----------------------------------------------------------------------

int
ccnl_repo256(struct ccnl_relay_s *repo, struct ccnl_face_s *from,
             int suite, int skip, unsigned char **data, int *datalen)
{
    int rc = -1;
    unsigned char *start, *requestByDigest = NULL, *digest = NULL;
    struct ccnl_pkt_s *pkt = NULL;
    khint_t k;

    DEBUGMSG(DEBUG, "ccnl_repo (suite=%s, skip=%d, %d bytes left)\n",
             ccnl_suite2str(suite), skip, *datalen);

    *data += skip;
    *datalen -=  skip;
    start = *data;

    switch(suite) {
    case CCNL_SUITE_CCNTLV: {
        int hdrlen = ccnl_ccntlv_getHdrLen(*data, *datalen);
        *data += hdrlen;
        *datalen -= hdrlen;
        pkt = ccnl_ccntlv_bytes2pkt(start, data, datalen);
        if (!pkt || pkt->type != CCNX_TLV_TL_Interest)
            goto Done;
        requestByDigest = pkt->s.ccntlv.objHashRestr;
        break;
    }
    case CCNL_SUITE_NDNTLV:
        pkt = ccnl_ndntlv_bytes2pkt(start, data, datalen);
        if (!pkt || pkt->type != NDN_TLV_Interest)
            goto Done;
        requestByDigest = pkt->s.ndntlv.dataHashRestr;
        break;
    default:
        goto Done;
    }

    if (!pkt) {
        DEBUGMSG(INFO, "  packet decoding problem\n");
        goto Done;
    }

    if (requestByDigest) {
        DEBUGMSG(DEBUG, "lookup %s\n", digest2str(requestByDigest));
        if (repo_mode == MODE_INDEX) {
            k = kh_get(256, OKset, digest2key(suite, requestByDigest));
            if (k != kh_end(OKset)) {
                DEBUGMSG(DEBUG, "  found OKset entry at position %d\n", k);
                digest = requestByDigest;
            }
        } else if (repo_mode == MODE_FILE) {
            unsigned char *key = digest2key(suite, requestByDigest);
            k = kh_get(256, ERset, key);
            if (k != kh_end(ERset)) { // bad hash value in file
                DEBUGMSG(DEBUG, "  ERset hit - request discarded\n");
                goto Done;
            }
            k = kh_get(256, NOset, key);
            if (k != kh_end(NOset)) { // known to be absent
                DEBUGMSG(DEBUG, "  NOset hit - request discarded\n");
                goto Done;
            }
            digest = requestByDigest;
        }
    } else {
        khPFX_t n;

        DEBUGMSG(DEBUG, "lookup by name [%s]%s, %p/%d\n",
                 ccnl_prefix2path(prefixBuf,
                                  CCNL_ARRAY_SIZE(prefixBuf),
                                  pkt->pfx),
                 ccnl_suite2str(suite),
                 pkt->pfx->nameptr, (int)(pkt->pfx->namelen));

        n = ccnl_malloc(sizeof(struct khPFX_s) + pkt->pfx->namelen);
        n->len = 1 + pkt->pfx->namelen;
        n->mem[0] = pkt->pfx->suite;
        memcpy(n->mem + 1, pkt->pfx->nameptr, pkt->pfx->namelen);
        k = kh_get(PFX, NMmap, n);
        if (k != kh_end(NMmap)) {
            DEBUGMSG(DEBUG, "  found NMmap entry at position %d\n", k);
            digest = kh_val(NMmap, k) + 1;
        }
        ccnl_free(n);
    }

    if (digest) {
        char *path;
        struct stat s;
        int f;
        unsigned char *key;

        path = digest2fname(theDirPath, digest);
        if (stat(path, &s)) {
            int absent = 0;
//            perror("stat");
            free(path);
// badFile:
            DEBUGMSG(DEBUG, "  NOset += %s/%s\n", digest2str(digest),
                     ccnl_suite2str(suite));
            key = digest2key(suite, digest);
            k = kh_put(256, NOset, key, &absent);
            if (absent) {
                unsigned char *key2 = ccnl_malloc(SHA256_DIGEST_LENGTH+1);
                memcpy(key2, key, SHA256_DIGEST_LENGTH+1);
                kh_key(NOset, k) = key2;
            }
            goto Done;
        }
        f = open(path, O_RDONLY);
        free(path);
        if (f < 0) {
            int absent = 0;
//            perror("open");
badContent:
            DEBUGMSG(DEBUG, "  ERset += %s/%s\n", digest2str(digest),
                     ccnl_suite2str(suite));
            key = digest2key(suite, digest);
            k = kh_put(256, ERset, key, &absent);
            if (absent) {
                unsigned char *key2 = ccnl_malloc(SHA256_DIGEST_LENGTH+1);
                memcpy(key2, key, SHA256_DIGEST_LENGTH+1);
                kh_key(ERset, k) = key2;
            }
            goto Done;
        }
        struct ccnl_buf_s *buf = ccnl_buf_new(NULL, s.st_size);
        int dlen;
        buf->datalen = s.st_size;
        dlen = read(f, buf->data, buf->datalen);
        close(f);

        if (repo_mode == MODE_FILE) {
            free_packet(pkt);
            switch(suite) {
            case CCNL_SUITE_CCNTLV: {
                int hdrlen;
                unsigned char *data2;

                data2 = start = buf->data;

                hdrlen = ccnl_ccntlv_getHdrLen(data2, dlen);
                data2 += hdrlen;
                dlen -= hdrlen;

                pkt = ccnl_ccntlv_bytes2pkt(start, &data2, &dlen);
                break;
            }
            case CCNL_SUITE_NDNTLV: {
                unsigned char *data2;

                data2 = start = buf->data;
                pkt = ccnl_ndntlv_bytes2pkt(start, &data2, &dlen);
                break;
            }
            default:
                pkt = NULL;
                break;
            }
            if (!pkt || memcmp(pkt->md, digest, SHA256_DIGEST_LENGTH)) {
                ccnl_free(buf);
                goto badContent;
            }
        }
        ccnl_face_enqueue(repo, from, buf);
    }
    rc = 0;

Done:
    free_packet(pkt);
    return rc;
}

void
ccnl_repo_RX(struct ccnl_relay_s *repo, int ifndx, unsigned char *data,
             int datalen, struct sockaddr *sa, int addrlen)
{
    unsigned char *base = data;
    struct ccnl_face_s *from;
    int enc, suite = -1, skip;

    (void) base; // silence compiler warning (if USE_DEBUG is not set)

    DEBUGMSG(DEBUG, "ccnl_repo_RX ifndx=%d, %d bytes\n", ifndx, datalen);
    //    DEBUGMSG_ON(DEBUG, "ccnl_core_RX ifndx=%d, %d bytes\n", ifndx, datalen);

#ifdef USE_STATS
    if (ifndx >= 0)
        repo->ifs[ifndx].rx_cnt++;
#endif

    from = ccnl_get_face_or_create(repo, ifndx, sa, addrlen);
    if (!from) {
        DEBUGMSG(DEBUG, "  no face\n");
        return;
    } else {
        DEBUGMSG(DEBUG, "  face %d, peer=%s\n", from->faceid,
                    ccnl_addr2ascii(&from->peer));
    }

    // loop through all packets in the received frame (UDP, Ethernet etc)
    while (datalen > 0) {
        // work through explicit code switching
        while (!ccnl_switch_dehead(&data, &datalen, &enc))
            suite = ccnl_enc2suite(enc);
        if (suite == -1)
            suite = ccnl_pkt2suite(data, datalen, &skip);

        if (!ccnl_isSuite(suite)) {
            DEBUGMSG(WARNING, "?unknown packet format? ccnl_core_RX ifndx=%d, %d bytes starting with 0x%02x at offset %zd\n",
                     ifndx, datalen, *data, data - base);
            return;
        } else if (ccnl_repo256(repo, from, suite, skip, &data, &datalen) < 0)
            break;
        if (datalen > 0) {
            DEBUGMSG(WARNING, "ccnl_core_RX: %d bytes left\n", datalen);
        }
    }
}

int
ccnl_io_loop(struct ccnl_relay_s *ccnl)
{
    int i, len, maxfd = -1, rc;
    fd_set readfs, writefs;
    unsigned char buf[CCNL_MAX_PACKET_SIZE];

    if (ccnl->ifcount == 0) {
        DEBUGMSG(ERROR, "no socket to work with, not good, quitting\n");
        exit(EXIT_FAILURE);
    }
    for (i = 0; i < ccnl->ifcount; i++)
        if (ccnl->ifs[i].sock > maxfd)
            maxfd = ccnl->ifs[i].sock;
    maxfd++;

    DEBUGMSG(INFO, "starting main event and IO loop\n");
    while (!ccnl->halt_flag) {
        int usec;

        FD_ZERO(&readfs);
        FD_ZERO(&writefs);

        for (i = 0; i < ccnl->ifcount; i++) {
            FD_SET(ccnl->ifs[i].sock, &readfs);
            if (ccnl->ifs[i].qlen > 0)
                FD_SET(ccnl->ifs[i].sock, &writefs);
        }

        usec = ccnl_run_events();
        if (usec >= 0) {
            struct timeval deadline;
            deadline.tv_sec = usec / 1000000;
            deadline.tv_usec = usec % 1000000;
            rc = select(maxfd, &readfs, &writefs, NULL, &deadline);
        } else
            rc = select(maxfd, &readfs, &writefs, NULL, NULL);

        if (rc < 0) {
            perror("select(): ");
            exit(EXIT_FAILURE);
        }

        for (i = 0; i < ccnl->ifcount; i++) {
            if (FD_ISSET(ccnl->ifs[i].sock, &readfs)) {
                sockunion src_addr;
                socklen_t addrlen = sizeof(sockunion);
                if ((len = recvfrom(ccnl->ifs[i].sock, buf, sizeof(buf), 0,
                                (struct sockaddr*) &src_addr, &addrlen)) > 0) {
                    if (0) {}
#ifdef USE_IPV4
                    else if (src_addr.sa.sa_family == AF_INET) {
                        ccnl_repo_RX(ccnl, i, buf, len,
                                     &src_addr.sa, sizeof(src_addr.ip4));
                    }
#endif
#ifdef USE_ETHERNET
                    else if (src_addr.sa.sa_family == AF_PACKET) {
                        if (len > 14)
                            ccnl_repo_RX(ccnl, i, buf+14, len-14,
                                         &src_addr.sa, sizeof(src_addr.eth));
                    }
#endif
#ifdef USE_UNIXSOCKET
                    else if (src_addr.sa.sa_family == AF_UNIX) {
                        ccnl_repo_RX(ccnl, i, buf, len,
                                     &src_addr.sa, sizeof(src_addr.ux));
                    }
#endif
                }
            }

            if (FD_ISSET(ccnl->ifs[i].sock, &writefs)) {
              ccnl_interface_CTS(ccnl, ccnl->ifs + i);
            }
        }
    }

    return 0;
}

// ----------------------------------------------------------------------

struct ccnl_pkt_s*
contentFile2packet(char *path, int *suite)
{
    int datalen, skip;
    struct ccnl_pkt_s *pkt = NULL;
    unsigned char *data;

    DEBUGMSG(DEBUG, "loading %s\n", path);

    datalen = file2iobuf(path);
    if (datalen <= 0)
        return NULL;

    *suite = ccnl_pkt2suite(iobuf, datalen, &skip);
    switch (*suite) {
#ifdef USE_SUITE_CCNB
    case CCNL_SUITE_CCNB: {
        unsigned char *start;

        data = start = iobuf + skip;
        datalen -= skip;

        if (data[0] != 0x04 || data[1] != 0x82)
            goto notacontent;
        data += 2;
        datalen -= 2;

        pkt = ccnl_ccnb_bytes2pkt(start, &data, &datalen);
        break;
    }
#endif
#ifdef USE_SUITE_CCNTLV
    case CCNL_SUITE_CCNTLV: {
        int hdrlen;
        unsigned char *start;

        data = start = iobuf + skip;
        datalen -=  skip;

        hdrlen = ccnl_ccntlv_getHdrLen(data, datalen);
        data += hdrlen;
        datalen -= hdrlen;

        pkt = ccnl_ccntlv_bytes2pkt(start, &data, &datalen);
        break;
    }
#endif
#ifdef USE_SUITE_CISTLV
    case CCNL_SUITE_CISTLV: {
        int hdrlen;
        unsigned char *start;

        data = start = iobuf + skip;
        datalen -=  skip;

        hdrlen = ccnl_cistlv_getHdrLen(data, datalen);
        data += hdrlen;
        datalen -= hdrlen;

        pkt = ccnl_cistlv_bytes2pkt(start, &data, &datalen);
        break;
    }
#endif
#ifdef USE_SUITE_IOTTLV
    case CCNL_SUITE_IOTTLV: {
        unsigned char *olddata;

        data = olddata = iobuf + skip;
        datalen -= skip;
        if (ccnl_iottlv_dehead(&data, &datalen, &typ, &len) ||
                                                       typ != IOT_TLV_Reply)
            goto notacontent;
        pkt = ccnl_iottlv_bytes2pkt(typ, olddata, &data, &datalen);
        break;
    }
#endif
#ifdef USE_SUITE_NDNTLV
    case CCNL_SUITE_NDNTLV: {
        unsigned char *olddata;

        data = olddata = iobuf + skip;
        datalen -= skip;
        pkt = ccnl_ndntlv_bytes2pkt(olddata, &data, &datalen);
        break;
    }
#endif
    default:
        DEBUGMSG(WARNING, "unknown packet format (%s)\n", path);
        break;
    }

    if (!pkt) {
        DEBUGMSG(DEBUG, "  parsing error in %s\n", path);
        return NULL;
    }

    return pkt;
}

// ----------------------------------------------------------------------

void
add_content(char *dirpath, char *path)
{
    int _suite;
    struct ccnl_pkt_s *pkt = NULL;

    DEBUGMSG(DEBUG, "add_content %s %s\n", dirpath, path);

    pkt = contentFile2packet(path, &_suite);
    if (!pkt)
        return;

    char *path2;
    path2 = digest2fname(dirpath, pkt->md);
    if (repo_mode == MODE_INDEX && strcmp(path2, path)) {
        DEBUGMSG(WARNING, "wrong digest for file <%s>, ignored\n", path);
//                DEBUGMSG(WARNING, "                      %s\n", path2);
        free(path2);
        return;
    }

    unsigned char *key = digest2key(_suite, pkt->md);
    int absent = 0;
    khint_t k = kh_put(256, OKset, key, &absent);

    if (absent) {
        unsigned char *key2 = ccnl_malloc(SHA256_DIGEST_LENGTH+1);
        memcpy(key2, key, SHA256_DIGEST_LENGTH+1);
        kh_key(OKset, k) = key2;
    }
    key = kh_key(OKset, k);

    if (pkt->pfx) {
        khPFX_t n;
        DEBUGMSG(DEBUG, "pkt has name [%s]%s, %p/%d\n",
                 ccnl_prefix2path(prefixBuf,
                                  CCNL_ARRAY_SIZE(prefixBuf),
                                  pkt->pfx),
                 ccnl_suite2str(pkt->pfx->suite),
                 pkt->pfx->nameptr, (int)(pkt->pfx->namelen));
        DEBUGMSG(DEBUG, "adding name [%s]%s -->\n",
                 ccnl_prefix2path(prefixBuf,
                                  CCNL_ARRAY_SIZE(prefixBuf),
                                  pkt->pfx),
                 ccnl_suite2str(pkt->pfx->suite));
        DEBUGMSG(DEBUG, "  %s\n", digest2str(pkt->md));

        n = ccnl_malloc(sizeof(struct khPFX_s) + pkt->pfx->namelen);
        n->len = 1 + pkt->pfx->namelen;
        n->mem[0] = pkt->pfx->suite;
        memcpy(n->mem + 1, pkt->pfx->nameptr, pkt->pfx->namelen);
        absent = 0;
        k = kh_put(PFX, NMmap, n, &absent);
        if (absent) {
            kh_key(NMmap, k) = n;
            kh_val(NMmap, k) = key;
        } else {
            DEBUGMSG(WARNING, "name %s already scanned, file %s ommited\n",
                     ccnl_prefix2path(prefixBuf,
                                      CCNL_ARRAY_SIZE(prefixBuf), pkt->pfx),
                     path2);
            ccnl_free(n);
        }
    }
    free(path2);
    free_packet(pkt);
}

void
walk_fs(char *dirpath, char *path)
{
    DIR *dp;
    struct dirent *de;

    dp = opendir(path);
    if (!dp)
        return;
    while ((de = readdir(dp))) {
        char *path2;
        asprintf(&path2, "%s/%s", path, de->d_name);
        switch(de->d_type) {
        case DT_REG:
            add_content(dirpath, path2);
            break;
        case DT_LNK:
            if (repo_mode == MODE_FILE)
                add_content(dirpath, path2);
            break;
        case DT_DIR:
            if (de->d_name[0] != '.')
                walk_fs(dirpath, path2);
            break;
        default:
            break;
        }
        free(path2);
    }
    closedir(dp);
}

// ----------------------------------------------------------------------

int
ccnl_repo256_import(char *dir)
{
    DIR *dp;
    struct dirent *de;

    dp = opendir(dir);
    if (!dp)
        return 0;
    while ((de = readdir(dp))) {
        char *walk, *hashName, *linkContent;
        int dummy;
        struct ccnl_pkt_s *pkt;

        asprintf(&walk, "%s/%s", dir, de->d_name);
        switch(de->d_type) {
        case DT_LNK:
        case DT_REG:
            pkt = contentFile2packet(walk, &dummy);
            if (!pkt) {
                DEBUGMSG(DEBUG, "  no packet?\n");
                break;
            }
            hashName = digest2fname(theDirPath, pkt->md);
            char *hex = digest2str(pkt->md);
            if (access(hashName, F_OK)) { // no such file, create it
                int f;
                DEBUGMSG(DEBUG, "  creating %s\n", hashName);
                assertDir(theDirPath, hex);
                f = open(hashName, O_CREAT | O_TRUNC | O_WRONLY, 0666);
                write(f, pkt->buf->data, pkt->buf->datalen);
                close(f);
            }
            free(hashName);
            if (pkt->pfx) { // has name: add a symlink to the zz directory
                asprintf(&hashName, "%s/zz/%s", theDirPath, hex);
                if (access(hashName, F_OK)) { // no such file/link, create it
                    assertDir(theDirPath, "zz");
                    asprintf(&linkContent, "../%c%c/%s", hex[0], hex[1], hex+2);
                    symlink(linkContent, hashName);
                    free(linkContent);
                } else {
                    DEBUGMSG(INFO, "%s already exists, ignored\n", hashName);
                }
                free(hashName);
            }
            free_packet(pkt);
            break;
        case DT_DIR:
            if (de->d_name[0] != '.')
                ccnl_repo256_import(walk);
            break;
        default:
            break;
        }
        free(walk);
    }
    closedir(dp);

    return 0;
}

// ----------------------------------------------------------------------

int
main(int argc, char *argv[])
{
    int opt, max_cache_entries = 0, udpport = 7777;
    char *ethdev = NULL, *uxpath = NULL, *import_path = NULL;

    while ((opt = getopt(argc, argv, "hc:d:e:i:m:u:v:x:")) != -1) {
        switch (opt) {
        case 'c':
            max_cache_entries = atoi(optarg);
            break;
        case 'e':
            ethdev = optarg;
            break;
        case 'i':
            import_path = optarg;
            break;
        case 'm':
            repo_mode = !strcmp(optarg, "file") ? MODE_FILE : MODE_INDEX;
            break;
        case 'u':
            udpport = atoi(optarg);
            break;
        case 'v':
#ifdef USE_LOGGING
            if (isdigit(optarg[0]))
                debug_level = atoi(optarg);
            else
                debug_level = ccnl_debug_str2level(optarg);
#endif
            break;
        case 'x':
            uxpath = optarg;
            break;
        case 'h':
        default:
usage:
            fprintf(stderr,
                    "usage: %s [options]  REPO_DIR                (server)\n"
                    "       %s -i IMPORT_DIR [options] REPO_DIR   (importing)\n"
                    "options:\n"
//                  "  -c MAX_CONTENT_ENTRIES  (dflt: 0)\n"
                    "  -e ETHDEV\n"
                    "  -h              this text\n"
                    "  -m MODE         ('file'=read through, 'ndx'=internal index (dflt)\n"
#ifdef USE_IPV4
                    "  -u UDPPORT      (default: 7777)\n"
#endif

#ifdef USE_LOGGING
                    "  -v DEBUG_LEVEL  (fatal, error, warning, info, debug, verbose, trace)\n"
#endif
#ifdef USE_UNIXSOCKET
                    "  -x UNIXPATH\n"
#endif
                    , argv[0], argv[0]);
            exit(-1);
        }
    }

    if (optind >= argc)
        goto usage;
    theDirPath = argv[optind++];
    if (optind != argc)
        goto usage;
    while (strlen(theDirPath) > 1 && theDirPath[strlen(theDirPath) - 1] == '/')
        theDirPath[strlen(theDirPath) - 1] = '\0';

    if (import_path)
        return ccnl_repo256_import(import_path);

    ccnl_core_init();

    DEBUGMSG(INFO, "This is ccn-lite-repo256, starting at %s",
             ctime(&theRepo.startup_time) + 4);
    DEBUGMSG(INFO, "  ccnl-core: %s\n", CCNL_VERSION);
    DEBUGMSG(INFO, "  compile time: %s %s\n", __DATE__, __TIME__);
    DEBUGMSG(INFO, "  compile options: %s\n", compile_string);

    ccnl_repo256_config(&theRepo, ethdev, udpport, uxpath, max_cache_entries);

    if (repo_mode == MODE_FILE) {
        ERset = kh_init(256);  // set of hashes for which the file is wrong
        NOset = kh_init(256);  // set of hashes known to be absent
    }
    OKset = kh_init(256);  // set of verified hashes (from files)
    NMmap = kh_init(PFX);  // map of verified names (from files)

    if (repo_mode == MODE_INDEX) {
        DEBUGMSG(INFO, "loading files from <%s>\n", theDirPath);
        walk_fs(theDirPath, theDirPath);
    } else {
        char *fname;
        asprintf(&fname, "%s/zz", theDirPath);
        DEBUGMSG(INFO, "loading files from <%s>\n", fname);
        walk_fs(theDirPath, fname);
        free(fname);
    }
    DEBUGMSG(INFO, "loaded %d files (%d with name, %d without name)\n",
             kh_size(OKset), kh_size(NMmap), kh_size(OKset)-kh_size(NMmap));

    DEBUGMSG(DEBUG, "allocated memory: total %ld bytes in %d chunks\n",
             ccnl_total_alloc_bytes, ccnl_total_alloc_chunks);

    ccnl_io_loop(&theRepo);

/*
    {
        khiter_t k;

        for (k = kh_begin(OKset); k != kh_end(OKset); k++)
            if (kh_exist(OKset, k))
                ccnl_free(kh_key(OKset, k);

        for (k = kh_begin(NMmap); k != kh_end(NMmap); k++)
            if (kh_exist(NMmap, k))
                ccnl_free(kh_key(NMmap, k);
    }
*/
}

// eof
