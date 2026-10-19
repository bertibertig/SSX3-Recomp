/*
 * LAN multiplayer – sessions, lobby, clock sync and snapshot relay over ENet (see net.h).
 *
 * Wire format (all little-endian, see net_wire.h), the first byte of every packet is its type:
 *   client -> host   C_HELLO, C_PROFILE, C_READY, C_CHAT, C_FINISH (channel 0), C_SNAP, C_PING (channel 1)
 *   host -> client   H_WELCOME, H_REJECT, H_KICK, H_LOBBY, H_CHAT (channel 0), H_SNAP, H_PONG (channel 1)
 * H_LOBBY always carries the whole lobby (rules, state, every member), so a client never has to merge partial
 * updates; clients derive their START and RESULTS events from the difference to the previous lobby.
 *
 * Members are indexed by their id (members[id]); the host is id 0. A client identifies itself with a random
 * 64-bit token, so after a drop it gets its old id, place and score back (LAN_RECONNECT_MS).
 */
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <enet/enet.h>

#include "net.h"
#include "net_wire.h"

#define HELLO_MAGIC   0x4E4C3353u      /* "S3LN" */

enum {
    C_HELLO = 0x01, C_PROFILE, C_READY, C_CHAT, C_FINISH, C_SNAP, C_PING,
    H_WELCOME = 0x81, H_REJECT, H_KICK, H_LOBBY, H_CHAT, H_SNAP, H_PONG,
};

enum { CH_RELIABLE = 0, CH_UNRELIABLE = 1, CH_COUNT = 2 };

#define DISCONNECT_LEAVE 0x4C454156u   /* disconnect data of an orderly leave ("LEAV"); a timeout carries 0 */

#define EVENT_QUEUE   128
#define SNAP_HISTORY  8
#define CLOCK_SAMPLES 8
#define EXTRAPOLATE_MS 250u

typedef struct {
    ENetPeer *peer;
    uint64_t  token;
    uint32_t  drop_time;                /* local clock when the connection was lost */
    uint32_t  chat_time[5];             /* chat flood limit: 5 messages per 5 s */
} host_member;

typedef struct {
    lan_snapshot s[SNAP_HISTORY];
    int          count, head;           /* head = index of the newest */
} snap_ring;

struct lan_session {
    lan_config  cfg;
    int         host;
    ENetHost   *enet;
    ENetPeer   *server;                 /* client: the connection to the host */
    ENetAddress server_addr;
    ENetSocket  discovery;              /* host: answers lobby browser queries */
    uint64_t    token;
    uint8_t     local_id;
    int         joined, kicked, dropped;
    lan_profile me;
    lan_lobby   lobby;
    int         lobby_dirty;
    uint32_t    next_ping_update;

    host_member hm[LAN_MAX_MEMBERS];
    uint64_t    banned[32];
    int         nbanned;

    /* client clock: host time = local time + offset, from the ping sample with the smallest round trip */
    int32_t     clock_offset[CLOCK_SAMPLES];
    uint32_t    clock_rtt[CLOCK_SAMPLES];
    int         clock_count, clock_next;
    uint32_t    next_clock_ping;
    int         pings_sent;

    snap_ring   snaps[LAN_MAX_MEMBERS];

    lan_event   events[EVENT_QUEUE];
    int         ev_head, ev_count;
};

static int g_net_ready;

