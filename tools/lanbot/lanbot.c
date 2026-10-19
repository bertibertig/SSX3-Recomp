/*
 * ssx3_lanbot – headless LAN test client for the net core (docs/lan-multiplayer.md, phase 1).
 *
 *   ssx3_lanbot selftest                    host + 11 fake players + extras in one process, scripted checks
 *   ssx3_lanbot host [options]              open a lobby and print what happens
 *   ssx3_lanbot bots N ADDRESS[:PORT] [options]   N fake players join, get ready, send snapshots
 *   ssx3_lanbot browse                      list the lobbies on the LAN
 *
 * Options: --port P, --name NAME, --mode race|freeroam, --max N, --seconds S (0 = until killed),
 *          --start MS (host: start when everyone is ready, GO after MS), --game-id X, --quiet
 *
 * The game id stands in for the game build + XBE identity the real client sends; it must match the host's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <enet/enet.h>

#include "net.h"

#define DEFAULT_GAME_ID 0x4541003Cu     /* SSX 3 PAL title id; the game will send a build hash */

typedef struct {
    lan_session *s;
    int          joined, kicked, rejected, reason, disconnected, starts, results, chats;
    uint32_t     start_time;
} bot;

static void drain(bot *b, int verbose, const char *tag)
{
    lan_event e;
    while (lan_next_event(b->s, &e)) {
        switch (e.type) {
        case LAN_EV_JOINED:       b->joined = 1; break;
        case LAN_EV_REJECTED:     b->rejected = 1; b->reason = e.reason; break;
        case LAN_EV_KICKED:       b->kicked = 1; break;
        case LAN_EV_DISCONNECTED: b->disconnected++; break;
        case LAN_EV_START:        b->starts++; b->start_time = e.time; break;
        case LAN_EV_RESULTS:      b->results++; break;
        case LAN_EV_CHAT:         b->chats++; break;
        default: break;
        }
        if (verbose && e.type == LAN_EV_CHAT)
            printf("%s chat [%s] %s\n", tag, e.player == LAN_NO_PLAYER ? "*" : "player", e.text);
        else if (verbose && e.type == LAN_EV_START)
            printf("%s start: GO at %u (now %u)\n", tag, e.time, lan_time(b->s));
        else if (verbose && e.type == LAN_EV_REJECTED)
            printf("%s rejected: %s\n", tag, lan_reject_text(e.reason));
    }
}

/* ---------------------------------------------------------------- selftest */

#define NBOTS 11

static bot g_host, g_bots[NBOTS + 3];
static int g_nbots, g_failures;
static lan_config g_cfg;

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        g_failures++;
}

/* Services everything for `ms`, sending a snapshot from every rider at about 30 Hz. */
static void pump(uint32_t ms, int snaps)
{
    uint32_t end = enet_time_get() + ms, next_snap = 0;
    int i;
    while ((int32_t)(enet_time_get() - end) < 0) {
        uint32_t now = enet_time_get();
        if (snaps && (int32_t)(now - next_snap) >= 0) {
            next_snap = now + 33;
            for (i = -1; i < g_nbots; i++) {
                bot *b = i < 0 ? &g_host : &g_bots[i];
                lan_snapshot sn;
                int id;
                if (!b->s || (id = lan_local_id(b->s)) == LAN_NO_PLAYER)
                    continue;
                memset(&sn, 0, sizeof sn);
                /* rider `id` rides along x at 10 units/s from x = id * 100 */
                sn.time = lan_time(b->s);
                sn.pos[0] = (float)id * 100.0f + (float)(sn.time % 100000u) / 100.0f;
                sn.pos[1] = (float)id;
                sn.rot[3] = 1.0f;
                sn.vel[0] = 10.0f;
                sn.anim = (uint16_t)id;
                lan_send_snapshot(b->s, &sn);
            }
        }
        lan_service(g_host.s, 1);
        drain(&g_host, 0, "host");
        for (i = 0; i < g_nbots; i++)
            if (g_bots[i].s) {
                lan_service(g_bots[i].s, 0);
                drain(&g_bots[i], 0, "bot");
            }
    }
}

