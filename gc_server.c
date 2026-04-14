/*
 * gc_server.c  —  GC Visualizer Backend
 * REST API serving GC state as JSON over HTTP.
 *
 * Compile:  gcc -o gc_server gc_server.c
 * Run:      ./gc_server          (listens on http://localhost:8080)
 *
 * Endpoints:
 *   GET  /health
 *   GET  /state/rc  |  /state/ms  |  /state/gen
 *   POST /rc/alloc  |  /rc/addref  |  /rc/dropref  |  /rc/cycle  |  /rc/reset
 *   POST /ms/alloc  |  /ms/connect |  /ms/disconnect |  /ms/gc/step |  /ms/reset
 *   POST /gen/alloc |  /gen/minor  |  /gen/major    |  /gen/reset
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdarg.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")
#define close_socket closesocket
#define write_socket(fd, buf, len) send((fd), (buf), (int)(len), 0)
#define read_socket(fd, buf, len) recv((fd), (buf), (int)(len), 0)
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#define close_socket close
#define write_socket(fd, buf, len) write((fd), (buf), (len))
#define read_socket(fd, buf, len) read((fd), (buf), (len))
#endif

/* ═══════════════════════════════════════════════════════════════════════
   CONFIGURATION
   ═══════════════════════════════════════════════════════════════════════ */
#define PORT          8080
#define BUF_SIZE      8192
#define JSON_CAP      32768

/* ═══════════════════════════════════════════════════════════════════════
   OS PAYLOAD & STATS
   ═══════════════════════════════════════════════════════════════════════ */
#define PAYLOAD_SIZE (5 * 1024 * 1024)

static void init_payload(void **p) {
    *p = malloc(PAYLOAD_SIZE);
    if (*p) memset(*p, 1, PAYLOAD_SIZE);
}
static void free_payload(void **p) {
    if (*p) { free(*p); *p = NULL; }
}

static void os_stats_json(char *out, int outsz) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        snprintf(out, outsz, "\"os\":{\"rss_mb\":%llu,\"faults\":%llu}",
            (unsigned long long)(pmc.WorkingSetSize / (1024 * 1024)),
            (unsigned long long)pmc.PageFaultCount);
    } else {
        snprintf(out, outsz, "\"os\":{\"rss_mb\":0,\"faults\":0}");
    }
#else
    snprintf(out, outsz, "\"os\":{\"rss_mb\":0,\"faults\":0}");
#endif
}

/* ═══════════════════════════════════════════════════════════════════════
   LOG BUFFER
   ═══════════════════════════════════════════════════════════════════════ */
#define LOG_MAX  48
#define LOG_LEN  128

typedef struct {
    char entries[LOG_MAX][LOG_LEN];
    int  head;
    int  count;
} LogBuf;

static void lb_write(LogBuf *l, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l->entries[l->head], LOG_LEN, fmt, ap);
    va_end(ap);
    l->head  = (l->head + 1) % LOG_MAX;
    if (l->count < LOG_MAX) l->count++;
}

static void lb_clear(LogBuf *l) { l->head = l->count = 0; }

/* Write last N entries into a JSON array string (oldest → newest). */
static int lb_to_json(const LogBuf *l, char *out, int outsz) {
    int pos = 0;
    pos += snprintf(out + pos, outsz - pos, "[");
    int n     = l->count < LOG_MAX ? l->count : LOG_MAX;
    int start = ((l->head - n) % LOG_MAX + LOG_MAX) % LOG_MAX;
    for (int i = 0; i < n; i++) {
        const char *s = l->entries[(start + i) % LOG_MAX];
        pos += snprintf(out + pos, outsz - pos, "\"");
        /* JSON-escape inline */
        for (; *s && pos < outsz - 4; s++) {
            if (*s == '"')       { out[pos++] = '\\'; out[pos++] = '"'; }
            else if (*s == '\\') { out[pos++] = '\\'; out[pos++] = '\\'; }
            else if (*s == '\n') { out[pos++] = '\\'; out[pos++] = 'n'; }
            else                   out[pos++] = *s;
        }
        pos += snprintf(out + pos, outsz - pos, "%s", i < n-1 ? "\"," : "\"");
    }
    pos += snprintf(out + pos, outsz - pos, "]");
    return pos;
}