/* ---------------------------------------------------------------- helpers */

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
static void logf_(const lan_session *s, const char *fmt, ...)
{
    va_list ap;
    if (s && s->cfg.quiet)
        return;
    va_start(ap, fmt);
    fprintf(stderr, "[LAN %s] ", s ? (s->host ? "host" : "client") : "-");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static uint32_t local_ms(const lan_session *s)
{
    return enet_time_get() + (uint32_t)s->cfg.clock_skew_ms;
}

uint32_t lan_time(const lan_session *s)
{
    if (s->host)
        return local_ms(s);
    if (!s->clock_count)
        return local_ms(s);
    {
        int i, best = 0;
        for (i = 1; i < s->clock_count; i++)
            if (s->clock_rtt[i] < s->clock_rtt[best])
                best = i;
        return local_ms(s) + (uint32_t)s->clock_offset[best];
    }
}

int lan_clock_synced(const lan_session *s)
{
    return s->host || s->clock_count >= 3;
}

static void push_event(lan_session *s, lan_event_type type, uint8_t player, uint8_t reason, uint32_t time,
                       const char *text)
{
    lan_event *e;
    if (s->ev_count == EVENT_QUEUE) {       /* full: drop the oldest */
        s->ev_head = (s->ev_head + 1) % EVENT_QUEUE;
        s->ev_count--;
    }
    e = &s->events[(s->ev_head + s->ev_count) % EVENT_QUEUE];
    s->ev_count++;
    memset(e, 0, sizeof *e);
    e->type = type; e->player = player; e->reason = reason; e->time = time;
    if (text)
        snprintf(e->text, sizeof e->text, "%s", text);
}

int lan_next_event(lan_session *s, lan_event *ev)
{
    if (!s->ev_count)
        return 0;
    *ev = s->events[s->ev_head];
    s->ev_head = (s->ev_head + 1) % EVENT_QUEUE;
    s->ev_count--;
    return 1;
}

static void sanitize_text(char *t, size_t max)
{
    size_t i, n = strlen(t);
    if (n > max)
        t[n = max] = 0;
    for (i = 0; i < n; i++)
        if ((unsigned char)t[i] < 0x20 || (unsigned char)t[i] > 0x7E)
            t[i] = '_';
}

static void sanitize_name(char *name)
{
    size_t n, start = 0;
    sanitize_text(name, LAN_NAME_MAX);
    while (name[start] == ' ')
        start++;
    memmove(name, name + start, strlen(name + start) + 1);
    n = strlen(name);
    while (n && name[n - 1] == ' ')
        name[--n] = 0;
    if (!n)
        strcpy(name, "Rider");
}

static int name_taken(const lan_lobby *l, const char *name, int except)
{
    int i;
    for (i = 0; i < LAN_MAX_MEMBERS; i++) {
        const char *a = l->members[i].profile.name, *b = name;
        if (i == except || !l->members[i].used)
            continue;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
            a++, b++;
        if (!*a && !*b)
            return 1;
    }
    return 0;
}

/* "Zoe", "Zoe2", "Zoe3" ...; the suffix replaces the end of a name that would get too long. */
static void unique_name(const lan_lobby *l, char *name, int self)
{
    char base[LAN_NAME_MAX + 1], suffix[8];
    int k;
    sanitize_name(name);
    if (!name_taken(l, name, self))
        return;
    snprintf(base, sizeof base, "%s", name);
    for (k = 2; k < 100; k++) {
        size_t sl, bl;
        snprintf(suffix, sizeof suffix, "%d", k);
        sl = strlen(suffix);
        bl = strlen(base);
        if (bl + sl > LAN_NAME_MAX)
            bl = LAN_NAME_MAX - sl;
        snprintf(name, LAN_NAME_MAX + 1, "%.*s%s", (int)bl, base, suffix);
        if (!name_taken(l, name, self))
            return;
    }
}

static void clamp_rules(lan_rules *r)
{
    int cap = r->mode == LAN_MODE_FREEROAM ? LAN_MAX_PLAYERS : 6;   /* races: the engine's 6 riders */
    if (r->max_players < 2) r->max_players = 2;
    if (r->max_players > cap) r->max_players = (uint8_t)cap;
    if (r->mode > LAN_MODE_FREESTYLE) r->mode = LAN_MODE_RACE;
    if (r->collide > LAN_COLLIDE_BUMP) r->collide = LAN_COLLIDE_GHOST;
    sanitize_text(r->lobby_name, LAN_LOBBY_NAME_MAX);
    if (!r->lobby_name[0])
        strcpy(r->lobby_name, "SSX 3 LAN");
}

/* What the host's rules allow of a profile: equal stats = level 1, one bar in every attribute. */
static void apply_rules(const lan_rules *r, lan_profile *p)
{
    int i;
    if (!r->leveled) {
        p->level = 1;
        for (i = 0; i < LAN_ATTRIBS; i++)
            p->attrib[i] = LAN_ATTRIB_BASE;
    }
    for (i = 0; i < LAN_ATTRIBS; i++)
        if (p->attrib[i] < LAN_ATTRIB_BASE || p->attrib[i] > 11 * LAN_ATTRIB_BASE)
            p->attrib[i] = LAN_ATTRIB_BASE;
    if (!r->bragging)
        p->handicaps = 0;
}

static void count_members(const lan_lobby *l, int *players, int *spectators)
{
    int i;
    *players = *spectators = 0;
    for (i = 0; i < LAN_MAX_MEMBERS; i++)
        if (l->members[i].used) {
            if (l->members[i].profile.spectator)
                (*spectators)++;
            else
                (*players)++;
        }
}

/* ---------------------------------------------------------------- serialization */

static void put_profile(net_writer *w, const lan_profile *p)
{
    int i;
    nw_str(w, p->name, LAN_NAME_MAX);
    nw_u8(w, p->character); nw_u8(w, p->level);
    for (i = 0; i < LAN_ATTRIBS; i++)
        nw_u8(w, p->attrib[i]);
    nw_u8(w, p->gear_count > LAN_GEAR_MAX ? LAN_GEAR_MAX : p->gear_count);
    for (i = 0; i < p->gear_count && i < LAN_GEAR_MAX; i++)
        nw_u16(w, p->gear[i]);
    nw_u8(w, p->handicaps); nw_u8(w, p->spectator);
}

static void get_profile(net_reader *r, lan_profile *p)
{
    int i;
    uint32_t n;
    memset(p, 0, sizeof *p);
    nr_str(r, p->name, sizeof p->name);
    p->character = (uint8_t)nr_u8(r); p->level = (uint8_t)nr_u8(r);
    for (i = 0; i < LAN_ATTRIBS; i++)
        p->attrib[i] = (uint8_t)nr_u8(r);
    n = nr_u8(r);
    for (i = 0; i < (int)n; i++) {
        uint32_t item = nr_u16(r);
        if (i < LAN_GEAR_MAX)
            p->gear[i] = (uint16_t)item;
    }
    p->gear_count = (uint8_t)(n > LAN_GEAR_MAX ? LAN_GEAR_MAX : n);
    p->handicaps = (uint8_t)nr_u8(r); p->spectator = (uint8_t)nr_u8(r) ? 1 : 0;
}

static void put_rules(net_writer *w, const lan_rules *r)
{
    nw_str(w, r->lobby_name, LAN_LOBBY_NAME_MAX);
    nw_u8(w, r->mode); nw_u8(w, r->peak); nw_u8(w, r->event); nw_u8(w, r->max_players);
    nw_u8(w, r->leveled); nw_u8(w, r->ai_fill); nw_u8(w, r->collide); nw_u8(w, r->spectators);
    nw_u8(w, r->bragging);
}

static void get_rules(net_reader *r, lan_rules *o)
{
    memset(o, 0, sizeof *o);
    nr_str(r, o->lobby_name, sizeof o->lobby_name);
    o->mode = (uint8_t)nr_u8(r); o->peak = (uint8_t)nr_u8(r); o->event = (uint8_t)nr_u8(r);
    o->max_players = (uint8_t)nr_u8(r); o->leveled = (uint8_t)nr_u8(r); o->ai_fill = (uint8_t)nr_u8(r);
    o->collide = (uint8_t)nr_u8(r); o->spectators = (uint8_t)nr_u8(r); o->bragging = (uint8_t)nr_u8(r);
}

static void put_snap(net_writer *w, const lan_snapshot *s)
{
    int i;
    nw_u32(w, s->time);
    for (i = 0; i < 3; i++) nw_f32(w, s->pos[i]);
    for (i = 0; i < 4; i++) nw_f32(w, s->rot[i]);
    for (i = 0; i < 3; i++) nw_f32(w, s->vel[i]);
    nw_u16(w, s->anim); nw_u16(w, s->flags);
    nw_f32(w, s->boost); nw_f32(w, s->progress);
}

static void get_snap(net_reader *r, lan_snapshot *s)
{
    int i;
    s->time = nr_u32(r);
    for (i = 0; i < 3; i++) s->pos[i] = nr_f32(r);
    for (i = 0; i < 4; i++) s->rot[i] = nr_f32(r);
    for (i = 0; i < 3; i++) s->vel[i] = nr_f32(r);
    s->anim = (uint16_t)nr_u16(r); s->flags = (uint16_t)nr_u16(r);
    s->boost = nr_f32(r); s->progress = nr_f32(r);
}

static size_t put_lobby(uint8_t *buf, size_t cap, const lan_lobby *l)
{
    net_writer w;
    int i, n = 0;
    nw_init(&w, buf, cap);
    nw_u8(&w, H_LOBBY);
    put_rules(&w, &l->rules);
    nw_u8(&w, l->state); nw_u8(&w, l->round); nw_u32(&w, l->start_time);
    for (i = 0; i < LAN_MAX_MEMBERS; i++)
        n += l->members[i].used;
    nw_u8(&w, (uint32_t)n);
    for (i = 0; i < LAN_MAX_MEMBERS; i++) {
        const lan_member *m = &l->members[i];
        if (!m->used)
            continue;
        nw_u8(&w, m->id); nw_u8(&w, m->connected); nw_u8(&w, m->ready); nw_u8(&w, m->finished);
        nw_u16(&w, m->ping_ms); nw_u32(&w, m->finish_ms); nw_u32(&w, m->score);
        nw_u8(&w, m->place); nw_u8(&w, m->slot);
        put_profile(&w, &m->profile);
    }
    return w.bad ? 0 : w.len;
}

static int get_lobby(net_reader *r, lan_lobby *l)
{
    int i, n;
    memset(l, 0, sizeof *l);
    get_rules(r, &l->rules);
    l->state = (uint8_t)nr_u8(r); l->round = (uint8_t)nr_u8(r); l->start_time = nr_u32(r);
    n = (int)nr_u8(r);
    for (i = 0; i < n && !r->bad; i++) {
        uint32_t id = nr_u8(r);
        lan_member m;
        memset(&m, 0, sizeof m);
        m.used = 1; m.id = (uint8_t)id;
        m.connected = (uint8_t)nr_u8(r); m.ready = (uint8_t)nr_u8(r); m.finished = (uint8_t)nr_u8(r);
        m.ping_ms = (uint16_t)nr_u16(r); m.finish_ms = nr_u32(r); m.score = nr_u32(r);
        m.place = (uint8_t)nr_u8(r); m.slot = (uint8_t)nr_u8(r);
        get_profile(r, &m.profile);
        if (id >= LAN_MAX_MEMBERS)
            return 0;
        l->members[id] = m;
    }
    return !r->bad;
}

/* ---------------------------------------------------------------- sending */

static void send_buf(ENetPeer *peer, int channel, const void *buf, size_t len)
{
    ENetPacket *p;
    if (!peer || !len)
        return;
    p = enet_packet_create(buf, len, channel == CH_RELIABLE ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (p && enet_peer_send(peer, (enet_uint8)channel, p) < 0)
        enet_packet_destroy(p);
}

/* Host: to every connected member except `except` (LAN_NO_PLAYER = all). */
static void host_broadcast(lan_session *s, int channel, const void *buf, size_t len, int except)
{
    int i;
    for (i = 1; i < LAN_MAX_MEMBERS; i++)
        if (i != except && s->lobby.members[i].used && s->lobby.members[i].connected && s->hm[i].peer)
            send_buf(s->hm[i].peer, channel, buf, len);
}

static void host_chat(lan_session *s, uint8_t from, const char *text)
{
    uint8_t buf[256];
    net_writer w;
    nw_init(&w, buf, sizeof buf);
    nw_u8(&w, H_CHAT); nw_u8(&w, from); nw_str(&w, text, LAN_CHAT_MAX);
    host_broadcast(s, CH_RELIABLE, buf, w.len, LAN_NO_PLAYER);
    push_event(s, LAN_EV_CHAT, from, 0, 0, text);
}

static void host_system(lan_session *s, const char *fmt, const char *name)
{
    char text[LAN_CHAT_MAX + 1];
    snprintf(text, sizeof text, fmt, name);
    logf_(s, "%s", text);
    host_chat(s, LAN_NO_PLAYER, text);
}

static void send_reject(lan_session *s, ENetPeer *peer, int type, int reason)
{
    uint8_t buf[2] = { (uint8_t)type, (uint8_t)reason };
    (void)s;
    send_buf(peer, CH_RELIABLE, buf, 2);
    enet_peer_disconnect_later(peer, (enet_uint32)reason);
    peer->data = NULL;
}

/* ---------------------------------------------------------------- host logic */

static void host_flush_lobby(lan_session *s)
{
    uint8_t buf[4096];                              /* 16 members with full gear lists: ~1.8 KB */
    size_t len;
    if (!s->lobby_dirty)
        return;
    s->lobby_dirty = 0;
    len = put_lobby(buf, sizeof buf, &s->lobby);
    host_broadcast(s, CH_RELIABLE, buf, len, LAN_NO_PLAYER);
    push_event(s, LAN_EV_LOBBY, 0, 0, 0, NULL);
}

static void remove_member(lan_session *s, int id)
{
    memset(&s->lobby.members[id], 0, sizeof s->lobby.members[id]);
    memset(&s->hm[id], 0, sizeof s->hm[id]);
    memset(&s->snaps[id], 0, sizeof s->snaps[id]);
    s->lobby_dirty = 1;
}

static int is_banned(const lan_session *s, uint64_t token)
{
    int i;
    for (i = 0; i < s->nbanned; i++)
        if (s->banned[i] == token)
            return 1;
    return 0;
}

static void host_hello(lan_session *s, ENetPeer *peer, net_reader *r)
{
    uint32_t magic = nr_u32(r), version = nr_u16(r), game = nr_u32(r);
    uint64_t token = nr_u64(r);
    lan_profile p;
    int i, id = -1, players, spectators;
    lan_member *m;
    uint8_t buf[16];
    net_writer w;

    get_profile(r, &p);
    if (r->bad || magic != HELLO_MAGIC) { send_reject(s, peer, H_REJECT, LAN_REJECT_BAD_HELLO); return; }
    if (version != LAN_PROTOCOL_VERSION) { send_reject(s, peer, H_REJECT, LAN_REJECT_VERSION); return; }
    if (game != s->cfg.game_id) { send_reject(s, peer, H_REJECT, LAN_REJECT_GAME); return; }
    if (is_banned(s, token)) { send_reject(s, peer, H_REJECT, LAN_REJECT_KICKED); return; }

    /* the same token again: a reconnect (or a second hello over a new connection) */
    for (i = 1; i < LAN_MAX_MEMBERS; i++)
        if (s->lobby.members[i].used && s->hm[i].token == token) {
            id = i;
            break;
        }
    if (id >= 0) {
        m = &s->lobby.members[id];
        if (s->hm[id].peer && s->hm[id].peer != peer) {
            s->hm[id].peer->data = NULL;
            enet_peer_reset(s->hm[id].peer);
        }
        m->connected = 1;
        logf_(s, "%s reconnected as player %d", m->profile.name, id);
        host_system(s, "%s is back", m->profile.name);
    } else {
        count_members(&s->lobby, &players, &spectators);
        if (!p.spectator && s->lobby.state != LAN_STATE_LOBBY && s->lobby.rules.mode != LAN_MODE_FREEROAM)
            p.spectator = 1;                        /* a race is running: watch until the next one */
        if (!p.spectator && players >= s->lobby.rules.max_players)
            p.spectator = 1;
        if (p.spectator && (!s->lobby.rules.spectators || spectators >= LAN_MAX_SPECTATORS)) {
            send_reject(s, peer, H_REJECT, s->lobby.state == LAN_STATE_LOBBY ? LAN_REJECT_FULL : LAN_REJECT_RUNNING);
            return;
        }
        for (i = 1; i < LAN_MAX_MEMBERS; i++)
            if (!s->lobby.members[i].used) {
                id = i;
                break;
            }
        if (id < 0) { send_reject(s, peer, H_REJECT, LAN_REJECT_FULL); return; }
        m = &s->lobby.members[id];
        memset(m, 0, sizeof *m);
        m->used = 1; m->id = (uint8_t)id; m->connected = 1; m->slot = LAN_NO_PLAYER;
        apply_rules(&s->lobby.rules, &p);
        m->profile = p;
        unique_name(&s->lobby, m->profile.name, id);
        s->hm[id].token = token;
        logf_(s, "%s joined as player %d%s", m->profile.name, id, p.spectator ? " (spectator)" : "");
        host_system(s, p.spectator ? "%s is watching" : "%s joined", m->profile.name);
    }
    s->hm[id].peer = peer;
    s->hm[id].drop_time = 0;
    peer->data = &s->hm[id];
    enet_peer_timeout(peer, 32, 3000, 6000);

    nw_init(&w, buf, sizeof buf);
    nw_u8(&w, H_WELCOME); nw_u8(&w, (uint32_t)id); nw_u32(&w, local_ms(s));
    send_buf(peer, CH_RELIABLE, buf, w.len);
    s->lobby_dirty = 1;
}

static void assign_places(lan_session *s)
{
    lan_member *order[LAN_MAX_MEMBERS];
    int i, j, n = 0, all = 1;
    static const uint32_t points[] = { 10, 8, 6, 5, 4, 3, 2, 1 };

    for (i = 0; i < LAN_MAX_MEMBERS; i++) {
        lan_member *m = &s->lobby.members[i];
        if (!m->used || m->profile.spectator || m->slot == LAN_NO_PLAYER)
            continue;
        if (m->finished)
            order[n++] = m;
        else if (m->connected)
            all = 0;
    }
    for (i = 1; i < n; i++)                         /* insertion sort by race time */
        for (j = i; j > 0 && order[j]->finish_ms < order[j - 1]->finish_ms; j--) {
            lan_member *t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    for (i = 0; i < n; i++)
        order[i]->place = (uint8_t)(i + 1);
    if (all && s->lobby.state == LAN_STATE_RUNNING) {
        for (i = 0; i < n; i++)
            order[i]->score += i < (int)(sizeof points / sizeof points[0]) ? points[i] : 0;
        s->lobby.state = LAN_STATE_RESULTS;
        logf_(s, "round %u over, %d finished", s->lobby.round, n);
    }
    s->lobby_dirty = 1;
    push_event(s, LAN_EV_RESULTS, 0, 0, 0, NULL);
}

static void member_finish(lan_session *s, int id, uint32_t ms)
{
    lan_member *m = &s->lobby.members[id];
    if (s->lobby.state != LAN_STATE_RUNNING || m->profile.spectator || m->slot == LAN_NO_PLAYER || m->finished)
        return;
    m->finished = 1;
    m->finish_ms = ms;
    logf_(s, "%s finished in %u.%03u s", m->profile.name, ms / 1000, ms % 1000);
    assign_places(s);
}

static void member_chat(lan_session *s, int id, const char *text_in)
{
    char text[LAN_CHAT_MAX + 1];
    host_member *h = &s->hm[id];
    uint32_t now = local_ms(s);
    snprintf(text, sizeof text, "%s", text_in);
    sanitize_text(text, LAN_CHAT_MAX);
    if (!text[0])
        return;
    if (id && h->chat_time[0] && now - h->chat_time[0] < 5000)
        return;                                     /* 5 messages within 5 s: drop */
    memmove(h->chat_time, h->chat_time + 1, sizeof h->chat_time - sizeof h->chat_time[0]);
    h->chat_time[4] = now;
    host_chat(s, (uint8_t)id, text);
}

static void store_snap(lan_session *s, int id, const lan_snapshot *snap)
{
    snap_ring *r = &s->snaps[id];
    if (r->count && (int32_t)(snap->time - r->s[r->head].time) <= 0)
        return;                                     /* old or repeated */
    r->head = (r->head + 1) % SNAP_HISTORY;
    r->s[r->head] = *snap;
    if (r->count < SNAP_HISTORY)
        r->count++;
}

static void host_relay_snap(lan_session *s, int id, const lan_snapshot *snap)
{
    uint8_t buf[80];
    net_writer w;
    store_snap(s, id, snap);
    nw_init(&w, buf, sizeof buf);
    nw_u8(&w, H_SNAP); nw_u8(&w, (uint32_t)id);
    put_snap(&w, snap);
    host_broadcast(s, CH_UNRELIABLE, buf, w.len, id);
}

static void host_packet(lan_session *s, ENetPeer *peer, const uint8_t *data, size_t len)
{
    net_reader r;
    host_member *h = (host_member *)peer->data;
    int id = h ? (int)(h - s->hm) : -1, type;
    lan_member *m;

    nr_init(&r, data, len);
    type = (int)nr_u8(&r);
    if (type == C_HELLO) {
        if (id < 0)
            host_hello(s, peer, &r);
        return;
    }
    if (id < 0)
        return;                                     /* nothing but a hello before the welcome */
    m = &s->lobby.members[id];
    switch (type) {
    case C_PROFILE: {
        lan_profile p;
        get_profile(&r, &p);
        if (r.bad)
            return;
        p.spectator = m->profile.spectator;         /* switching sides is a rejoin */
        apply_rules(&s->lobby.rules, &p);
        m->profile = p;
        unique_name(&s->lobby, m->profile.name, id);
        s->lobby_dirty = 1;
        break;
    }
    case C_READY:
        m->ready = nr_u8(&r) ? 1 : 0;
        s->lobby_dirty = 1;
        break;
    case C_CHAT: {
        char text[LAN_CHAT_MAX + 1];
        nr_str(&r, text, sizeof text);
        if (!r.bad)
            member_chat(s, id, text);
        break;
    }
    case C_FINISH: {
        uint32_t ms = nr_u32(&r);
        if (!r.bad)
            member_finish(s, id, ms);
        break;
    }
    case C_SNAP: {
        lan_snapshot snap;
        get_snap(&r, &snap);
        if (!r.bad && !m->profile.spectator)
            host_relay_snap(s, id, &snap);
        break;
    }
    case C_PING: {
        uint32_t t = nr_u32(&r);
        uint8_t buf[16];
        net_writer w;
        if (r.bad)
            return;
        nw_init(&w, buf, sizeof buf);
        nw_u8(&w, H_PONG); nw_u32(&w, t); nw_u32(&w, local_ms(s));
        send_buf(peer, CH_UNRELIABLE, buf, w.len);
        break;
    }
    default:
        break;
    }
}

static void host_lost(lan_session *s, ENetPeer *peer, int left)
{
    host_member *h = (host_member *)peer->data;
    int id;
    lan_member *m;
    if (!h)
        return;
    id = (int)(h - s->hm);
    m = &s->lobby.members[id];
    peer->data = NULL;
    h->peer = NULL;
    if (!m->used)
        return;
    push_event(s, LAN_EV_DISCONNECTED, (uint8_t)id, 0, 0, NULL);
    if (left) {                                     /* left on purpose: no place kept */
        char name[LAN_NAME_MAX + 1];
        snprintf(name, sizeof name, "%s", m->profile.name);
        remove_member(s, id);
        host_system(s, "%s left", name);
        if (s->lobby.state == LAN_STATE_RUNNING)
            assign_places(s);
        return;
    }
    m->connected = 0;
    m->ready = 0;
    h->drop_time = local_ms(s) | 1;                 /* nonzero; may be 1 ms ahead of `now`, hence signed compares */
    host_system(s, "%s lost the connection", m->profile.name);
    s->lobby_dirty = 1;
    if (s->lobby.state == LAN_STATE_RUNNING)
        assign_places(s);                           /* the others may all be done now */
}

static void host_tick(lan_session *s)
{
    uint32_t now = local_ms(s);
    int i;

    for (i = 1; i < LAN_MAX_MEMBERS; i++) {
        lan_member *m = &s->lobby.members[i];
        if (m->used && !m->connected && s->hm[i].drop_time
            && (int32_t)(now - s->hm[i].drop_time) > (int32_t)LAN_RECONNECT_MS) {
            char name[LAN_NAME_MAX + 1];
            snprintf(name, sizeof name, "%s", m->profile.name);
            remove_member(s, i);
            host_system(s, "%s left", name);
        }
    }
    if (s->lobby.state == LAN_STATE_COUNTDOWN && (int32_t)(now - s->lobby.start_time) >= 0) {
        s->lobby.state = LAN_STATE_RUNNING;
        s->lobby_dirty = 1;
    }
    if ((int32_t)(now - s->next_ping_update) >= 0) {
        s->next_ping_update = now + 1000;
        for (i = 1; i < LAN_MAX_MEMBERS; i++)
            if (s->lobby.members[i].used && s->hm[i].peer
                && s->lobby.members[i].ping_ms != (uint16_t)s->hm[i].peer->roundTripTime) {
                s->lobby.members[i].ping_ms = (uint16_t)s->hm[i].peer->roundTripTime;
                s->lobby_dirty = 1;
            }
    }
}

/* Lobby browser queries: "S3LQ" + version; the answer describes this lobby. */
static void host_discovery(lan_session *s)
{
    uint8_t in[64], out[128];
    ENetBuffer b;
    ENetAddress from;
    int n, players, spectators;

    if (s->discovery == ENET_SOCKET_NULL)
        return;
    for (;;) {
        net_reader r;
        net_writer w;
        b.data = in; b.dataLength = sizeof in;
        n = enet_socket_receive(s->discovery, &from, &b, 1);
        if (n <= 0)
            return;
        nr_init(&r, in, (size_t)n);
        if (nr_u32(&r) != 0x514C3353u || r.bad)     /* "S3LQ" */
            continue;
        count_members(&s->lobby, &players, &spectators);
        nw_init(&w, out, sizeof out);
        nw_u32(&w, 0x524C3353u);                    /* "S3LR" */
        nw_u16(&w, LAN_PROTOCOL_VERSION); nw_u32(&w, s->cfg.game_id); nw_u16(&w, s->cfg.port);
        nw_str(&w, s->lobby.rules.lobby_name, LAN_LOBBY_NAME_MAX);
        nw_u8(&w, s->lobby.rules.mode); nw_u8(&w, s->lobby.state);
        nw_u8(&w, (uint32_t)players); nw_u8(&w, s->lobby.rules.max_players);
        b.data = out; b.dataLength = w.len;
        enet_socket_send(s->discovery, &from, &b, 1);
    }
}

/* ---------------------------------------------------------------- client logic */

static void client_send_hello(lan_session *s)
{
    uint8_t buf[160];
    net_writer w;
    nw_init(&w, buf, sizeof buf);
    nw_u8(&w, C_HELLO); nw_u32(&w, HELLO_MAGIC); nw_u16(&w, LAN_PROTOCOL_VERSION);
    nw_u32(&w, s->cfg.game_id); nw_u64(&w, s->token);
    put_profile(&w, &s->me);
    send_buf(s->server, CH_RELIABLE, buf, w.len);
}

static void client_lobby(lan_session *s, net_reader *r)
{
    lan_lobby old = s->lobby, nl;
    int i, places = 0;
    if (!get_lobby(r, &nl))
        return;
    s->lobby = nl;
    push_event(s, LAN_EV_LOBBY, 0, 0, 0, NULL);
    if (nl.state == LAN_STATE_COUNTDOWN && (old.state != LAN_STATE_COUNTDOWN || old.round != nl.round))
        push_event(s, LAN_EV_START, 0, 0, nl.start_time, NULL);
    for (i = 0; i < LAN_MAX_MEMBERS; i++)
        if (nl.members[i].place != old.members[i].place)
            places = 1;
    if (places)
        push_event(s, LAN_EV_RESULTS, 0, 0, 0, NULL);
}

static void client_pong(lan_session *s, net_reader *r)
{
    uint32_t sent = nr_u32(r), host_t = nr_u32(r), now = local_ms(s), rtt;
    if (r->bad)
        return;
    rtt = now - sent;
    if (rtt > 5000)
        return;
    s->clock_rtt[s->clock_next] = rtt;
    s->clock_offset[s->clock_next] = (int32_t)(host_t + rtt / 2 - now);
    s->clock_next = (s->clock_next + 1) % CLOCK_SAMPLES;
    if (s->clock_count < CLOCK_SAMPLES)
        s->clock_count++;
}

static void client_packet(lan_session *s, const uint8_t *data, size_t len)
{
    net_reader r;
    int type;
    nr_init(&r, data, len);
    type = (int)nr_u8(&r);
    switch (type) {
    case H_WELCOME: {
        uint32_t id = nr_u8(&r);
        if (r.bad || id >= LAN_MAX_MEMBERS)
            return;
        s->local_id = (uint8_t)id;
        s->joined = 1;
        s->pings_sent = 0;
        s->next_clock_ping = local_ms(s);
        logf_(s, "joined as player %u", id);
        push_event(s, LAN_EV_JOINED, (uint8_t)id, 0, 0, NULL);
        break;
    }
    case H_REJECT: {
        uint32_t reason = nr_u8(&r);
        logf_(s, "rejected: %s", lan_reject_text((int)reason));
        push_event(s, LAN_EV_REJECTED, LAN_NO_PLAYER, (uint8_t)reason, 0, NULL);
        s->kicked = 1;                              /* the coming disconnect is not a drop */
        break;
    }
    case H_KICK:
        logf_(s, "kicked by the host");
        push_event(s, LAN_EV_KICKED, LAN_NO_PLAYER, LAN_REJECT_KICKED, 0, NULL);
        s->kicked = 1;
        break;
    case H_LOBBY:
        if (s->joined)
            client_lobby(s, &r);
        break;
    case H_CHAT: {
        uint32_t from = nr_u8(&r);
        char text[LAN_CHAT_MAX + 1];
        nr_str(&r, text, sizeof text);
        if (!r.bad)
            push_event(s, LAN_EV_CHAT, (uint8_t)from, 0, 0, text);
        break;
    }
    case H_SNAP: {
        uint32_t id = nr_u8(&r);
        lan_snapshot snap;
        get_snap(&r, &snap);
        if (!r.bad && id < LAN_MAX_MEMBERS)
            store_snap(s, (int)id, &snap);
        break;
    }
    case H_PONG:
        client_pong(s, &r);
        break;
    default:
        break;
    }
}

static void client_tick(lan_session *s)
{
    uint32_t now = local_ms(s);
    if (!s->joined || !s->server || (int32_t)(now - s->next_clock_ping) < 0)
        return;
    {
        uint8_t buf[8];
        net_writer w;
        nw_init(&w, buf, sizeof buf);
        nw_u8(&w, C_PING); nw_u32(&w, now);
        send_buf(s->server, CH_UNRELIABLE, buf, w.len);
    }
    s->pings_sent++;
    s->next_clock_ping = now + (s->pings_sent < CLOCK_SAMPLES ? 100 : 1000);
}

/* ---------------------------------------------------------------- public API */

int lan_net_init(void)
{
    if (g_net_ready)
        return 0;
    if (enet_initialize() != 0)
        return -1;
    g_net_ready = 1;
    return 0;
}

void lan_net_shutdown(void)
{
    if (g_net_ready)
        enet_deinitialize();
    g_net_ready = 0;
}

static uint64_t random_token(void)
{
    uint64_t t = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&t, sizeof t, 1, f) != 1)
            t = 0;
        fclose(f);
    }
    if (!t) {
        static uint32_t counter;
        t = ((uint64_t)enet_time_get() << 32) ^ (uint64_t)(uintptr_t)&t ^ ((uint64_t)++counter << 48)
            ^ (uint64_t)rand();
    }
    return t ? t : 1;
}