static int all_bots(int (*pred)(const bot *), int from, int to)
{
    int i;
    for (i = from; i < to; i++)
        if (!pred(&g_bots[i]))
            return 0;
    return 1;
}

static int is_joined(const bot *b) { return b->joined; }
static int is_synced(const bot *b) { return lan_clock_synced(b->s); }

static lan_session *join_bot(const char *name, int spectator, uint32_t game_id, int skew)
{
    lan_profile p;
    lan_config c = g_cfg;
    memset(&p, 0, sizeof p);
    snprintf(p.name, sizeof p.name, "%s", name);
    p.character = 3;
    p.level = 7;
    memset(p.attrib, LAN_ATTRIB_BASE, sizeof p.attrib);
    p.attrib[4] = 9;                    /* like the user's Zoe: 4 points in attribute 4 */
    p.spectator = (uint8_t)spectator;
    c.game_id = game_id;
    c.clock_skew_ms = skew;
    return lan_join(&c, &p, "127.0.0.1", g_cfg.port);
}

static int selftest(void)
{
    lan_profile hp;
    lan_rules rules;
    const lan_lobby *l;
    int i, players = 0, ok, max_err;
    uint32_t start;
    char what[128];

    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.port = 27315;                 /* off the default port, so a running lobby does not interfere */
    g_cfg.game_id = DEFAULT_GAME_ID;
    g_cfg.quiet = getenv("LANBOT_VERBOSE") == NULL;

    memset(&hp, 0, sizeof hp);
    strcpy(hp.name, "Host");
    memset(&rules, 0, sizeof rules);
    strcpy(rules.lobby_name, "selftest");
    rules.mode = LAN_MODE_FREEROAM;
    rules.max_players = 12;
    rules.spectators = 1;
    rules.leveled = 0;                  /* everyone at level 1: the host must reset the bots' level 7 */
    g_host.s = lan_host(&g_cfg, &hp, &rules);
    if (!g_host.s) {
        printf("cannot open the host\n");
        return 2;
    }

    printf("11 players join (all named \"Zoe\", clocks skewed by -5 .. +5 s)\n");
    for (i = 0; i < NBOTS; i++)
        g_bots[i].s = join_bot("Zoe", 0, DEFAULT_GAME_ID, (i - 5) * 1000);
    g_nbots = NBOTS;
    pump(1500, 0);
    check(all_bots(is_joined, 0, NBOTS), "all 11 got a welcome");
    l = lan_get_lobby(g_host.s);
    for (i = 0; i < LAN_MAX_MEMBERS; i++)
        players += l->members[i].used && !l->members[i].profile.spectator;
    check(players == 12, "the host lists 12 players");
    l = lan_get_lobby(g_bots[10].s);
    ok = 1;
    for (i = 1; i <= NBOTS; i++) {
        char want[16];
        if (i == 1) strcpy(want, "Zoe"); else snprintf(want, sizeof want, "Zoe%d", i);
        if (!l->members[i].used || strcmp(l->members[i].profile.name, want))
            ok = 0;
    }
    check(ok, "a client sees the same list, names made unique (Zoe, Zoe2 .. Zoe11)");
    check(l->members[4].profile.level == 1 && l->members[4].profile.attrib[4] == LAN_ATTRIB_BASE,
          "equal stats rule: a leveled rider joins at level 1, one bar everywhere");

    printf("more members\n");
    g_bots[NBOTS].s = join_bot("Late", 0, DEFAULT_GAME_ID, 0);
    g_bots[NBOTS + 1].s = join_bot("Old build", 0, DEFAULT_GAME_ID + 1, 0);
    g_nbots = NBOTS + 2;
    pump(1000, 0);
    l = lan_get_lobby(g_host.s);
    check(g_bots[NBOTS].joined && l->members[lan_local_id(g_bots[NBOTS].s)].profile.spectator,
          "a 13th player becomes a spectator (lobby full)");
    check(g_bots[NBOTS + 1].rejected && g_bots[NBOTS + 1].reason == LAN_REJECT_GAME,
          "a different game build is refused");

    printf("clock sync\n");
    pump(1500, 0);
    check(all_bots(is_synced, 0, NBOTS), "every client has its clock synced");
    max_err = 0;
    for (i = 0; i < NBOTS; i++) {
        int err = (int)(int32_t)(lan_time(g_bots[i].s) - lan_time(g_host.s));
        if (abs(err) > max_err) max_err = abs(err);
    }
    snprintf(what, sizeof what, "host clock agrees on all clients within 5 ms (worst %d ms)", max_err);
    check(max_err <= 5, what);

    printf("chat\n");
    for (i = 0; i < g_nbots; i++)
        g_bots[i].chats = 0;
    g_host.chats = 0;
    lan_chat(g_bots[2].s, "hello from Zoe3 \x01");
    pump(300, 0);
    ok = g_host.chats == 1;
    for (i = 0; i < NBOTS + 1; i++)
        ok &= g_bots[i].chats == 1;
    check(ok, "a chat message reaches the host and all 12 clients once");
    for (i = 0; i < 8; i++)
        lan_chat(g_bots[3].s, "spam");
    g_host.chats = 0;
    pump(300, 0);
    check(g_host.chats == 5, "flood limit: 5 of 8 messages in a row get through");

    printf("snapshots, 30 Hz from every rider\n");
    pump(1500, 1);
    ok = 1;
    max_err = 0;
    for (i = 0; i < NBOTS; i++) {
        int p;
        uint32_t t = lan_time(g_bots[i].s) - 100;
        for (p = 0; p <= NBOTS; p++) {
            lan_snapshot sn;
            float want;
            if (p == lan_local_id(g_bots[i].s))
                continue;
            if (!lan_remote_state(g_bots[i].s, p, t, &sn) || (int)sn.pos[1] != p || sn.anim != p) {
                ok = 0;
                continue;
            }
            want = (float)p * 100.0f + (float)(t % 100000u) / 100.0f;
            if (fabsf(sn.pos[0] - want) * 100.0f > (float)max_err)
                max_err = (int)(fabsf(sn.pos[0] - want) * 100.0f);
        }
    }
    check(ok, "every client sees all 11 other riders");
    snprintf(what, sizeof what, "interpolated position 100 ms back is right within 0.1 units (worst %d.%02d)",
             max_err / 100, max_err % 100);
    check(max_err <= 10, what);
    check(!lan_remote_state(g_bots[0].s, lan_local_id(g_bots[NBOTS].s), lan_time(g_bots[0].s), &(lan_snapshot){0}),
          "a spectator sends no rider");

    printf("ready check and synchronized start\n");
    check(!lan_start(g_host.s, 3000), "start is refused while players are not ready");
    lan_set_ready(g_host.s, 1);
    for (i = 0; i < NBOTS; i++)
        lan_set_ready(g_bots[i].s, 1);
    pump(300, 0);
    check(lan_start(g_host.s, 3000), "start once everyone is ready");
    start = lan_get_lobby(g_host.s)->start_time;
    pump(300, 0);
    ok = 1;
    for (i = 0; i < NBOTS + 1; i++)
        ok &= g_bots[i].starts == 1 && g_bots[i].start_time == start;
    check(ok, "all clients got START with the host's GO time");
    pump(3000, 1);
    check(lan_get_lobby(g_bots[0].s)->state == LAN_STATE_RUNNING, "the event is running after GO");

    printf("finish order\n");
    for (i = 0; i < NBOTS; i++)
        lan_finish(g_bots[i].s, 60000u + (uint32_t)((i * 7) % NBOTS) * 1000u);   /* a shuffled order */
    pump(300, 1);
    check(lan_get_lobby(g_host.s)->state == LAN_STATE_RUNNING, "not over while the host is still riding");
    lan_finish(g_host.s, 59500);
    pump(300, 1);
    l = lan_get_lobby(g_bots[5].s);
    ok = l->state == LAN_STATE_RESULTS && l->members[0].place == 1 && l->members[0].score == 10;
    for (i = 0; i < NBOTS; i++) {
        int id = lan_local_id(g_bots[i].s), place = 2 + (i * 7) % NBOTS;
        ok &= l->members[id].place == place;
    }
    check(ok, "results: places by race time, points on the scoreboard");
    lan_end_round(g_host.s);
    pump(300, 0);
    check(lan_get_lobby(g_bots[1].s)->state == LAN_STATE_LOBBY && !lan_get_lobby(g_bots[1].s)->members[2].ready,
          "back to the lobby, everyone unready, scores kept");

    printf("kick\n");
    i = lan_local_id(g_bots[4].s);
    lan_kick(g_host.s, i);
    pump(500, 0);
    check(g_bots[4].kicked && !lan_get_lobby(g_host.s)->members[i].used, "a kicked player is told and removed");
    lan_reconnect(g_bots[4].s);
    pump(1000, 0);
    check(g_bots[4].rejected && g_bots[4].reason == LAN_REJECT_KICKED, "and cannot come back");

    printf("drop and reconnect\n");
    i = lan_local_id(g_bots[6].s);
    {
        uint32_t score = lan_get_lobby(g_host.s)->members[i].score;
        lan_drop(g_bots[6].s);
        pump(8000, 1);
        check(!lan_get_lobby(g_host.s)->members[i].connected && lan_get_lobby(g_host.s)->members[i].used,
              "the host notices the drop and keeps the place");
        lan_reconnect(g_bots[6].s);
        pump(1000, 0);
        check(lan_local_id(g_bots[6].s) == i && lan_get_lobby(g_host.s)->members[i].connected
              && lan_get_lobby(g_host.s)->members[i].score == score,
              "reconnect gets the same id and score back");
    }

    printf("leave\n");
    i = lan_local_id(g_bots[9].s);
    lan_close(g_bots[9].s);
    g_bots[9].s = NULL;
    pump(300, 0);
    check(!lan_get_lobby(g_host.s)->members[i].used, "a player who quits is removed at once (no reconnect wait)");

    printf("LAN browser\n");
    {
        lan_browser *br = lan_browse_open();
        lan_found f[8];
        int n = 0, k, found = 0;
        if (br) {
            lan_browse_query(br);
            pump(500, 0);
            n = lan_browse_poll(br, f, 8);
            lan_browse_close(br);
        }
        for (k = 0; k < n; k++)
            if (f[k].port == g_cfg.port && !strcmp(f[k].lobby_name, "selftest") && f[k].players == 10)
                found++;
        check(found == 1, "the lobby shows up with its name and 10 players, once");
    }

    printf("host leaves\n");
    lan_close(g_host.s);
    g_host.s = NULL;
    for (i = 0; i < g_nbots; i++)
        g_bots[i].disconnected = 0;
    {
        uint32_t end = enet_time_get() + 1000;
        while ((int32_t)(enet_time_get() - end) < 0)
            for (i = 0; i < g_nbots; i++)
                if (g_bots[i].s) {
                    lan_service(g_bots[i].s, 0);
                    drain(&g_bots[i], 0, "bot");
                }
    }
    ok = 1;
    for (i = 0; i < NBOTS; i++)
        if (i != 4 && i != 9)
            ok &= g_bots[i].disconnected == 1;
    check(ok, "every client is told");

    for (i = 0; i < g_nbots; i++)
        lan_close(g_bots[i].s);
    printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "OK", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}