/* ═══════════════════════════════════════════════════════════════════════
   1.  REFERENCE COUNTING ENGINE
   ═══════════════════════════════════════════════════════════════════════ */
#define RC_CAP      24
#define RC_MAX_REFS  8

typedef struct {
    int  id;
    int  alive;          /* 0 = slot is freed                    */
    int  rc;
    int  cycle;          /* 1 = flagged as part of a cycle       */
    int  dying;          /* 1 = rc just hit 0, about to free     */
    char name[16];
    int  refs[RC_MAX_REFS];
    int  ref_count;
    void *payload;
} RCObj;

static RCObj  rc_objs[RC_CAP];
static int    rc_obj_top  = 0;   /* next id to allocate   */
static int    rc_freed    = 0;
static int    rc_leaked   = 0;
static LogBuf rc_log;

static void rc_reset(void) {
    for (int i = 0; i < rc_obj_top; i++) free_payload(&rc_objs[i].payload);
    memset(rc_objs, 0, sizeof(rc_objs));
    rc_obj_top = rc_freed = rc_leaked = 0;
    lb_clear(&rc_log);
    /* seed with 3 objects */
    int ids[3];
    for (int i = 0; i < 3; i++) {
        int id = rc_obj_top++;
        ids[i] = id;
        rc_objs[id].id    = id;
        rc_objs[id].alive = 1;
        rc_objs[id].rc    = 0;
        init_payload(&rc_objs[id].payload);
        snprintf(rc_objs[id].name, 16, "obj%d", id);
        lb_write(&rc_log, "[ALLOC] %s rc=0", rc_objs[id].name);
    }
    (void)ids;
}

static void rc_do_alloc(void) {
    if (rc_obj_top >= RC_CAP) {
        lb_write(&rc_log, "[ERR] heap full"); return;
    }
    int id = rc_obj_top++;
    rc_objs[id].id    = id;
    rc_objs[id].alive = 1;
    rc_objs[id].rc    = 0;
    rc_objs[id].cycle = rc_objs[id].dying = 0;
    rc_objs[id].ref_count = 0;
    init_payload(&rc_objs[id].payload);
    snprintf(rc_objs[id].name, 16, "obj%d", id);
    lb_write(&rc_log, "[ALLOC] %s  rc=0", rc_objs[id].name);
}

static void rc_do_addref(void) {
    /* find two live objects, add an edge from→to if none exists */
    int live[RC_CAP], n = 0;
    for (int i = 0; i < rc_obj_top; i++)
        if (rc_objs[i].alive && !rc_objs[i].dying) live[n++] = i;
    if (n < 2) { lb_write(&rc_log, "[WARN] need >=2 live objects"); return; }
    /* pick first pair without an existing edge */
    for (int ai = 0; ai < n; ai++) {
        for (int bi = 0; bi < n; bi++) {
            if (ai == bi) continue;
            int a = live[ai], b = live[bi];
            int exists = 0;
            for (int r = 0; r < rc_objs[a].ref_count; r++)
                if (rc_objs[a].refs[r] == b) { exists = 1; break; }
            if (!exists && rc_objs[a].ref_count < RC_MAX_REFS) {
                rc_objs[a].refs[rc_objs[a].ref_count++] = b;
                rc_objs[b].rc++;
                lb_write(&rc_log, "[REF] %s→%s  %s.rc=%d",
                         rc_objs[a].name, rc_objs[b].name,
                         rc_objs[b].name, rc_objs[b].rc);
                return;
            }
        }
    }
    lb_write(&rc_log, "[WARN] no new ref possible");
}

/* Release all refs held by obj, decrement their rc, free if 0 */
static void rc_cascade_release(int id);
static void rc_cascade_release(int id) {
    RCObj *o = &rc_objs[id];
    for (int i = 0; i < o->ref_count; i++) {
        int tid = o->refs[i];
        if (!rc_objs[tid].alive) continue;
        rc_objs[tid].rc--;
        lb_write(&rc_log, "[RELEASE] %s.rc=%d", rc_objs[tid].name, rc_objs[tid].rc);
        if (rc_objs[tid].rc <= 0) {
            free_payload(&rc_objs[tid].payload);
            lb_write(&rc_log, "[FREE] %s", rc_objs[tid].name);
            rc_objs[tid].alive = 0;
            rc_freed++;
            rc_cascade_release(tid);
        }
    }
}