static lan_session *new_session(const lan_config *cfg, const lan_profile *me)
{
    lan_session *s = (lan_session *)calloc(1, sizeof *s);
    if (!s)
        return NULL;
    if (cfg)
        s->cfg = *cfg;
    if (!s->cfg.port)
        s->cfg.port = LAN_DEFAULT_PORT;
    if (me)
        s->me = *me;
    sanitize_name(s->me.name);
    if (!s->me.level)
        s->me.level = 1;
    s->discovery = ENET_SOCKET_NULL;
    s->local_id = LAN_NO_PLAYER;
    s->token = random_token();
    return s;
}

lan_session *lan_host(const lan_config *cfg, const lan_profile *me, const lan_rules *rules)
{
    lan_session *s;
    ENetAddress a;
    lan_member *m;

    if (lan_net_init() != 0 || !(s = new_session(cfg, me)))
        return NULL;
    s->host = 1;
    a.host = ENET_HOST_ANY;
    a.port = s->cfg.port;
    s->enet = enet_host_create(&a, LAN_MAX_MEMBERS + 4, CH_COUNT, 0, 0);
    if (!s->enet) {
        logf_(s, "cannot listen on port %u", s->cfg.port);
        free(s);
        return NULL;
    }
    if (rules)
        s->lobby.rules = *rules;
    clamp_rules(&s->lobby.rules);
    m = &s->lobby.members[0];
    m->used = 1; m->id = 0; m->connected = 1; m->slot = LAN_NO_PLAYER;
    m->profile = s->me;
    m->profile.spectator = 0;
    apply_rules(&s->lobby.rules, &m->profile);
    s->local_id = 0;
    s->joined = 1;

    s->discovery = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (s->discovery != ENET_SOCKET_NULL) {
        ENetAddress d;
        d.host = ENET_HOST_ANY;
        d.port = LAN_DISCOVERY_PORT;
        enet_socket_set_option(s->discovery, ENET_SOCKOPT_REUSEADDR, 1);
        enet_socket_set_option(s->discovery, ENET_SOCKOPT_NONBLOCK, 1);
        if (enet_socket_bind(s->discovery, &d) < 0) {
            logf_(s, "LAN browser port %u busy: this lobby is joinable by address only", LAN_DISCOVERY_PORT);
            enet_socket_destroy(s->discovery);
            s->discovery = ENET_SOCKET_NULL;
        }
    }
    logf_(s, "lobby \"%s\" open on port %u", s->lobby.rules.lobby_name, s->cfg.port);
    s->lobby_dirty = 1;
    return s;
}

