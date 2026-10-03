/*
 * cluster_video_ring - the phone's cluster stream, from the hook in dio_manager to the
 * renderer that shows it (maneuver_render, window 99).
 *
 * One POSIX shared-memory object: a header, then a byte ring of records.  The hook is its only
 * writer and never waits: it overwrites the oldest records, so a slow or absent reader can
 * never hold dio_manager up.  A reader validates each record after copying it, seqlock style:
 * before writing, the writer publishes how far it is about to write (`reserve`), after, how far
 * it has written (`head`).
 *
 * A record is a whole access unit in Annex B: CVR_CONFIG for the parameter sets, CVR_FRAME for
 * a frame, with CVR_KEY when it holds an IDR slice.  The header remembers the current stream's
 * newest config and key frame, so a reader that joins late or falls behind resumes at the
 * config and then the newest key frame - never replaying a backlog (map15).  When the ring has
 * no decodable point left, the reader asks for a key frame (`keyframe_requests`), which the
 * hook turns into the phone's forceKeyFrame.
 *
 * Positions are byte counts modulo 2^32; a record never wraps (a CVR_PAD record, or a tail
 * shorter than a record header, fills the end).  Shared fields are 32 bits, ordered by full
 * hardware barriers.  Header-only: hook, renderer and host tests build it from one definition.
 */
#ifndef CLUSTER_VIDEO_RING_H
#define CLUSTER_VIDEO_RING_H

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef CVR_SHM_NAME
#define CVR_SHM_NAME     "/cr_cluster_video"    /* not carplay_*: the log collector copies those from /tmp */
#endif
#define CVR_MAGIC        0x32525643u            /* "CVR2": layout version included */
#ifndef CVR_DATA_BYTES                          /* a power of two; smaller only in host tests */
#define CVR_DATA_BYTES   (1u << 20)             /* about 4 s of the cluster map (map11: frames <= 43 KB) */
#endif
#define CVR_RECORD_MAX   (CVR_DATA_BYTES / 4)   /* the largest record, its header included */
#define CVR_KEY_WAIT     60                     /* frames passed over waiting for a key frame before asking */

#define CVR_CONFIG       0x0001u
#define CVR_FRAME        0x0002u
#define CVR_KEY          0x0004u
#define CVR_PAD          0x8000u

#define CVR_HAVE_CONFIG  0x0001u                /* cvr_header_t.have: config_pos / key_pos are valid */
#define CVR_HAVE_KEY     0x0002u

typedef struct {
    uint32_t bytes;               /* payload after this header */
    uint32_t flags;
    uint32_t session;             /* the stream connection it came on */
    uint32_t seq;                 /* per session, from 0 */
    uint64_t time_ns;             /* CLOCK_MONOTONIC when written */
} cvr_record_t;                   /* 24 bytes; records start 8-byte aligned */

typedef struct {
    uint32_t magic;               /* CVR_MAGIC once a writer has set the ring up */
    uint32_t data_bytes;
    uint32_t epoch;               /* new whenever a writer sets the ring up */
    uint32_t session;             /* the current stream; 0 before the first */
    uint32_t reserve, head;
    uint32_t config_pos, key_pos, have;
    uint32_t keyframe_requests;   /* readers add one to ask for a key frame */
    uint32_t spare[22];
} cvr_header_t;                   /* 128 bytes */

/* A full hardware barrier.  QNX 6.5's GCC 4.4 has no barrier pattern for ARM (its
 * __sync_synchronize() only stops the compiler), so on ARM it is ARMv7's DMB SY, spelled as
 * its encoding because that compiler runs without -march. */
#if defined(__arm__) && defined(__thumb2__)
#define CVR_FENCE()      __asm__ __volatile__(".hword 0xf3bf, 0x8f5f" : : : "memory")
#elif defined(__arm__) && defined(__thumb__)
#error "cluster_video_ring.h: Thumb-1 has no DMB; build in ARM state"
#elif defined(__arm__)
#define CVR_FENCE()      __asm__ __volatile__(".word 0xf57ff05f" : : : "memory")
#else
#define CVR_FENCE()      __sync_synchronize()
#endif