static void rc_do_dropref(void) {
    /* find an existing ref and drop it */
    for (int i = 0; i < rc_obj_top; i++) {
        if (!rc_objs[i].alive) continue;
        for (int r = 0; r < rc_objs[i].ref_count; r++) {
            int tid = rc_objs[i].refs[r];
            if (!rc_objs[tid].alive) continue;
            /* remove edge */
            rc_objs[i].refs[r] = rc_objs[i].refs[--rc_objs[i].ref_count];
            rc_objs[tid].rc--;
            lb_write(&rc_log, "[DROP] %s→%s  %s.rc=%d",
                     rc_objs[i].name, rc_objs[tid].name,
                     rc_objs[tid].name, rc_objs[tid].rc);
            if (rc_objs[tid].rc <= 0) {
                lb_write(&rc_log, "[FREE] %s", rc_objs[tid].name);
                rc_objs[tid].alive = 0;
                rc_freed++;
                rc_cascade_release(tid);
            }
            return;
        }
    }
    lb_write(&rc_log, "[WARN] no refs to drop");
}

static void rc_do_cycle(void) {
    /* ensure at least 2 live objects, create mutual refs */
    int live[RC_CAP], n = 0;
    for (int i = 0; i < rc_obj_top; i++)
        if (rc_objs[i].alive && !rc_objs[i].cycle) live[n++] = i;
    if (n < 2) {
        rc_do_alloc(); rc_do_alloc();
        n = 0;
        for (int i = 0; i < rc_obj_top; i++)
            if (rc_objs[i].alive && !rc_objs[i].cycle) live[n++] = i;
    }
    int a = live[0], b = live[1];
    /* A → B */
    rc_objs[a].refs[rc_objs[a].ref_count++] = b; rc_objs[b].rc++;
    /* B → A */
    rc_objs[b].refs[rc_objs[b].ref_count++] = a; rc_objs[a].rc++;
    rc_objs[a].cycle = rc_objs[b].cycle = 1;
    rc_leaked += 2;
    lb_write(&rc_log, "[CYCLE] %s↔%s  — will NEVER be freed!",
             rc_objs[a].name, rc_objs[b].name);
}

static void rc_state_json(char *out, int outsz) {
    int pos = 0;
    pos += snprintf(out+pos, outsz-pos, "{\"objects\":[");
    int first = 1;
    for (int i = 0; i < rc_obj_top; i++) {
        RCObj *o = &rc_objs[i];
        if (!first) pos += snprintf(out+pos, outsz-pos, ",");
        pos += snprintf(out+pos, outsz-pos,
            "{\"id\":%d,\"name\":\"%s\",\"rc\":%d,"
            "\"alive\":%d,\"cycle\":%d}",
            o->id, o->name, o->rc, o->alive, o->cycle);
        first = 0;
    }
    pos += snprintf(out+pos, outsz-pos, "],\"refs\":[");
    first = 1;
    for (int i = 0; i < rc_obj_top; i++) {
        if (!rc_objs[i].alive) continue;
        for (int r = 0; r < rc_objs[i].ref_count; r++) {
            if (!first) pos += snprintf(out+pos, outsz-pos, ",");
            pos += snprintf(out+pos, outsz-pos,
                "{\"from\":%d,\"to\":%d}", i, rc_objs[i].refs[r]);
            first = 0;
        }
    }
    char log_str[4096];
    lb_to_json(&rc_log, log_str, sizeof(log_str));
    char os_str[128];
    os_stats_json(os_str, sizeof(os_str));
    pos += snprintf(out+pos, outsz-pos,
        "],\"freed\":%d,\"leaked\":%d,\"log\":%s,%s}",
        rc_freed, rc_leaked, log_str, os_str);
}

/* ═══════════════════════════════════════════════════════════════════════
   2.  MARK & SWEEP ENGINE  (step-by-step with state machine)
   ═══════════════════════════════════════════════════════════════════════ */
#define MS_CAP       20
#define MS_EDGE_CAP   8
#define MS_ROOT_CAP   6

typedef enum { MS_WHITE = 0, MS_GRAY, MS_BLACK } MSColor;