static int client_connect(lan_session *s)
{
    s->enet = enet_host_create(NULL, 1, CH_COUNT, 0, 0);
    if (!s->enet)
        return -1;
    s->server = enet_host_connect(s->enet, &s->server_addr, CH_COUNT, LAN_PROTOCOL_VERSION);
    if (!s->server) {
        enet_host_destroy(s->enet);
        s->enet = NULL;
        return -1;
    }
    enet_peer_timeout(s->server, 32, 3000, 6000);
    s->joined = s->kicked = s->dropped = 0;
    return 0;
}

lan_session *lan_join(const lan_config *cfg, const lan_profile *me, const char *address, uint16_t port)
{
    lan_session *s;
    if (lan_net_init() != 0 || !(s = new_session(cfg, me)))
        return NULL;
    if (enet_address_set_host(&s->server_addr, address) != 0) {
        logf_(s, "unknown host %s", address);
        free(s);
        return NULL;
    }
    s->server_addr.port = port ? port : LAN_DEFAULT_PORT;
    if (client_connect(s) != 0) {
        free(s);
        return NULL;
    }
    return s;
}

int lan_reconnect(lan_session *s)
{
    if (s->host)
        return -1;
    if (s->enet)
        enet_host_destroy(s->enet);
    s->enet = NULL;
    s->server = NULL;
    s->clock_count = 0;
    return client_connect(s);
}