/* A load nothing after it moves above; a store nothing before it moves below. */
static inline uint32_t cvr_load(const uint32_t *p) {
    uint32_t v = *(const volatile uint32_t *)p;
    CVR_FENCE();
    return v;
}
static inline void cvr_store(uint32_t *p, uint32_t v) {
    CVR_FENCE();
    *(volatile uint32_t *)p = v;
}

static inline uint32_t cvr_align(uint32_t n) { return (n + 7u) & ~7u; }
static inline size_t cvr_shm_bytes(void) { return sizeof(cvr_header_t) + CVR_DATA_BYTES; }
static inline uint8_t *cvr_data(cvr_header_t *h) { return (uint8_t *)h + sizeof(cvr_header_t); }

/* The ring mapped read-write: `create` opens or creates it (the writer).  NULL with errno. */
static inline cvr_header_t *cvr_map(int create) {
    struct stat st;
    void *p = MAP_FAILED;
    int e, fd = shm_open(CVR_SHM_NAME, create ? O_RDWR | O_CREAT : O_RDWR, 0600);
    if (fd < 0) return NULL;
    if ((!create || ftruncate(fd, (off_t)cvr_shm_bytes()) == 0) && fstat(fd, &st) == 0) {
        if ((size_t)st.st_size >= cvr_shm_bytes())
            p = mmap(NULL, cvr_shm_bytes(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        else
            errno = ENODATA;
    }
    e = errno;
    close(fd);
    errno = e;
    return p == MAP_FAILED ? NULL : (cvr_header_t *)p;
}

static inline void cvr_unmap(cvr_header_t *h) {
    if (h) munmap((void *)h, cvr_shm_bytes());
}

/* ---- Writer (the hook's receiver thread) ---- */

typedef struct {
    cvr_header_t *h;
    uint32_t pos;                 /* == head */
    uint32_t session, seq, have, requests_seen;
} cvr_writer_t;

/* Sets the ring up afresh: every reader starts over.  `seed` makes the first epoch differ
 * between boots.  0, or -1 with errno set. */
static inline int cvr_writer_open(cvr_writer_t *w, uint32_t seed) {
    cvr_header_t *h = cvr_map(1);
    uint32_t epoch;
    memset(w, 0, sizeof(*w));
    if (!h) return -1;
    epoch = cvr_load(&h->magic) == CVR_MAGIC ? cvr_load(&h->epoch) + 1u : seed;
    cvr_store(&h->magic, 0u);
    h->data_bytes = CVR_DATA_BYTES;
    cvr_store(&h->session, 0u);
    cvr_store(&h->have, 0u);
    cvr_store(&h->reserve, 0u);
    cvr_store(&h->head, 0u);
    cvr_store(&h->epoch, epoch ? epoch : 1u);
    w->requests_seen = cvr_load(&h->keyframe_requests);
    cvr_store(&h->magic, CVR_MAGIC);
    w->h = h;
    return 0;
}

static inline void cvr_writer_close(cvr_writer_t *w) {
    cvr_unmap(w->h);
    w->h = NULL;
}

/* A new stream connection: readers drop what they have and wait for its config. */
static inline uint32_t cvr_writer_new_session(cvr_writer_t *w) {
    w->have = 0;
    w->seq = 0;
    if (++w->session == 0) w->session = 1;
    cvr_store(&w->h->have, 0u);
    cvr_store(&w->h->session, w->session);
    return w->session;
}

/* Room for a `bytes`-byte payload, contiguous, its record starting at *start; NULL when such
 * a record can never fit.  Every non-NULL return is followed by cvr_writer_commit. */
static inline uint8_t *cvr_writer_begin(cvr_writer_t *w, uint32_t bytes, uint32_t *start) {
    cvr_header_t *h = w->h;
    uint32_t total, offset, room;
    if (bytes > CVR_RECORD_MAX - (uint32_t)sizeof(cvr_record_t)) return NULL;
    total = cvr_align((uint32_t)sizeof(cvr_record_t) + bytes);
    offset = w->pos & (CVR_DATA_BYTES - 1u);
    room = CVR_DATA_BYTES - offset;
    if (room < total) {                         /* readers skip the tail; the record starts the ring */
        cvr_store(&h->reserve, w->pos + room + total);
        CVR_FENCE();
        if (room >= sizeof(cvr_record_t)) {
            cvr_record_t pad;
            memset(&pad, 0, sizeof(pad));
            pad.bytes = room - (uint32_t)sizeof(cvr_record_t);
            pad.flags = CVR_PAD;
            pad.session = w->session;
            memcpy(cvr_data(h) + offset, &pad, sizeof(pad));
        }
        w->pos += room;
        offset = 0;
    } else {
        cvr_store(&h->reserve, w->pos + total);
        CVR_FENCE();
    }
    *start = w->pos;
    return cvr_data(h) + offset + sizeof(cvr_record_t);
}

static inline void cvr_writer_commit(cvr_writer_t *w, uint32_t start, uint32_t bytes, uint32_t flags, uint64_t time_ns) {
    cvr_header_t *h = w->h;
    cvr_record_t r;
    r.bytes = bytes;
    r.flags = flags;
    r.session = w->session;
    r.seq = w->seq++;
    r.time_ns = time_ns;
    memcpy(cvr_data(h) + (start & (CVR_DATA_BYTES - 1u)), &r, sizeof(r));
    w->pos = start + cvr_align((uint32_t)sizeof(r) + bytes);
    cvr_store(&h->head, w->pos);
    if (flags & CVR_CONFIG) {
        cvr_store(&h->config_pos, start);
        w->have |= CVR_HAVE_CONFIG;
    }
    if (flags & CVR_KEY) {
        cvr_store(&h->key_pos, start);
        w->have |= CVR_HAVE_KEY;
    }
    cvr_store(&h->have, w->have);
}

/* Key frames readers asked for since the last call. */
static inline uint32_t cvr_writer_requests(cvr_writer_t *w) {
    uint32_t now = cvr_load(&w->h->keyframe_requests), n = now - w->requests_seen;
    w->requests_seen = now;
    return n;
}

/* ---- Reader ---- */

enum { CVR_NEED_CONFIG, CVR_AT_CONFIG, CVR_NEED_KEY, CVR_STREAMING };

typedef struct {
    cvr_header_t *h;
    uint32_t epoch, session, pos;
    int state;
    int wants_key;                /* no decodable point in the ring: ask for a key frame */
    uint32_t waited;              /* frames passed over since the config, waiting for a key frame */
    uint32_t lost;                /* times records were overwritten before being read */
} cvr_reader_t;

static inline int cvr_reader_attach(cvr_reader_t *r) {
    memset(r, 0, sizeof(*r));
    r->h = cvr_map(0);
    return r->h ? 0 : -1;
}

static inline void cvr_reader_detach(cvr_reader_t *r) {
    cvr_unmap(r->h);
    r->h = NULL;
}

/* Bytes from `pos` the writer may already be overwriting. */
static inline int cvr_reader_overwritten(const cvr_reader_t *r, uint32_t pos) {
    return cvr_load(&r->h->reserve) - pos > CVR_DATA_BYTES;
}

/* Starts over at the stream's newest config, then its newest key frame: a decoder opening
 * late, or one that missed a unit. */
static inline void cvr_reader_restart(cvr_reader_t *r) {
    r->state = CVR_NEED_CONFIG;
}

/* The renderer's side: __sync_fetch_and_add, as its QNX build already links (main.c). */
static inline void cvr_reader_request_key(cvr_reader_t *r) {
    __sync_fetch_and_add(&r->h->keyframe_requests, 1u);
    r->wants_key = 0;
}

/* The next record a decoder needs: 1 with its header in *rec and its payload in
 * out[0..rec->bytes); 0 when there is none yet.  A new writer, a new stream or a lost record
 * starts over at the stream's newest config, then its newest key frame; other frames before a
 * key frame are passed over, as are payloads bigger than `cap`. */
static inline int cvr_reader_next(cvr_reader_t *r, cvr_record_t *rec, uint8_t *out, uint32_t cap) {
    cvr_header_t *h = r->h;
    const uint8_t *data;
    uint32_t epoch, session;
    int guard;
    if (!h || cvr_load(&h->magic) != CVR_MAGIC || h->data_bytes != CVR_DATA_BYTES) return 0;
    data = cvr_data(h);
    epoch = cvr_load(&h->epoch);
    session = cvr_load(&h->session);
    if (epoch != r->epoch || session != r->session) {
        r->epoch = epoch;
        r->session = session;
        r->state = CVR_NEED_CONFIG;
        r->wants_key = 0;
    }
    if (!session) return 0;
    for (guard = 0; guard < 256; ++guard) {
        uint32_t head, offset, room, total, have = cvr_load(&h->have);
        if (r->state == CVR_NEED_CONFIG) {
            uint32_t config = cvr_load(&h->config_pos);
            if (!(have & CVR_HAVE_CONFIG) || cvr_reader_overwritten(r, config)) {
                if (have & CVR_HAVE_CONFIG) r->wants_key = 1;     /* gone: a key frame brings both */
                r->pos = cvr_load(&h->head);
                return 0;
            }
            r->pos = config;
            r->state = CVR_AT_CONFIG;
            r->waited = 0;
        }
        head = cvr_load(&h->head);
        if (r->state == CVR_NEED_KEY) {
            /* A newer config starts the reader over there; else it goes to the newest key frame. */
            uint32_t config = cvr_load(&h->config_pos), key = cvr_load(&h->key_pos);
            if (config - r->pos < head - r->pos) {
                r->state = CVR_NEED_CONFIG;
                continue;
            }
            if ((have & CVR_HAVE_KEY) && key - r->pos < head - r->pos && !cvr_reader_overwritten(r, key)) r->pos = key;
        }
        if (r->pos == head) return 0;
        if (head - r->pos > CVR_DATA_BYTES || cvr_reader_overwritten(r, r->pos)) {
            ++r->lost;
            r->state = CVR_NEED_CONFIG;
            continue;
        }
        offset = r->pos & (CVR_DATA_BYTES - 1u);
        room = CVR_DATA_BYTES - offset;
        if (room < sizeof(cvr_record_t)) {
            r->pos += room;
            continue;
        }
        memcpy(rec, data + offset, sizeof(*rec));
        CVR_FENCE();
        if (cvr_reader_overwritten(r, r->pos)) {
            ++r->lost;
            r->state = CVR_NEED_CONFIG;
            continue;
        }
        if (rec->flags & CVR_PAD) {
            r->pos += room;
            continue;
        }
        total = cvr_align((uint32_t)sizeof(*rec) + rec->bytes);
        if (rec->bytes > CVR_RECORD_MAX || total > room || head - r->pos < total || rec->session != r->session) {
            ++r->lost;                                      /* not a record this stream published */
            r->state = CVR_NEED_CONFIG;
            continue;
        }
        if (rec->bytes > cap || (r->state != CVR_STREAMING && (rec->flags & CVR_FRAME) && !(rec->flags & CVR_KEY))) {
            r->pos += total;
            if (r->state == CVR_STREAMING) r->state = CVR_NEED_KEY;
            if (++r->waited == CVR_KEY_WAIT) r->wants_key = 1;
            continue;
        }
        memcpy(out, data + offset + sizeof(*rec), rec->bytes);
        CVR_FENCE();
        if (cvr_reader_overwritten(r, r->pos)) {
            ++r->lost;
            r->state = CVR_NEED_CONFIG;
            continue;
        }
        r->pos += total;
        if ((rec->flags & CVR_CONFIG) && r->state == CVR_AT_CONFIG) r->state = CVR_NEED_KEY;
        if (rec->flags & CVR_KEY) {
            r->state = CVR_STREAMING;
            r->wants_key = 0;
            r->waited = 0;
        }
        return 1;
    }
    return 0;
}

#endif /* CLUSTER_VIDEO_RING_H */