typedef struct {
    int     id;
    int     alive;
    MSColor color;
    int     is_root;
    char    name[16];
    int     edges[MS_EDGE_CAP];
    int     edge_count;
    void    *payload;
} MSNode;

static MSNode  ms_heap[MS_CAP];
static int     ms_roots[MS_ROOT_CAP];
static int     ms_root_count = 0;
static int     ms_top        = 0;     /* next allocation slot */
static int     ms_gc_count   = 0;
static int     ms_last_freed = 0;
/* Mark phase stack */
static int     ms_stack[MS_CAP];
static int     ms_stack_top = 0;
static int     ms_sweep_cur  = 0;
static char    ms_phase[16]  = "idle";
static LogBuf  ms_log;

static void ms_reset(void) {
    for (int i = 0; i < ms_top; i++) free_payload(&ms_heap[i].payload);
    memset(ms_heap, 0, sizeof(ms_heap));
    ms_root_count = ms_top = ms_gc_count = ms_last_freed = 0;
    ms_stack_top  = ms_sweep_cur = 0;
    strcpy(ms_phase, "idle");
    lb_clear(&ms_log);
    /* seed: 2 roots, 4 children, 2 orphans */
    const char *names[] = {"root0","root1","child0","child1","child2","child3","orphanA","orphanB"};
    for (int i = 0; i < 8; i++) {
        ms_heap[i].id    = i;
        ms_heap[i].alive = 1;
        ms_heap[i].color = MS_WHITE;
        init_payload(&ms_heap[i].payload);
        strncpy(ms_heap[i].name, names[i], 15);
        ms_top = i + 1;
    }
    ms_heap[0].is_root = ms_heap[1].is_root = 1;
    ms_roots[0] = 0; ms_roots[1] = 1; ms_root_count = 2;
    /* edges: root0→child0, root0→child1, root1→child2, child0→child3 */
    ms_heap[0].edges[0] = 2; ms_heap[0].edges[1] = 3; ms_heap[0].edge_count = 2;
    ms_heap[1].edges[0] = 4; ms_heap[1].edge_count = 1;
    ms_heap[2].edges[0] = 5; ms_heap[2].edge_count = 1;
    lb_write(&ms_log, "Heap initialised: 2 roots, 4 children, 2 orphans");
}

static void ms_do_alloc(void) {
    if (ms_top >= MS_CAP) { lb_write(&ms_log, "[ERR] heap full"); return; }
    int id = ms_top++;
    ms_heap[id].id    = id;
    ms_heap[id].alive = 1;
    ms_heap[id].color = MS_WHITE;
    ms_heap[id].edge_count = 0;
    init_payload(&ms_heap[id].payload);
    snprintf(ms_heap[id].name, 16, "node%d", id);
    lb_write(&ms_log, "[ALLOC] %s", ms_heap[id].name);
}

static void ms_do_connect(void) {
    /* find two alive nodes with no edge between them */
    for (int a = 0; a < ms_top; a++) {
        if (!ms_heap[a].alive || ms_heap[a].edge_count >= MS_EDGE_CAP) continue;
        for (int b = 0; b < ms_top; b++) {
            if (a == b || !ms_heap[b].alive) continue;
            int exists = 0;
            for (int e = 0; e < ms_heap[a].edge_count; e++)
                if (ms_heap[a].edges[e] == b) { exists = 1; break; }
            if (!exists) {
                ms_heap[a].edges[ms_heap[a].edge_count++] = b;
                lb_write(&ms_log, "[EDGE] %s→%s", ms_heap[a].name, ms_heap[b].name);
                return;
            }
        }
    }
    lb_write(&ms_log, "[WARN] no new edge possible");
}

static void ms_do_disconnect(void) {
    /* remove an edge from a non-root node */
    for (int i = 0; i < ms_top; i++) {
        if (!ms_heap[i].alive || ms_heap[i].is_root || ms_heap[i].edge_count == 0) continue;
        int tgt = ms_heap[i].edges[--ms_heap[i].edge_count];
        lb_write(&ms_log, "[DISC] %s→%s removed", ms_heap[i].name, ms_heap[tgt].name);
        return;
    }
    lb_write(&ms_log, "[WARN] nothing to disconnect");
}