void lan_drop(lan_session *s)
{
    if (s->host || !s->enet)
        return;
    enet_host_destroy(s->enet);                     /* no disconnect message: the host times out */
    s->enet = NULL;
    s->server = NULL;
    s->joined = 0;
    s->dropped = 1;
}

void lan_close(lan_session *s)
{
    if (!s)
        return;
    if (s->enet) {
        size_t i;
        if (s->host) {
            for (i = 0; i < s->enet->peerCount; i++)
                if (s->enet->peers[i].state == ENET_PEER_STATE_CONNECTED)
                    enet_peer_disconnect(&s->enet->peers[i], DISCONNECT_LEAVE);
        } else if (s->server) {
            enet_peer_disconnect(s->server, DISCONNECT_LEAVE);
        }
        enet_host_flush(s->enet);
        enet_host_destroy(s->enet);
    }
    if (s->discovery != ENET_SOCKET_NULL)
        enet_socket_destroy(s->discovery);
    free(s);
}

void lan_service(lan_session *s, uint32_t wait_ms)
{
    ENetEvent e;
    if (!s->enet)
        return;                                     /* a dropped client until lan_reconnect() */
    if (s->host) {
        host_tick(s);
        host_discovery(s);
        host_flush_lobby(s);
    } else {
        client_tick(s);
    }
    while (s->enet && enet_host_service(s->enet, &e, wait_ms) > 0) {
        wait_ms = 0;
        switch (e.type) {
        case ENET_EVENT_TYPE_CONNECT:
            e.peer->data = NULL;
            if (!s->host && e.peer == s->server)
                client_send_hello(s);
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            if (s->host)
                host_packet(s, e.peer, e.packet->data, e.packet->dataLength);
            else
                client_packet(s, e.packet->data, e.packet->dataLength);
            enet_packet_destroy(e.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            if (s->host) {
                host_lost(s, e.peer, e.data == DISCONNECT_LEAVE);
            } else if (e.peer == s->server) {
                s->server = NULL;
                s->joined = 0;
                if (!s->kicked) {
                    logf_(s, "lost the host");
                    push_event(s, LAN_EV_DISCONNECTED, LAN_NO_PLAYER, 0, 0, NULL);
                }
            }
            break;
        default:
            break;
        }
    }
    if (s->host)
        host_flush_lobby(s);
}

int lan_is_host(const lan_session *s) { return s->host; }
int lan_local_id(const lan_session *s) { return s->joined ? s->local_id : LAN_NO_PLAYER; }
const lan_lobby *lan_get_lobby(const lan_session *s) { return &s->lobby; }

void lan_set_profile(lan_session *s, const lan_profile *p)
{
    s->me = *p;
    sanitize_name(s->me.name);
    if (s->host) {
        lan_member *m = &s->lobby.members[0];
        m->profile = s->me;
        m->profile.spectator = 0;
        apply_rules(&s->lobby.rules, &m->profile);
        unique_name(&s->lobby, m->profile.name, 0);
        s->lobby_dirty = 1;
    } else if (s->joined) {
        uint8_t buf[160];
        net_writer w;
        nw_init(&w, buf, sizeof buf);
        nw_u8(&w, C_PROFILE);
        put_profile(&w, &s->me);
        send_buf(s->server, CH_RELIABLE, buf, w.len);
    }
}

void lan_set_ready(lan_session *s, int ready)
{
    if (s->host) {
        s->lobby.members[0].ready = ready ? 1 : 0;
        s->lobby_dirty = 1;
    } else if (s->joined) {
        uint8_t buf[2] = { C_READY, (uint8_t)(ready ? 1 : 0) };
        send_buf(s->server, CH_RELIABLE, buf, 2);
    }
}

void lan_chat(lan_session *s, const char *text)
{
    if (s->host) {
        member_chat(s, 0, text);
    } else if (s->joined) {
        uint8_t buf[256];
        net_writer w;
        nw_init(&w, buf, sizeof buf);
        nw_u8(&w, C_CHAT); nw_str(&w, text, LAN_CHAT_MAX);
        send_buf(s->server, CH_RELIABLE, buf, w.len);
    }
}

void lan_send_snapshot(lan_session *s, const lan_snapshot *snap)
{
    lan_snapshot v = *snap;
    if (!v.time)
        v.time = lan_time(s);
    if (s->host) {
        host_relay_snap(s, 0, &v);
    } else if (s->joined) {
        uint8_t buf[80];
        net_writer w;
        nw_init(&w, buf, sizeof buf);
        nw_u8(&w, C_SNAP);
        put_snap(&w, &v);
        send_buf(s->server, CH_UNRELIABLE, buf, w.len);
        store_snap(s, s->local_id, &v);
    }
}

void lan_finish(lan_session *s, uint32_t race_ms)
{
    if (s->host) {
        member_finish(s, 0, race_ms);
    } else if (s->joined) {
        uint8_t buf[8];
        net_writer w;
        nw_init(&w, buf, sizeof buf);
        nw_u8(&w, C_FINISH); nw_u32(&w, race_ms);
        send_buf(s->server, CH_RELIABLE, buf, w.len);
    }
}

static void nlerp(const float *a, const float *b, float t, float *out)
{
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], sign = d < 0 ? -1.0f : 1.0f, len = 0;
    int i;
    for (i = 0; i < 4; i++) {
        out[i] = a[i] + (b[i] * sign - a[i]) * t;
        len += out[i] * out[i];
    }
    if (len > 0) {
        float inv = 1.0f / sqrtf(len);
        for (i = 0; i < 4; i++)
            out[i] *= inv;
    }
}

