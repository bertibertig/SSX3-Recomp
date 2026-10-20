/*
 * LAN multiplayer – net core (docs/lan-multiplayer.md, phase 1).
 *
 * Game-independent: sessions, lobby, chat, clock sync, snapshots and LAN discovery over ENet. The game hooks
 * (src/lan/) and the headless test tool (tools/lanbot/) both sit on top of this API; nothing here touches guest
 * memory.
 *
 * Topology: a star. The host (the player who opened the lobby, player id 0) owns the session: the player list,
 * the rules, the clock, the start signal, the finish order and the scoreboard. Clients own only their own
 * rider and send its snapshots to the host, which forwards them to everyone else.
 *
 * Channels: 0 reliable and ordered (lobby, chat, start, results), 1 unreliable (snapshots, clock pings).
 *
 * All functions are single-threaded: call lan_service() regularly (each frame) from one thread and drain
 * events with lan_next_event() after it.
 */
#ifndef SSX3_NET_H
#define SSX3_NET_H

#include <stddef.h>
#include <stdint.h>

#define LAN_PROTOCOL_VERSION 2u          /* 2: snapshots carry the control input */
#define LAN_DEFAULT_PORT     27215u     /* game sessions (ENet) */
#define LAN_DISCOVERY_PORT   27216u     /* lobby browser queries (plain UDP) */

#define LAN_MAX_PLAYERS      12         /* riders in one session */
#define LAN_MAX_SPECTATORS   4
#define LAN_MAX_MEMBERS      (LAN_MAX_PLAYERS + LAN_MAX_SPECTATORS)
#define LAN_NAME_MAX         15         /* characters, printable ASCII */
#define LAN_LOBBY_NAME_MAX   31
#define LAN_CHAT_MAX         120
#define LAN_NO_PLAYER        0xFFu
#define LAN_ATTRIBS          7          /* rider attributes in the profile */
#define LAN_ATTRIB_BASE      5          /* an unleveled attribute (one bar) */
#define LAN_GEAR_MAX         32         /* equipped items in a profile (the user's Zoe: 19) */
#define LAN_RECONNECT_MS     30000u     /* a dropped player keeps its place this long */

typedef enum {
    LAN_MODE_RACE = 0,
    LAN_MODE_FREEROAM = 1,              /* Conquer the Mountain */
    LAN_MODE_FREESTYLE = 2,             /* later (phase 9) */
} lan_mode;

typedef enum {
    LAN_STATE_LOBBY = 0,
    LAN_STATE_COUNTDOWN = 1,            /* start time is set, everyone loads the event */
    LAN_STATE_RUNNING = 2,
    LAN_STATE_RESULTS = 3,
} lan_state;

typedef enum {
    LAN_REJECT_NONE = 0,
    LAN_REJECT_VERSION,                 /* different protocol version */
    LAN_REJECT_GAME,                    /* different game build or XBE */
    LAN_REJECT_FULL,
    LAN_REJECT_KICKED,
    LAN_REJECT_RUNNING,                 /* race in progress and spectators are not allowed */
    LAN_REJECT_BAD_HELLO,
} lan_reject;

typedef enum {
    LAN_COLLIDE_GHOST = 0,
    LAN_COLLIDE_BUMP = 1,
} lan_collide;

/* What a player brings to the session: chosen in the lobby or imported from a save. */
typedef struct {
    char    name[LAN_NAME_MAX + 1];
    uint8_t character;                  /* the game's character index */
    uint8_t gear_count;
    uint16_t gear[LAN_GEAR_MAX];        /* equipped item ids (inventory flag 0x10): outfit pieces, board, ... */
    uint8_t level;                      /* 1 = unleveled */
    uint8_t attrib[LAN_ATTRIBS];        /* the save's attribute bytes: 5 per bar, 5 .. 55 (notes.md, spike 6) */
    uint8_t handicaps;                  /* Bragging Rights bits */
    uint8_t spectator;                  /* joins as viewer */
} lan_profile;

typedef struct {
    char    lobby_name[LAN_LOBBY_NAME_MAX + 1];
    uint8_t mode;                       /* lan_mode */
    uint8_t peak, event;
    uint8_t max_players;                /* 2 .. LAN_MAX_PLAYERS (race: 6 for now) */
    uint8_t leveled;                    /* 1: leveled riders allowed, 0: everyone at level 1 */
    uint8_t ai_fill;
    uint8_t collide;                    /* lan_collide */
    uint8_t spectators;                 /* spectators allowed */
    uint8_t bragging;                   /* Bragging Rights allowed */
} lan_rules;

typedef struct {
    uint8_t     used;
    uint8_t     id;                     /* stable for the session, 0 = host */
    uint8_t     connected;              /* 0 while waiting for a reconnect */
    uint8_t     ready;
    uint8_t     finished;
    uint16_t    ping_ms;
    uint32_t    finish_ms;              /* race time, valid when finished */
    uint32_t    score;                  /* session scoreboard */
    uint8_t     place;                  /* 1-based place in the last result, 0 = none */
    uint8_t     slot;                   /* rider slot in the running event, LAN_NO_PLAYER = none */
    lan_profile profile;
} lan_member;