/* advance one GC step; returns 1 if still running, 0 if idle */
static int ms_do_step(void) {
    if (strcmp(ms_phase, "idle") == 0) {
        /* ── begin: reset colours, push roots ── */
        for (int i = 0; i < ms_top; i++) ms_heap[i].color = MS_WHITE;
        ms_stack_top  = 0;
        ms_sweep_cur  = 0;
        for (int i = 0; i < ms_root_count; i++) {
            int r = ms_roots[i];
            if (ms_heap[r].alive) {
                ms_heap[r].color = MS_GRAY;
                ms_stack[ms_stack_top++] = r;
                lb_write(&ms_log, "[MARK] %s → GRAY (root)", ms_heap[r].name);
            }
        }
        strcpy(ms_phase, "marking");
        lb_write(&ms_log, "=== GC #%d: mark phase ===", ms_gc_count + 1);
        return 1;
    }

    if (strcmp(ms_phase, "marking") == 0) {
        if (ms_stack_top == 0) {
            /* mark done → start sweep */
            strcpy(ms_phase, "sweeping");
            ms_sweep_cur = 0;
            lb_write(&ms_log, "=== GC #%d: sweep phase ===", ms_gc_count + 1);
            return 1;
        }
        /* pop one node, turn BLACK, push WHITE children */
        int id = ms_stack[--ms_stack_top];
        ms_heap[id].color = MS_BLACK;
        lb_write(&ms_log, "[MARK] %s → BLACK", ms_heap[id].name);
        for (int e = 0; e < ms_heap[id].edge_count; e++) {
            int c = ms_heap[id].edges[e];
            if (ms_heap[c].alive && ms_heap[c].color == MS_WHITE) {
                ms_heap[c].color = MS_GRAY;
                ms_stack[ms_stack_top++] = c;
                lb_write(&ms_log, "[MARK] %s → GRAY", ms_heap[c].name);
            }
        }
        return 1;
    }

    if (strcmp(ms_phase, "sweeping") == 0) {
        while (ms_sweep_cur < ms_top && !ms_heap[ms_sweep_cur].alive)
            ms_sweep_cur++;
        if (ms_sweep_cur >= ms_top) {
            strcpy(ms_phase, "idle");
            ms_gc_count++;
            lb_write(&ms_log, "=== GC #%d done: freed %d ===", ms_gc_count, ms_last_freed);
            ms_last_freed = 0;
            return 0;
        }
        MSNode *n = &ms_heap[ms_sweep_cur];
        if (n->color == MS_WHITE) {
            free_payload(&n->payload);
            lb_write(&ms_log, "[SWEEP] free '%s' (unreachable)", n->name);
            n->alive = 0;
            ms_last_freed++;
        } else {
            n->color = MS_WHITE;   /* reset for next cycle */
        }
        ms_sweep_cur++;
        return 1;
    }
    return 0;
}

static void ms_state_json(char *out, int outsz) {
    int pos = 0;
    pos += snprintf(out+pos, outsz-pos, "{\"nodes\":[");
    const char *color_names[] = {"white","gray","black"};
    int first = 1;
    for (int i = 0; i < ms_top; i++) {
        MSNode *n = &ms_heap[i];
        if (!first) pos += snprintf(out+pos, outsz-pos, ",");
        pos += snprintf(out+pos, outsz-pos,
            "{\"id\":%d,\"name\":\"%s\",\"alive\":%d,"
            "\"color\":\"%s\",\"is_root\":%d,\"edges\":[",
            n->id, n->name, n->alive,
            color_names[n->color], n->is_root);
        for (int e = 0; e < n->edge_count; e++)
            pos += snprintf(out+pos, outsz-pos, "%s%d", e?",":"", n->edges[e]);
        pos += snprintf(out+pos, outsz-pos, "]}");
        first = 0;
    }
    char log_str[4096];
    lb_to_json(&ms_log, log_str, sizeof(log_str));
    char os_str[128];
    os_stats_json(os_str, sizeof(os_str));
    pos += snprintf(out+pos, outsz-pos,
        "],\"phase\":\"%s\",\"gc_count\":%d,\"log\":%s,%s}",
        ms_phase, ms_gc_count, log_str, os_str);
}

/* ═══════════════════════════════════════════════════════════════════════
   3.  GENERATIONAL ENGINE
   ═══════════════════════════════════════════════════════════════════════ */