int lan_remote_state(const lan_session *s, int player, uint32_t t, lan_snapshot *out)
{
    const snap_ring *r;
    int i, k;
    if (player < 0 || player >= LAN_MAX_MEMBERS)
        return 0;
    r = &s->snaps[player];
    if (!r->count)
        return 0;
    /* walk from newest to oldest for the first snapshot at or before t */
    for (k = 0; k < r->count; k++) {
        int idx = (r->head - k + SNAP_HISTORY) % SNAP_HISTORY;
        const lan_snapshot *a = &r->s[idx];
        if ((int32_t)(t - a->time) < 0)
            continue;
        if (k == 0) {                               /* newer than everything: extrapolate */
            uint32_t dt = t - a->time;
            float sec;
            if (dt > EXTRAPOLATE_MS)
                dt = EXTRAPOLATE_MS;
            sec = (float)dt / 1000.0f;
            *out = *a;
            for (i = 0; i < 3; i++)
                out->pos[i] += a->vel[i] * sec;
            out->time = t;
            return 1;
        }
        {
            const lan_snapshot *b = &r->s[(idx + 1) % SNAP_HISTORY];
            float f = b->time != a->time ? (float)(t - a->time) / (float)(b->time - a->time) : 0.0f;
            *out = f < 0.5f ? *a : *b;              /* discrete fields from the nearer one */
            for (i = 0; i < 3; i++) {
                out->pos[i] = a->pos[i] + (b->pos[i] - a->pos[i]) * f;
                out->vel[i] = a->vel[i] + (b->vel[i] - a->vel[i]) * f;
            }
            nlerp(a->rot, b->rot, f, out->rot);
            out->boost = a->boost + (b->boost - a->boost) * f;
            out->progress = a->progress + (b->progress - a->progress) * f;
            out->time = t;
            return 1;
        }
    }
    *out = r->s[(r->head - r->count + 1 + SNAP_HISTORY) % SNAP_HISTORY];   /* older than all: the oldest */
    return 1;
}

