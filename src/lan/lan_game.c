/*
 * LAN multiplayer – the net core (src/net) in the game: a race between game instances (phase 3, first cut).
 *
 * No lobby UI yet: the session comes from the environment.
 *   SSX3_LAN_HOST=1               open a lobby (port SSX3_LAN_PORT, default 27215)
 *   SSX3_LAN_JOIN=<addr>[:port]   join one (retried every 2 s until the host answers)
 *   SSX3_LAN_NAME=<name>          name shown to the others (default: the save's profile name)
 *   SSX3_LAN_PLAYERS=<n>          host: start once n players (with the host) are ready (default 2)
 * Every player picks the same event in the game's own menus (Single Event, same peak and course); the
 * lobby rules do not drive the menus yet.
 *
 * Flow:
 *   - A network thread services the session every 2 ms; the game's hooks take the same lock briefly.
 *   - Countdown hold (race.c, PreRace -> Countdown): the first held frame reads this PC's profile (name,
 *     character, attributes, equipped gear: docs/notes.md, spike 6) and reports "ready". The host starts the
 *     round once SSX3_LAN_PLAYERS are ready; everyone releases the hold at the host's start time, so the
 *     180-frame countdown runs in step on every PC.
 *   - Slots: our rider is game slot 0 everywhere. The others take game slots 1.. in their lobby slot order;
 *     those AI slots become puppets (puppet.c) whose pose comes from the remote rider's snapshots, shown 100 ms
 *     in the past. AI riders in the remaining slots are local to each PC (not synced yet: AI fill-in by the
 *     host is still to do).
 *   - Our rider's pose (rider+0x180) goes out at 30 Hz from the rider-manager update.
 *   - Finish: our time goes to the host; a puppet that crosses the line takes its player's own time from
 *     the lobby when it is there already.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#include "net.h"
#include "lan_game.h"

extern ptrdiff_t g_xbox_mem_offset;
extern uint32_t g_ssx3_xbe_hash;                        /* main.c */

#define M8(va)  (*(volatile uint8_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define M16(va) (*(volatile uint16_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define M32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define MF(va)  (*(volatile float *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* Profile (docs/notes.md, spike 6): rider records of 0xF88 bytes per (player·10 + character) */
#define PROFILE_RECORDS  0x003469E4u
#define RECORD_SIZE      0xF88u
#define REC_GEAR_COUNT   0x298u
#define REC_GEAR         0x29Cu
#define REC_ATTRIB       0xBEBu
#define GEAR_EQUIPPED    0x10u
#define CHAR_SLOT_INDEX  0x00346400u     /* player -> index into the slot table */
#define CHAR_SLOT_TABLE  0x0034595Cu     /* + index·0x20: character */
#define RIDER_POSE       0x180u
#define RENDER_DELAY_MS  100u

#if defined(_WIN32)
static CRITICAL_SECTION s_lock;
#define LOCK()   EnterCriticalSection(&s_lock)
#define UNLOCK() LeaveCriticalSection(&s_lock)
#else
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()   pthread_mutex_lock(&s_lock)
#define UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

static int          s_state;                    /* 0 not set up, 1 active, -1 off */
static lan_session *s_session;
static char         s_join_addr[128];
static uint16_t     s_join_port;
static lan_config   s_cfg;
static lan_profile  s_profile;
static int          s_want_players = 2;
static int          s_profile_sent;
static uint32_t     s_next_retry;
static int          s_slot_member[6];           /* game slot -> lobby member id, -1 = local AI */
static uint8_t      s_mapped_round;
static uint32_t     s_last_send;
static float        s_last_pos[3];
static uint32_t     s_last_pos_time;
static int          s_holding;