#define GEN_YOUNG_CAP   16
#define GEN_OLD_CAP     48
#define GEN_CHILD_CAP    4
#define GEN_PROMOTE_AGE  3

typedef struct {
    int  id;
    int  age;
    int  alive;
    int  in_old;       /* 1 = tenured to old gen       */
    int  just_promoted;/* flash green on frontend       */
    int  reachable;    /* scratch during GC             */
    char name[20];
    int  children[GEN_CHILD_CAP];
    int  child_count;
    void *payload;
} GenObj;

#define GEN_POOL_CAP (GEN_YOUNG_CAP + GEN_OLD_CAP + 8)
static GenObj  gen_pool[GEN_POOL_CAP];
static int     gen_pool_top  = 0;
static int     gen_next_id   = 0;
static int     gen_young_idx[GEN_YOUNG_CAP]; /* indices into gen_pool */
static int     gen_young_count = 0;
static int     gen_old_idx[GEN_OLD_CAP];
static int     gen_old_count   = 0;
static int     gen_minor_count = 0;
static int     gen_major_count = 0;
static int     gen_root_idx    = -1;   /* one stable root object */
static LogBuf  gen_log;

static void gen_reset(void) {
    for (int i = 0; i < gen_pool_top; i++) free_payload(&gen_pool[i].payload);
    memset(gen_pool, 0, sizeof(gen_pool));
    gen_pool_top = gen_next_id = gen_young_count = gen_old_count = 0;
    gen_minor_count = gen_major_count = 0;
    gen_root_idx = -1;
    lb_clear(&gen_log);
    /* create one permanent root and a few initial young objects */
    gen_root_idx = gen_pool_top++;
    GenObj *r = &gen_pool[gen_root_idx];
    r->id = gen_next_id++; r->alive = 1;
    init_payload(&r->payload);
    snprintf(r->name, 20, "root");
    gen_young_idx[gen_young_count++] = gen_root_idx;
    lb_write(&gen_log, "[NEW] root → young");
    for (int i = 0; i < 3; i++) {
        int idx = gen_pool_top++;
        GenObj *o = &gen_pool[idx];
        o->id = gen_next_id++; o->alive = 1;
        init_payload(&o->payload);
        snprintf(o->name, 20, "obj%d", o->id);
        gen_young_idx[gen_young_count++] = idx;
        lb_write(&gen_log, "[NEW] %s → young", o->name);
    }
}

static void gen_do_alloc(int n) {
    for (int k = 0; k < n; k++) {
        if (gen_young_count >= GEN_YOUNG_CAP) {
            lb_write(&gen_log, "[WARN] nursery full — run minor GC first"); return;
        }
        if (gen_pool_top >= GEN_POOL_CAP) {
            lb_write(&gen_log, "[ERR] pool exhausted"); return;
        }
        int idx = gen_pool_top++;
        GenObj *o = &gen_pool[idx];
        memset(o, 0, sizeof(*o));
        o->id = gen_next_id++; o->alive = 1;
        init_payload(&o->payload);
        snprintf(o->name, 20, "obj%d", o->id);
        gen_young_idx[gen_young_count++] = idx;
        lb_write(&gen_log, "[NEW] %s → young (age 0)", o->name);
    }
}

static void gen_mark_obj(int idx);
static void gen_mark_obj(int idx) {
    GenObj *o = &gen_pool[idx];
    if (!o->alive || o->reachable) return;
    o->reachable = 1;
    for (int i = 0; i < o->child_count; i++)
        gen_mark_obj(o->children[i]);
}