void lan_set_rules(lan_session *s, const lan_rules *rules)
{
    int i;
    if (!s->host)
        return;
    s->lobby.rules = *rules;
    clamp_rules(&s->lobby.rules);
    /* equal stats from now on; turning it off again takes effect as players send their profiles again */
    for (i = 0; i < LAN_MAX_MEMBERS; i++)
        if (s->lobby.members[i].used)
            apply_rules(&s->lobby.rules, &s->lobby.members[i].profile);
    s->lobby_dirty = 1;
}

void lan_kick(lan_session *s, int player)
{
    lan_member *m;
    char name[LAN_NAME_MAX + 1];
    if (!s->host || player <= 0 || player >= LAN_MAX_MEMBERS || !s->lobby.members[player].used)
        return;
    m = &s->lobby.members[player];
    snprintf(name, sizeof name, "%s", m->profile.name);
    if (s->nbanned < (int)(sizeof s->banned / sizeof s->banned[0]))
        s->banned[s->nbanned++] = s->hm[player].token;
    if (s->hm[player].peer)
        send_reject(s, s->hm[player].peer, H_KICK, LAN_REJECT_KICKED);
    remove_member(s, player);
    host_system(s, "%s was kicked", name);
}

int lan_start(lan_session *s, uint32_t delay_ms)
{
    int i, slot = 0;
    if (!s->host || s->lobby.state != LAN_STATE_LOBBY)
        return 0;
    for (i = 0; i < LAN_MAX_MEMBERS; i++) {
        const lan_member *m = &s->lobby.members[i];
        if (m->used && m->connected && !m->profile.spectator && !m->ready)
            return 0;
    }
    for (i = 0; i < LAN_MAX_MEMBERS; i++) {
        lan_member *m = &s->lobby.members[i];
        m->finished = 0; m->finish_ms = 0; m->place = 0;
        m->slot = m->used && m->connected && !m->profile.spectator && slot < s->lobby.rules.max_players
                  ? (uint8_t)slot++ : LAN_NO_PLAYER;
    }
    s->lobby.round++;
    s->lobby.state = LAN_STATE_COUNTDOWN;
    s->lobby.start_time = local_ms(s) + delay_ms;
    s->lobby_dirty = 1;
    logf_(s, "round %u: %d riders, GO in %u ms", s->lobby.round, slot, delay_ms);
    push_event(s, LAN_EV_START, 0, 0, s->lobby.start_time, NULL);
    return 1;
}