/* ---------------------------------------------------------------- interactive modes */

typedef struct {
    uint16_t port;
    char     name[LAN_NAME_MAX + 1];
    int      mode, max, seconds, start_ms, quiet;
    uint32_t game_id;
} options;

static int parse_options(int argc, char **argv, int first, options *o)
{
    int i;
    for (i = first; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--quiet")) { o->quiet = 1; continue; }
        if (!v) { fprintf(stderr, "%s needs a value\n", a); return -1; }
        if (!strcmp(a, "--port")) o->port = (uint16_t)atoi(v);
        else if (!strcmp(a, "--name")) snprintf(o->name, sizeof o->name, "%s", v);
        else if (!strcmp(a, "--mode")) o->mode = !strcmp(v, "freeroam") ? LAN_MODE_FREEROAM : LAN_MODE_RACE;
        else if (!strcmp(a, "--max")) o->max = atoi(v);
        else if (!strcmp(a, "--seconds")) o->seconds = atoi(v);
        else if (!strcmp(a, "--start")) o->start_ms = atoi(v);
        else if (!strcmp(a, "--game-id")) o->game_id = (uint32_t)strtoul(v, NULL, 0);
        else { fprintf(stderr, "unknown option %s\n", a); return -1; }
        i++;
    }
    return 0;
}