typedef struct {
    lan_rules  rules;
    uint8_t    state;                   /* lan_state */
    uint8_t    round;                   /* events started in this session */
    uint32_t   start_time;              /* host clock, ms: the moment GO is shown */
    lan_member members[LAN_MAX_MEMBERS];
} lan_lobby;

/* A rider's state at one moment, sent at about 30 Hz. Positions in game units, z up (rider+0x180). */
typedef struct {
    uint32_t time;                      /* host clock, ms */
    float    pos[3];
    float    rot[4];                    /* quaternion x, y, z, w */
    float    vel[3];
    uint16_t anim;                      /* animation / trick state */
    uint16_t flags;
    float    boost;
    float    progress;                  /* track progress (race rank) */
    uint32_t input[2];                  /* the rider's control input this frame (game: 8 bytes, see lan_game.c) */
} lan_snapshot;

typedef enum {
    LAN_EV_NONE = 0,
    LAN_EV_JOINED,                      /* client: welcome received, `player` = our id */
    LAN_EV_REJECTED,                    /* client: `reason` */
    LAN_EV_DISCONNECTED,                /* client: lost the host; host: a member dropped (`player`) */
    LAN_EV_LOBBY,                       /* the lobby changed (players, rules, state) */
    LAN_EV_CHAT,                        /* `player` (LAN_NO_PLAYER = system), `text` */
    LAN_EV_START,                       /* countdown: GO at `time` (host clock) */
    LAN_EV_RESULTS,                     /* finish order changed (see members[].place) */
    LAN_EV_KICKED,                      /* client: the host removed us */
} lan_event_type;

typedef struct {
    lan_event_type type;
    uint8_t        player;
    uint8_t        reason;
    uint32_t       time;
    char           text[LAN_CHAT_MAX + 1];
} lan_event;

typedef struct {
    uint16_t port;                      /* host: listen port (0 = LAN_DEFAULT_PORT) */
    uint32_t game_id;                   /* identity of the game build + XBE; must match to join */
    int32_t  clock_skew_ms;             /* tests only: offset added to this session's local clock */
    int      quiet;                     /* no log lines on stderr */
} lan_config;

typedef struct lan_session lan_session;

int  lan_net_init(void);                /* once per process; 0 on success */
void lan_net_shutdown(void);

lan_session *lan_host(const lan_config *cfg, const lan_profile *me, const lan_rules *rules);
/* `address`: host name or IP; `port` 0 = default. Connecting is asynchronous: wait for LAN_EV_JOINED. */
lan_session *lan_join(const lan_config *cfg, const lan_profile *me, const char *address, uint16_t port);
/* Leaves the session (a client tells the host; a host closes it for everyone). */
void lan_close(lan_session *s);
/* Tests: drops the connection without telling the host, as a crash or cable pull would. */
void lan_drop(lan_session *s);
/* Reconnects a dropped client with the same token, so it gets its old place back. */
int  lan_reconnect(lan_session *s);

/* Network work: sends, receives, clock pings, reconnect timeouts. `wait_ms` blocks for traffic (0 = poll). */
void lan_service(lan_session *s, uint32_t wait_ms);
int  lan_next_event(lan_session *s, lan_event *ev);

int              lan_is_host(const lan_session *s);
int              lan_local_id(const lan_session *s);           /* LAN_NO_PLAYER until joined */
const lan_lobby *lan_get_lobby(const lan_session *s);
uint32_t         lan_time(const lan_session *s);               /* host clock, ms */
int              lan_clock_synced(const lan_session *s);

void lan_set_profile(lan_session *s, const lan_profile *p);
void lan_set_ready(lan_session *s, int ready);
void lan_chat(lan_session *s, const char *text);
void lan_send_snapshot(lan_session *s, const lan_snapshot *snap);
void lan_finish(lan_session *s, uint32_t race_ms);             /* our rider crossed the line */

/* The state of `player` at host time `t`: interpolated between the two snapshots around it, extrapolated
 * up to 250 ms past the newest one. Returns 0 if there is nothing to show. Render remote riders at
 * lan_time() - 100 ms. */
int lan_remote_state(const lan_session *s, int player, uint32_t t, lan_snapshot *out);

/* Host only (ignored on clients). */
void lan_set_rules(lan_session *s, const lan_rules *rules);
void lan_kick(lan_session *s, int player);
int  lan_start(lan_session *s, uint32_t delay_ms);             /* 0 if not everyone is ready */
void lan_end_round(lan_session *s);                            /* back to the lobby, keeps the scores */

/* LAN browser: asks every host on the local networks (and 127.0.0.1) for its lobby. */
typedef struct {
    char     address[64];
    uint16_t port;
    char     lobby_name[LAN_LOBBY_NAME_MAX + 1];
    uint8_t  mode, state, players, max_players;
    uint32_t game_id;
    uint16_t version;
} lan_found;

typedef struct lan_browser lan_browser;

lan_browser *lan_browse_open(void);
void         lan_browse_query(lan_browser *b);                 /* send a query; answers arrive in poll */
int          lan_browse_poll(lan_browser *b, lan_found *out, int max);   /* all lobbies seen so far */
void         lan_browse_close(lan_browser *b);

const char *lan_reject_text(int reason);

#endif