static void gen_do_minor(void) {
    gen_minor_count++;
    lb_write(&gen_log, "--- Minor GC #%d ---", gen_minor_count);

    /* mark from root */
    if (gen_root_idx >= 0) gen_mark_obj(gen_root_idx);

    int new_young[GEN_YOUNG_CAP], new_ycnt = 0;
    int freed = 0, promoted = 0;

    for (int i = 0; i < gen_young_count; i++) {
        int idx = gen_young_idx[i];
        GenObj *o = &gen_pool[idx];
        o->just_promoted = 0;
        if (!o->reachable) {
            free_payload(&o->payload);
            lb_write(&gen_log, "[FREE] %s (young)", o->name);
            o->alive = 0; freed++;
        } else {
            o->age++;
            o->reachable = 0;
            if (o->age >= GEN_PROMOTE_AGE) {
                if (gen_old_count < GEN_OLD_CAP) {
                    gen_old_idx[gen_old_count++] = idx;
                    o->in_old = 1; o->just_promoted = 1;
                    lb_write(&gen_log, "[PROMOTE] %s age=%d → old", o->name, o->age);
                    promoted++;
                } else {
                    new_young[new_ycnt++] = idx; /* old full, keep young */
                }
            } else {
                new_young[new_ycnt++] = idx;
                lb_write(&gen_log, "[SURVIVE] %s age=%d", o->name, o->age);
            }
        }
    }
    gen_young_count = new_ycnt;
    memcpy(gen_young_idx, new_young, new_ycnt * sizeof(int));
    lb_write(&gen_log, "freed=%d promoted=%d survived=%d", freed, promoted, new_ycnt);
}

static void gen_do_major(void) {
    gen_major_count++;
    lb_write(&gen_log, "=== Major GC #%d ===", gen_major_count);
    gen_do_minor();

    /* mark-sweep old gen */
    if (gen_root_idx >= 0) gen_mark_obj(gen_root_idx);
    int new_old[GEN_OLD_CAP], new_ocnt = 0;
    int freed = 0;
    for (int i = 0; i < gen_old_count; i++) {
        int idx = gen_old_idx[i];
        GenObj *o = &gen_pool[idx];
        o->just_promoted = 0;
        if (!o->reachable) {
            free_payload(&o->payload);
            lb_write(&gen_log, "[FREE] %s (old)", o->name);
            o->alive = 0; o->in_old = 0; freed++;
        } else {
            o->reachable = 0;
            new_old[new_ocnt++] = idx;
        }
    }
    gen_old_count = new_ocnt;
    memcpy(gen_old_idx, new_old, new_ocnt * sizeof(int));
    lb_write(&gen_log, "old freed=%d old remaining=%d", freed, new_ocnt);
}

static void gen_state_json(char *out, int outsz) {
    int pos = 0;
    pos += snprintf(out+pos, outsz-pos, "{\"young\":[");
    for (int i = 0; i < gen_young_count; i++) {
        GenObj *o = &gen_pool[gen_young_idx[i]];
        pos += snprintf(out+pos, outsz-pos, "%s{\"id\":%d,\"name\":\"%s\",\"age\":%d}",
            i?",":"", o->id, o->name, o->age);
    }
    pos += snprintf(out+pos, outsz-pos, "],\"old\":[");
    for (int i = 0; i < gen_old_count; i++) {
        GenObj *o = &gen_pool[gen_old_idx[i]];
        pos += snprintf(out+pos, outsz-pos, "%s{\"id\":%d,\"name\":\"%s\","
            "\"age\":%d,\"just_promoted\":%d}",
            i?",":"", o->id, o->name, o->age, o->just_promoted);
    }
    char log_str[4096];
    lb_to_json(&gen_log, log_str, sizeof(log_str));
    char os_str[128];
    os_stats_json(os_str, sizeof(os_str));
    pos += snprintf(out+pos, outsz-pos,
        "],\"minor_count\":%d,\"major_count\":%d,"
        "\"young_cap\":%d,\"old_cap\":%d,\"log\":%s,%s}",
        gen_minor_count, gen_major_count,
        GEN_YOUNG_CAP, GEN_OLD_CAP, log_str, os_str);
}

/* ═══════════════════════════════════════════════════════════════════════
   HTTP SERVER
   ═══════════════════════════════════════════════════════════════════════ */
static char json_buf[JSON_CAP];

static void http_send(int fd, int code, const char *ctype, const char *body) {
    char hdr[512];
    const char *status = (code == 200) ? "OK" : (code == 204) ? "No Content" : "Not Found";
    int blen = body ? (int)strlen(body) : 0;
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n\r\n",
        code, status, ctype, blen);
    write_socket(fd, hdr, strlen(hdr));
    if (body) write_socket(fd, body, blen);
}

