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
 *   - Animation: a rider's brain (rider+0x74C) fills an 8-byte control input each frame (vtable[0], from the
 *     pad or the AI) and hands it to its current control mode (0x30970: riding, air, grind ...): dword 0 bits
 *     26-31 / 20-25 = left stick, bits 8-19 buttons, dword 1 another axis. Our rider's input goes out with
 *     every snapshot with the control mode it was made for; a puppet's brain keeps running, and 0x30970 gets
 *     the remote player's input instead of its AI's whenever the puppet is in that same mode (the bits mean
 *     different things per mode), so crouch, carve, jumps and tricks animate as on the other PC, while the pose (0x24D50) still
 *     comes from the network.
 *   - End: the game ends the race once its humans are through (0x12AA0); lan_game_wait_end() keeps it going
 *     until every remote player in it has finished on their own PC (or left) and their puppet has crossed the
 *     line here (up to 3 s later, it rides 100 ms behind), so the results list real times, not estimates.
 *     After 120 s of waiting the race ends anyway. The results themselves are finalized by the mode handler
 *     when our rider finishes (riders still on the course get an estimate, 0x22E50); a remote player's
 *     puppet that crosses later gets its real time into the results (0x369408[slot]) and the list is ranked
 *     again (0xCE140).
 *   - Look: the network thread re-reads our profile every 500 ms (character = race slot 0 in the slot table
 *     0x367FD0 + index·0x20: +0 name[12], +0xC owner, +0x10 flags, +0x14 character; index = 0x346400[slot])
 *     and sends it when it changed, so the others have it before the race loads. When the race creates its
 *     AI riders (0x13C60, which reads each slot's character), lan_game_before_ai() writes the remote players'
 *     characters and names into their slots; the results screen asks 0x4DC00 for an AI row's character name,
 *     and lan_game_result_name() hands it the slot's name instead. Gear is sent but not applied yet.
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
extern void sub_000CE140(void);                         /* rank the results by 0x369408, stdcall (count), ret 4 */

#if defined(_MSC_VER)
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp;
#else
extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp;
#endif

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
#define SLOT_INDEX       0x00346400u     /* game slot -> index into the race slot table (dwords) */
#define SLOT_TABLE       0x00367FD0u     /* + index·0x20: +0 name[12], +0x14 character */
#define SLOT_NAME_MAX    11u
#define RIDER_POSE       0x180u
#define RENDER_DELAY_MS  100u
#define BRAIN_MODE       0xE64u          /* rider brain (rider+0x74C): current control mode, 0x30970 */

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
static uint32_t     s_next_profile_poll;
static uint8_t      s_crossed[6];               /* game slot: the puppet crossed the line here */
static uint32_t     s_wait_since, s_seen_finished[6];
static int          s_we_finished;              /* our rider is through: the results are finalized */
static uint32_t     s_local_input[2];           /* our rider's control input this frame */
static uint32_t     s_local_mode;               /* ... and the control mode it was made for (brain+0xE64) */

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
    for (i = 0; i < 6; i++) {
        s_slot_member[i] = -1;
        s_crossed[i] = 0;
        s_seen_finished[i] = 0;
    }
    s_wait_since = 0;
    s_we_finished = 0;
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

static void read_profile(lan_profile *p);

/* Our profile as the game has it now; sent whenever it changes (character select, Load game ...). */
static void poll_profile(void)
{
    lan_profile p = s_profile;
    uint32_t now = lan_time(s_session);
    if ((int32_t)(now - s_next_profile_poll) < 0)
        return;
    s_next_profile_poll = now + 500;
    read_profile(&p);
    if (memcmp(&p, &s_profile, sizeof p) != 0 || !s_profile_sent) {
        s_profile = p;
        s_profile_sent = 1;
        lan_set_profile(s_session, &s_profile);
        fprintf(stderr, "  [LAN] profile: %s, character %u, attributes %u %u %u %u %u %u %u, %u items\n",
                s_profile.name, s_profile.character, s_profile.attrib[0], s_profile.attrib[1],
                s_profile.attrib[2], s_profile.attrib[3], s_profile.attrib[4], s_profile.attrib[5],
                s_profile.attrib[6], s_profile.gear_count);
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
            if (lan_local_id(s_session) != LAN_NO_PLAYER)
                poll_profile();
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
    uint32_t ch = M8(SLOT_TABLE + M32(SLOT_INDEX) * 0x20u + 0x14u), rec, n, i;
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
        sn.input[0] = s_local_input[0];
        sn.input[1] = s_local_input[1];
        sn.flags = (uint16_t)(s_local_mode & 0xFF);     /* the control mode the input belongs to */
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
        s_we_finished = 1;
        lan_finish(s_session, frames * 1000u / 60u);
        fprintf(stderr, "  [LAN] we finished: %u.%02u s\n", frames / 60u, frames % 60u * 100u / 60u);
    } else if (slot < 6 && s_slot_member[slot] >= 0) {
        const lan_member *m = &lan_get_lobby(s_session)->members[s_slot_member[slot]];
        s_crossed[slot] = 1;
        if (m->used && m->finished) {
            out = m->finish_ms * 60u / 1000u;
            fprintf(stderr, "  [LAN] %s finished: %u frames (own clock; locally %u)\n", m->profile.name, out, frames);
        }
    }
    UNLOCK();
    return out;
}

/* The players who will ride with us, in lobby slot order (= member id order, as lan_start() assigns them). */
static int expected_others(int *members, int max)
{
    const lan_lobby *l = lan_get_lobby(s_session);
    int i, n = 0, me = lan_local_id(s_session);
    for (i = 0; i < LAN_MAX_MEMBERS && n < max; i++)
        if (l->members[i].used && l->members[i].connected && !l->members[i].profile.spectator && i != me)
            members[n++] = i;
    return n;
}

void lan_game_before_ai(uint32_t mgr)
{
    int others[5], n, k;
    uint32_t first;
    if (mgr < 0x10000u)
        return;
    first = M32(mgr + 0x78);                            /* the AI riders are created from this slot on */
    if (!setup())
        return;
    LOCK();
    n = expected_others(others, 5);
    for (k = 0; k < 6; k++)
        s_slot_member[k] = -1;
    for (k = 0; k < n && first + (uint32_t)k < 6; k++) {
        uint32_t slot = first + (uint32_t)k, idx = M32(SLOT_INDEX + 4 * slot), e = SLOT_TABLE + idx * 0x20u, i;
        const lan_profile *p = &lan_get_lobby(s_session)->members[others[k]].profile;
        if (idx > 5)
            continue;
        s_slot_member[slot] = others[k];
        if (p->character <= 9)
            M8(e + 0x14) = p->character;
        for (i = 0; i < SLOT_NAME_MAX && p->name[i]; i++)
            M8(e + i) = (uint8_t)p->name[i];
        M8(e + i) = 0;
        fprintf(stderr, "  [LAN] slot %u: %s, character %u\n", slot, p->name, p->character);
    }
    UNLOCK();
}

uint32_t lan_game_result_name(uint32_t slot)
{
    uint32_t va = 0;
    if (s_state <= 0 || slot >= 6)
        return 0;
    LOCK();
    if (s_slot_member[slot] >= 0)
        va = SLOT_TABLE + M32(SLOT_INDEX + 4 * slot) * 0x20u;
    UNLOCK();
    return va;
}

int lan_game_wait_end(void)
{
    const lan_lobby *l;
    uint32_t now;
    int slot, waiting = 0;
    if (s_state <= 0)
        return 0;
    LOCK();
    l = lan_get_lobby(s_session);
    now = lan_time(s_session);
    if (!s_wait_since)
        s_wait_since = now | 1;
    for (slot = 1; slot < 6; slot++) {
        const lan_member *m;
        if (s_slot_member[slot] < 0 || s_crossed[slot])
            continue;
        m = &l->members[s_slot_member[slot]];
        if (!m->used || !m->connected)
            continue;                                   /* left: the game estimates its time */
        if (!m->finished) {
            waiting++;                                  /* still riding on its own PC */
            continue;
        }
        if (!s_seen_finished[slot])
            s_seen_finished[slot] = now | 1;
        if ((int32_t)(now - s_seen_finished[slot]) < 3000)
            waiting++;                                  /* finished there, its puppet is about to cross here */
    }
    if (waiting && (int32_t)(now - s_wait_since) > 120000) {
        fprintf(stderr, "  [LAN] gave up waiting for %d player(s)\n", waiting);
        waiting = 0;
    }
    if (!waiting)
        s_wait_since = 0;
    UNLOCK();
    return waiting > 0;
}

void lan_game_after_finish(uint32_t slot)
{
    uint32_t game, world, mgr, rider, eax, ecx, edx;
    int redo;
    if (s_state <= 0 || slot == 0 || slot >= 6)
        return;
    LOCK();
    redo = s_slot_member[slot] >= 0 && s_we_finished;
    UNLOCK();
    if (!redo || (game = M32(0x00369534u)) < 0x10000u || (world = M32(game + 0x74)) < 0x10000u
        || (mgr = M32(world + 0x0C)) < 0x10000u || (rider = M32(mgr + 0x28 + 4 * slot)) < 0x10000u)
        return;
    M32(0x00369408u + 4 * slot) = M32(rider + 0x4D8);   /* its real time instead of the estimate */
    eax = g_eax; ecx = g_ecx; edx = g_edx;
    g_esp -= 8;
    M32(g_esp + 4) = 10;                                /* 0xCE140(10), as the mode handler calls it */
    M32(g_esp) = 0;                                     /* return address: its ret 4 pops it and the argument */
    sub_000CE140();
    g_eax = eax; g_ecx = ecx; g_edx = edx;
    fprintf(stderr, "  [LAN] results: slot %u now %u frames, place %u\n", slot, M32(0x00369408u + 4 * slot),
            M32(0x00369458u + 4 * slot) + 1);
}

void lan_game_input(uint32_t brain, uint32_t input)
{
    uint32_t game, world, mgr, slot, n;
    if (s_state <= 0 || brain < 0x10000u || input < 0x10000u || (game = M32(0x00369534u)) < 0x10000u
        || (world = M32(game + 0x74)) < 0x10000u || (mgr = M32(world + 0x0C)) < 0x10000u)
        return;
    n = M32(mgr + 0x78);
    for (slot = 0; slot < n && slot < 6; slot++) {
        uint32_t rider = M32(mgr + 0x28 + 4 * slot);
        if (rider < 0x10000u || M32(rider + 0x74C) != brain)
            continue;
        if (slot == 0) {                                /* ours: goes out with the next snapshot */
            s_local_input[0] = M32(input);
            s_local_input[1] = M32(input + 4);
            s_local_mode = M32(brain + BRAIN_MODE);
        } else {
            int member;
            lan_snapshot sn;
            LOCK();
            member = race_running(lan_get_lobby(s_session)) ? s_slot_member[slot] : -1;
            /* The input's bits mean different things per control mode (dword 0 byte 2: buttons when riding,
             * the trick number in mode 5, used unchecked as an index into the trick table 0x285590). The
             * puppet runs 100 ms behind and can be in another mode than the input was made for: then a
             * button pattern became a trick number, a wild animation id and a crash (seen in 0x1A7960).
             * So the remote input is only used in the same mode; otherwise the AI's own input stays. */
            if (member >= 0 && lan_remote_state(s_session, member, lan_time(s_session) - RENDER_DELAY_MS, &sn)
                && (sn.flags & 0xFF) == (M32(brain + BRAIN_MODE) & 0xFF)) {
                M32(input) = sn.input[0];               /* the remote player's input, not its AI's */
                M32(input + 4) = sn.input[1];
            }
            UNLOCK();
        }
        return;
    }
}