static int timed_out(uint32_t begin, int seconds)
{
    return seconds > 0 && enet_time_get() - begin > (uint32_t)seconds * 1000u;
}

static void print_lobby(const lan_lobby *l)
{
    int i;
    printf("lobby \"%s\" state %u round %u:", l->rules.lobby_name, l->state, l->round);
    for (i = 0; i < LAN_MAX_MEMBERS; i++)
        if (l->members[i].used)
            printf(" %u:%s%s%s(%ums,%u pts)", i, l->members[i].profile.name, l->members[i].ready ? "*" : "",
                   l->members[i].profile.spectator ? "[spec]" : "", l->members[i].ping_ms, l->members[i].score);
    printf("\n");
}

static int run_host(const options *o)
{
    lan_config c;
    lan_profile p;
    lan_rules r;
    bot h;
    uint32_t begin = enet_time_get(), last_print = 0;

    memset(&c, 0, sizeof c); memset(&p, 0, sizeof p); memset(&r, 0, sizeof r); memset(&h, 0, sizeof h);
    c.port = o->port; c.game_id = o->game_id; c.quiet = o->quiet;
    snprintf(p.name, sizeof p.name, "%s", o->name[0] ? o->name : "Host");
    snprintf(r.lobby_name, sizeof r.lobby_name, "%s's lobby", p.name);
    r.mode = (uint8_t)o->mode; r.max_players = (uint8_t)(o->max ? o->max : 12);
    r.spectators = 1; r.leveled = 1; r.ai_fill = 1; r.bragging = 1;
    if (!(h.s = lan_host(&c, &p, &r)))
        return 2;
    lan_set_ready(h.s, 1);
    while (!timed_out(begin, o->seconds)) {
        lan_service(h.s, 10);
        drain(&h, 1, "host");
        if (o->start_ms && lan_get_lobby(h.s)->state == LAN_STATE_LOBBY) {
            int i, n = 0;
            for (i = 1; i < LAN_MAX_MEMBERS; i++)
                n += lan_get_lobby(h.s)->members[i].used;
            if (n && lan_start(h.s, (uint32_t)o->start_ms))
                printf("host: everyone ready, starting\n");
        }
        if (enet_time_get() - last_print > 2000) {
            last_print = enet_time_get();
            print_lobby(lan_get_lobby(h.s));
            fflush(stdout);
        }
    }
    lan_close(h.s);
    return 0;
}