static void handle_request(int fd, const char *method, const char *path) {
    /* ── Health ── */
    if (strcmp(path, "/health") == 0) {
        http_send(fd, 200, "application/json", "{\"status\":\"ok\"}");
        return;
    }

    /* ── RC state ── */
    if (strcmp(path, "/state/rc") == 0) {
        rc_state_json(json_buf, JSON_CAP);
        http_send(fd, 200, "application/json", json_buf); return;
    }
    /* ── MS state ── */
    if (strcmp(path, "/state/ms") == 0) {
        ms_state_json(json_buf, JSON_CAP);
        http_send(fd, 200, "application/json", json_buf); return;
    }
    /* ── Gen state ── */
    if (strcmp(path, "/state/gen") == 0) {
        gen_state_json(json_buf, JSON_CAP);
        http_send(fd, 200, "application/json", json_buf); return;
    }

    /* POST routes */
    if (strcmp(method, "POST") != 0) {
        http_send(fd, 404, "text/plain", "Not Found"); return;
    }

    if (strcmp(path, "/rc/alloc")   == 0) { rc_do_alloc();      rc_state_json(json_buf, JSON_CAP); }
    else if (strcmp(path, "/rc/addref")  == 0) { rc_do_addref();     rc_state_json(json_buf, JSON_CAP); }
    else if (strcmp(path, "/rc/dropref") == 0) { rc_do_dropref();    rc_state_json(json_buf, JSON_CAP); }
    else if (strcmp(path, "/rc/cycle")   == 0) { rc_do_cycle();      rc_state_json(json_buf, JSON_CAP); }
    else if (strcmp(path, "/rc/reset")   == 0) { rc_reset();         rc_state_json(json_buf, JSON_CAP); }
    else if (strcmp(path, "/ms/alloc")      == 0) { ms_do_alloc();      ms_state_json(json_buf, JSON_CAP); }
    else if (strcmp(path, "/ms/connect")    == 0) { ms_do_connect();    ms_state_json(json_buf, JSON_CAP); }
    else if (strcmp(path, "/ms/disconnect") == 0) { ms_do_disconnect(); ms_state_json(json_buf, JSON_CAP); }
    else if (strcmp(path, "/ms/gc/step")    == 0) { ms_do_step();       ms_state_json(json_buf, JSON_CAP); }
    else if (strcmp(path, "/ms/reset")      == 0) { ms_reset();         ms_state_json(json_buf, JSON_CAP); }
    else if (strcmp(path, "/gen/alloc")  == 0) { gen_do_alloc(3);   gen_state_json(json_buf, JSON_CAP); }
    else if (strcmp(path, "/gen/minor")  == 0) { gen_do_minor();     gen_state_json(json_buf, JSON_CAP); }
    else if (strcmp(path, "/gen/major")  == 0) { gen_do_major();     gen_state_json(json_buf, JSON_CAP); }
    else if (strcmp(path, "/gen/reset")  == 0) { gen_reset();        gen_state_json(json_buf, JSON_CAP); }
    else { http_send(fd, 404, "text/plain", "Not Found"); return; }

    http_send(fd, 200, "application/json", json_buf);
}

static int server_fd = -1;
static void on_sigint(int s) { (void)s; if (server_fd>=0) close_socket(server_fd); exit(0); }

int main(void) {
    signal(SIGINT, on_sigint);

    /* initialise GC engines */
    rc_reset(); ms_reset(); gen_reset();

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed.\n");
        return 1;
    }
#endif

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    listen(server_fd, 16);
    printf("GC backend listening on http://localhost:%d\n", PORT);
    printf("Press Ctrl-C to stop.\n\n");

    static char req_buf[BUF_SIZE];
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        int cfd = accept(server_fd, (struct sockaddr *)&client_addr, &clen);
        if (cfd < 0) continue;

        int n = read_socket(cfd, req_buf, BUF_SIZE - 1);
        if (n <= 0) { close_socket(cfd); continue; }
        req_buf[n] = '\0';

        /* parse method and path from first line */
        char method[8] = {0}, path[128] = {0};
        sscanf(req_buf, "%7s %127s", method, path);

        printf("[%s] %s\n", method, path);

        /* handle CORS preflight */
        if (strcmp(method, "OPTIONS") == 0) {
            http_send(cfd, 204, "text/plain", NULL);
        } else {
            handle_request(cfd, method, path);
        }
        close_socket(cfd);
    }
    return 0;
}