void lan_end_round(lan_session *s)
{
    int i;
    if (!s->host)
        return;
    for (i = 0; i < LAN_MAX_MEMBERS; i++) {
        s->lobby.members[i].ready = 0;
        s->lobby.members[i].slot = LAN_NO_PLAYER;
    }
    s->lobby.state = LAN_STATE_LOBBY;
    s->lobby_dirty = 1;
}

const char *lan_reject_text(int reason)
{
    switch (reason) {
    case LAN_REJECT_VERSION:   return "different LAN protocol version";
    case LAN_REJECT_GAME:      return "different game build";
    case LAN_REJECT_FULL:      return "lobby full";
    case LAN_REJECT_KICKED:    return "kicked by the host";
    case LAN_REJECT_RUNNING:   return "event in progress, no spectators";
    case LAN_REJECT_BAD_HELLO: return "bad handshake";
    default:                   return "none";
    }
}

/* ---------------------------------------------------------------- LAN browser */

struct lan_browser {
    ENetSocket sock;
    lan_found  found[32];
    int        count;
};

lan_browser *lan_browse_open(void)
{
    lan_browser *b;
    ENetAddress a;
    if (lan_net_init() != 0 || !(b = (lan_browser *)calloc(1, sizeof *b)))
        return NULL;
    b->sock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (b->sock == ENET_SOCKET_NULL) {
        free(b);
        return NULL;
    }
    a.host = ENET_HOST_ANY;
    a.port = 0;
    enet_socket_bind(b->sock, &a);
    enet_socket_set_option(b->sock, ENET_SOCKOPT_BROADCAST, 1);
    enet_socket_set_option(b->sock, ENET_SOCKOPT_NONBLOCK, 1);
    return b;
}

void lan_browse_query(lan_browser *b)
{
    /* the LAN, the loopback net (hosts on this machine, all of them get a broadcast) and 127.0.0.1 */
    static const char *targets[] = { "255.255.255.255", "127.255.255.255", "127.0.0.1" };
    uint8_t buf[8];
    net_writer w;
    ENetBuffer eb;
    size_t i;
    nw_init(&w, buf, sizeof buf);
    nw_u32(&w, 0x514C3353u); nw_u16(&w, LAN_PROTOCOL_VERSION);
    eb.data = buf; eb.dataLength = w.len;
    for (i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        ENetAddress a;
        if (enet_address_set_host_ip(&a, targets[i]) != 0)
            continue;
        a.port = LAN_DISCOVERY_PORT;
        enet_socket_send(b->sock, &a, &eb, 1);
    }
}

int lan_browse_poll(lan_browser *b, lan_found *out, int max)
{
    uint8_t buf[128];
    ENetBuffer eb;
    ENetAddress from;
    int n, i;
    for (;;) {
        net_reader r;
        lan_found f;
        eb.data = buf; eb.dataLength = sizeof buf;
        n = enet_socket_receive(b->sock, &from, &eb, 1);
        if (n <= 0)
            break;
        nr_init(&r, buf, (size_t)n);
        if (nr_u32(&r) != 0x524C3353u)
            continue;
        memset(&f, 0, sizeof f);
        f.version = (uint16_t)nr_u16(&r); f.game_id = nr_u32(&r); f.port = (uint16_t)nr_u16(&r);
        nr_str(&r, f.lobby_name, sizeof f.lobby_name);
        f.mode = (uint8_t)nr_u8(&r); f.state = (uint8_t)nr_u8(&r);
        f.players = (uint8_t)nr_u8(&r); f.max_players = (uint8_t)nr_u8(&r);
        if (r.bad)
            continue;
        enet_address_get_host_ip(&from, f.address, sizeof f.address);
        /* a lobby on this machine answers on loopback and on its LAN address: one entry, the LAN one */
        for (i = 0; i < b->count; i++)
            if (b->found[i].port == f.port && (!strcmp(b->found[i].address, f.address)
                    || ((!strncmp(f.address, "127.", 4) || !strncmp(b->found[i].address, "127.", 4))
                        && !strcmp(b->found[i].lobby_name, f.lobby_name))))
                break;
        if (i < b->count && !strncmp(f.address, "127.", 4) && strncmp(b->found[i].address, "127.", 4))
            continue;
        if (i == b->count) {
            if (b->count == (int)(sizeof b->found / sizeof b->found[0]))
                continue;
            b->count++;
        }
        b->found[i] = f;
    }
    for (i = 0; i < b->count && i < max; i++)
        out[i] = b->found[i];
    return i;
}

void lan_browse_close(lan_browser *b)
{
    if (!b)
        return;
    enet_socket_destroy(b->sock);
    free(b);
}