static int run_bots(int n, const char *target, const options *o)
{
    char host[128];
    char *colon;
    uint16_t port = o->port;
    bot *bots;
    uint32_t begin = enet_time_get(), next_snap = 0;
    int i;

    snprintf(host, sizeof host, "%s", target);
    if ((colon = strrchr(host, ':')) != NULL) {
        *colon = 0;
        port = (uint16_t)atoi(colon + 1);
    }
    if (n < 1 || n > 32 || !(bots = (bot *)calloc((size_t)n, sizeof *bots)))
        return 2;
    for (i = 0; i < n; i++) {
        lan_config c;
        lan_profile p;
        memset(&c, 0, sizeof c); memset(&p, 0, sizeof p);
        c.game_id = o->game_id; c.quiet = o->quiet;
        snprintf(p.name, sizeof p.name, "%s%d", o->name[0] ? o->name : "Bot", i + 1);
        p.character = (uint8_t)(i % 10);
        p.level = 1;
        bots[i].s = lan_join(&c, &p, host, port);
    }
    while (!timed_out(begin, o->seconds)) {
        uint32_t now = enet_time_get();
        for (i = 0; i < n; i++) {
            int was = bots[i].joined;
            if (!bots[i].s)
                continue;
            lan_service(bots[i].s, i ? 0 : 5);
            drain(&bots[i], i == 0, "bot1");
            if (bots[i].joined && !was)
                lan_set_ready(bots[i].s, 1);
            if ((bots[i].rejected || bots[i].kicked) && bots[i].s) {
                lan_close(bots[i].s);
                bots[i].s = NULL;
            }
        }
        if ((int32_t)(now - next_snap) >= 0) {
            next_snap = now + 33;
            for (i = 0; i < n; i++) {
                lan_snapshot sn;
                float a;
                if (!bots[i].s || lan_local_id(bots[i].s) == LAN_NO_PLAYER)
                    continue;
                memset(&sn, 0, sizeof sn);
                a = (float)(now % 10000u) / 10000.0f * 6.2831853f + (float)i;   /* riding in circles */
                sn.pos[0] = cosf(a) * 50.0f; sn.pos[1] = sinf(a) * 50.0f;
                sn.rot[2] = sinf(a / 2); sn.rot[3] = cosf(a / 2);
                lan_send_snapshot(bots[i].s, &sn);
            }
        }
    }
    for (i = 0; i < n; i++)
        lan_close(bots[i].s);
    free(bots);
    return 0;
}