static void sleep_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts = { 0, (long)ms * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static void open_session(void)
{
    if (s_join_addr[0]) {
        s_session = lan_join(&s_cfg, &s_profile, s_join_addr, s_join_port);
        if (s_session)
            fprintf(stderr, "  [LAN] joining %s:%u\n", s_join_addr, s_join_port ? s_join_port : LAN_DEFAULT_PORT);
    } else {
        lan_rules r;
        memset(&r, 0, sizeof r);
        snprintf(r.lobby_name, sizeof r.lobby_name, "%s's race", s_profile.name);
        r.mode = LAN_MODE_RACE;
        r.max_players = 6;
        r.leveled = 1;
        r.spectators = 1;
        r.bragging = 1;
        s_session = lan_host(&s_cfg, &s_profile, &r);
    }
}

/* Game slots for the round that just started: the others in lobby slot order behind our slot 0. */
static void map_slots(const lan_lobby *l)
{
    int i, me = lan_local_id(s_session), next = 1, k;
    for (i = 0; i < 6; i++)
        s_slot_member[i] = -1;
    for (k = 0; k < LAN_MAX_PLAYERS; k++)             /* lobby slot k */
        for (i = 0; i < LAN_MAX_MEMBERS; i++)
            if (l->members[i].used && l->members[i].slot == k && i != me && next < 6)
                s_slot_member[next++] = i;
    s_mapped_round = l->round;
    for (i = 1; i < next; i++)
        fprintf(stderr, "  [LAN] game slot %d = %s (player %d)\n", i,
                l->members[s_slot_member[i]].profile.name, s_slot_member[i]);
}

static void handle_events(void)
{
    lan_event e;
    const lan_lobby *l = lan_get_lobby(s_session);
    while (lan_next_event(s_session, &e)) {
        switch (e.type) {
        case LAN_EV_CHAT:
            fprintf(stderr, "  [LAN] %s%s%s\n", e.player == LAN_NO_PLAYER ? "" : l->members[e.player].profile.name,
                    e.player == LAN_NO_PLAYER ? "" : ": ", e.text);
            break;
        case LAN_EV_START:
            map_slots(l);
            fprintf(stderr, "  [LAN] round %u: countdown starts in %d ms\n", l->round,
                    (int)(int32_t)(e.time - lan_time(s_session)));
            break;
        case LAN_EV_REJECTED:
            fprintf(stderr, "  [LAN] refused: %s\n", lan_reject_text(e.reason));
            break;
        case LAN_EV_DISCONNECTED:
            if (!lan_is_host(s_session))
                s_next_retry = lan_time(s_session) + 2000;
            break;
        default:
            break;
        }
    }
}

static void host_autostart(void)
{
    const lan_lobby *l = lan_get_lobby(s_session);
    int i, ready = 0;
    if (!lan_is_host(s_session) || l->state != LAN_STATE_LOBBY)
        return;
    for (i = 0; i < LAN_MAX_MEMBERS; i++)
        if (l->members[i].used && l->members[i].connected && !l->members[i].profile.spectator) {
            if (!l->members[i].ready)
                return;
            ready++;
        }
    if (ready >= s_want_players && lan_start(s_session, 1500))
        fprintf(stderr, "  [LAN] %d players ready, starting\n", ready);
}

#if defined(_WIN32)
static DWORD WINAPI net_thread(LPVOID arg)
#else
static void *net_thread(void *arg)
#endif
{
    (void)arg;
    for (;;) {
        LOCK();
        if (s_session) {
            lan_service(s_session, 0);
            handle_events();
            host_autostart();
            if (!lan_is_host(s_session) && lan_local_id(s_session) == LAN_NO_PLAYER && s_next_retry
                && (int32_t)(lan_time(s_session) - s_next_retry) >= 0) {
                s_next_retry = 0;
                lan_reconnect(s_session);
            }
        }
        UNLOCK();
        sleep_ms(2);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static int setup(void)
{
    const char *host = getenv("SSX3_LAN_HOST"), *join = getenv("SSX3_LAN_JOIN"), *e;
    if (s_state)
        return s_state > 0;
    s_state = -1;
    if (!(host && atoi(host)) && !(join && join[0]))
        return 0;
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.game_id = g_ssx3_xbe_hash ^ (LAN_PROTOCOL_VERSION * 0x9E3779B1u);
    if ((e = getenv("SSX3_LAN_PORT")) != NULL)
        s_cfg.port = (uint16_t)atoi(e);
    if ((e = getenv("SSX3_LAN_PLAYERS")) != NULL && atoi(e) >= 1)
        s_want_players = atoi(e);
    if (join && join[0]) {
        char *colon;
        snprintf(s_join_addr, sizeof s_join_addr, "%s", join);
        if ((colon = strrchr(s_join_addr, ':')) != NULL) {
            *colon = 0;
            s_join_port = (uint16_t)atoi(colon + 1);
        }
    }
    memset(&s_profile, 0, sizeof s_profile);
    snprintf(s_profile.name, sizeof s_profile.name, "%s", (e = getenv("SSX3_LAN_NAME")) ? e : "Rider");
    if (lan_net_init() != 0)
        return 0;
#if defined(_WIN32)
    InitializeCriticalSection(&s_lock);
#endif
    LOCK();
    open_session();
    UNLOCK();
    if (!s_session) {
        fprintf(stderr, "  [LAN] no session\n");
        return 0;
    }
    {
#if defined(_WIN32)
        CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
#else
        pthread_t t;
        pthread_create(&t, NULL, net_thread, NULL);
        pthread_detach(t);
#endif
    }
    s_state = 1;
    fprintf(stderr, "  [LAN] %s, game id %08X\n", s_join_addr[0] ? "client" : "host", s_cfg.game_id);
    return 1;
}

int lan_game_active(void)
{
    return setup();
}

/* This PC's rider as the save has it: player 0's character, its attributes and equipped gear. */
static void read_profile(lan_profile *p)
{
    uint32_t ch = M8(CHAR_SLOT_TABLE + M8(CHAR_SLOT_INDEX) * 0x20u), rec, n, i;
    char name[16];
    if (!getenv("SSX3_LAN_NAME")) {
        for (i = 0; i < 15; i++)
            name[i] = (char)M8(PROFILE_RECORDS + i);
        name[15] = 0;
        if (name[0])
            snprintf(p->name, sizeof p->name, "%s", name);
    }
    if (ch > 9)
        return;
    p->character = (uint8_t)ch;
    rec = PROFILE_RECORDS + ch * RECORD_SIZE;
    for (i = 0; i < LAN_ATTRIBS; i++)
        p->attrib[i] = M8(rec + REC_ATTRIB + i);
    n = M32(rec + REC_GEAR_COUNT);
    p->gear_count = 0;
    for (i = 0; i < n && i < 0x20D && p->gear_count < LAN_GEAR_MAX; i++)
        if (M16(rec + REC_GEAR + 4 * i + 2) & GEAR_EQUIPPED)
            p->gear[p->gear_count++] = M16(rec + REC_GEAR + 4 * i);
}

int lan_game_hold(void)
{
    const lan_lobby *l;
    int hold = 1, me;
    if (!setup())
        return -1;
    LOCK();
    l = lan_get_lobby(s_session);
    me = lan_local_id(s_session);
    if (!s_holding) {
        s_holding = 1;
        if (!s_profile_sent) {
            read_profile(&s_profile);
            s_profile_sent = 1;
            fprintf(stderr, "  [LAN] profile: %s, character %u, attributes %u %u %u %u %u %u %u, %u items\n",
                    s_profile.name, s_profile.character, s_profile.attrib[0], s_profile.attrib[1],
                    s_profile.attrib[2], s_profile.attrib[3], s_profile.attrib[4], s_profile.attrib[5],
                    s_profile.attrib[6], s_profile.gear_count);
        }
        lan_set_profile(s_session, &s_profile);
        if (lan_is_host(s_session) && l->state == LAN_STATE_RESULTS)
            lan_end_round(s_session);                   /* a new race from the results: back to the lobby */
        fprintf(stderr, "  [LAN] at the start, waiting for the others\n");
    }
    if (me != LAN_NO_PLAYER) {
        if (l->state == LAN_STATE_LOBBY && !l->members[me].ready)
            lan_set_ready(s_session, 1);                /* again after a reconnect or a new round */
        if ((l->state == LAN_STATE_COUNTDOWN || l->state == LAN_STATE_RUNNING)
            && l->members[me].slot != LAN_NO_PLAYER && (int32_t)(lan_time(s_session) - l->start_time) >= 0) {
            if (s_mapped_round != l->round)
                map_slots(l);
            hold = 0;
            s_holding = 0;
            fprintf(stderr, "  [LAN] GO (host clock %u)\n", lan_time(s_session));
        }
    }
    UNLOCK();
    return hold;
}

static uint32_t rider_manager(void)
{
    uint32_t game = M32(0x00369534u), world;
    if (game < 0x10000u || (world = M32(game + 0x74)) < 0x10000u)
        return 0;
    return M32(world + 0x0C);
}

static int race_running(const lan_lobby *l)
{
    return l->state == LAN_STATE_COUNTDOWN || l->state == LAN_STATE_RUNNING || l->state == LAN_STATE_RESULTS;
}

int lan_game_puppet_member(uint32_t rider)
{
    uint32_t mgr;
    int slot, member = -1;
    if (s_state <= 0 || rider < 0x10000u || (mgr = rider_manager()) < 0x10000u)
        return -1;
    for (slot = 1; slot < 6 && slot < (int)M32(mgr + 0x78); slot++)
        if (M32(mgr + 0x28 + 4 * slot) == rider) {
            LOCK();
            if (race_running(lan_get_lobby(s_session)))
                member = s_slot_member[slot];
            UNLOCK();
            break;
        }
    return member;
}

int lan_game_pose(uint32_t rider)
{
    int member = lan_game_puppet_member(rider), ok, i;
    lan_snapshot sn;
    if (member < 0)
        return 0;
    LOCK();
    ok = lan_remote_state(s_session, member, lan_time(s_session) - RENDER_DELAY_MS, &sn);
    UNLOCK();
    if (!ok)
        return 1;                                       /* nothing yet: stays where the grid put it */
    for (i = 0; i < 3; i++)
        MF(rider + RIDER_POSE + 4 * i) = sn.pos[i];
    MF(rider + RIDER_POSE + 12) = 1.0f;
    for (i = 0; i < 4; i++)
        MF(rider + RIDER_POSE + 16 + 4 * i) = sn.rot[i];
    return 1;
}

void lan_game_tick(uint32_t mgr)
{
    uint32_t rider, now;
    lan_snapshot sn;
    int i;
    if (s_state <= 0 || mgr < 0x10000u || M32(mgr + 0x78) < 1)
        return;
    rider = M32(mgr + 0x28);                            /* game slot 0: our rider */
    if (rider < 0x10000u)
        return;
    LOCK();
    now = lan_time(s_session);
    if (lan_local_id(s_session) != LAN_NO_PLAYER && race_running(lan_get_lobby(s_session))
        && now - s_last_send >= 33) {
        memset(&sn, 0, sizeof sn);
        sn.time = now;
        for (i = 0; i < 3; i++)
            sn.pos[i] = MF(rider + RIDER_POSE + 4 * i);
        for (i = 0; i < 4; i++)
            sn.rot[i] = MF(rider + RIDER_POSE + 16 + 4 * i);
        if (s_last_pos_time && now != s_last_pos_time)    /* units per second, for extrapolation */
            for (i = 0; i < 3; i++)
                sn.vel[i] = (sn.pos[i] - s_last_pos[i]) * 1000.0f / (float)(now - s_last_pos_time);
        for (i = 0; i < 3; i++)
            s_last_pos[i] = sn.pos[i];
        s_last_pos_time = now;
        s_last_send = now;
        lan_send_snapshot(s_session, &sn);
    }
    UNLOCK();
}

uint32_t lan_game_finish(uint32_t slot, uint32_t frames)
{
    uint32_t out = frames;
    if (s_state <= 0)
        return frames;
    LOCK();
    if (slot == 0) {
        lan_finish(s_session, frames * 1000u / 60u);
        fprintf(stderr, "  [LAN] we finished: %u.%02u s\n", frames / 60u, frames % 60u * 100u / 60u);
    } else if (slot < 6 && s_slot_member[slot] >= 0) {
        const lan_member *m = &lan_get_lobby(s_session)->members[s_slot_member[slot]];
        if (m->used && m->finished) {
            out = m->finish_ms * 60u / 1000u;
            fprintf(stderr, "  [LAN] %s finished: %u frames (own clock; locally %u)\n", m->profile.name, out, frames);
        }
    }
    UNLOCK();
    return out;
}