static int run_browse(void)
{
    lan_browser *b = lan_browse_open();
    lan_found f[32];
    uint32_t end;
    int n, i;
    if (!b)
        return 2;
    lan_browse_query(b);
    end = enet_time_get() + 1000;
    do
        n = lan_browse_poll(b, f, 32);
    while ((int32_t)(enet_time_get() - end) < 0);
    for (i = 0; i < n; i++)
        printf("%s:%u  \"%s\"  %s  %u/%u  state %u  %s\n", f[i].address, f[i].port, f[i].lobby_name,
               f[i].mode == LAN_MODE_FREEROAM ? "free roam" : "race", f[i].players, f[i].max_players, f[i].state,
               f[i].version != LAN_PROTOCOL_VERSION ? "(other version)"
               : f[i].game_id != DEFAULT_GAME_ID ? "(other game build)" : "");
    if (!n)
        printf("no lobbies found\n");
    lan_browse_close(b);
    return 0;
}

int main(int argc, char **argv)
{
    options o;
    int rc;
    memset(&o, 0, sizeof o);
    o.game_id = DEFAULT_GAME_ID;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s selftest | host [opts] | bots N ADDRESS[:PORT] [opts] | browse\n", argv[0]);
        return 2;
    }
    if (lan_net_init() != 0) {
        fprintf(stderr, "ENet init failed\n");
        return 2;
    }
    if (!strcmp(argv[1], "selftest"))
        rc = selftest();
    else if (!strcmp(argv[1], "host"))
        rc = parse_options(argc, argv, 2, &o) ? 2 : run_host(&o);
    else if (!strcmp(argv[1], "bots") && argc >= 4)
        rc = parse_options(argc, argv, 4, &o) ? 2 : run_bots(atoi(argv[2]), argv[3], &o);
    else if (!strcmp(argv[1], "browse"))
        rc = run_browse();
    else {
        fprintf(stderr, "unknown command %s\n", argv[1]);
        rc = 2;
    }
    lan_net_shutdown();
    return rc;
}
