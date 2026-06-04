// ============================================================
// Vault 89 PDI-Boy -- Multi-device Game Protocol
// Shared ESP-NOW wire format for turn-based games. Each game
// (blackjack, liar's dice, hearts, ...) plugs its own opaque
// state + action blobs into the generic envelope below. The
// host drives turns; the protocol handles invites, joins,
// state broadcasts, nudges for inattentive players, and end.
// ============================================================
#pragma once

#include <stdint.h>

// Message type IDs reserve 12..19 for games. Keep in sync with
// the MsgType enum in net_espnow.cpp.
#define MSG_GAME_INVITE  12   // host -> broadcast: "new table forming"
#define MSG_GAME_JOIN    13   // player -> host:   "I'm in, here's my name"
#define MSG_GAME_START   14   // host -> broadcast: seats locked, round on
#define MSG_GAME_STATE   15   // host -> broadcast: latest shared state
#define MSG_GAME_ACTION  16   // player -> host:   my turn's move
#define MSG_GAME_NUDGE   17   // host -> one:      "wake up, it's your turn"
#define MSG_GAME_END     18   // host -> broadcast: results + table closed

// Game IDs. Add to this enum whenever you implement a new one.
enum GameId : uint8_t {
    GAME_NONE        = 0,
    GAME_BLACKJACK   = 1,
    GAME_LIARS_DICE  = 2,
    GAME_GO_FISH     = 3,
    GAME_HEARTS      = 4,
    GAME_TICTACTOE   = 5,
};

#define GAME_MAX_PLAYERS     6
#define GAME_NAME_LEN       20
#define GAME_STATE_MAX     180   // fits after 16-byte header in 250-byte MTU
#define GAME_ACTION_MAX     32

// Every game frame begins with this 16-byte header so the router
// can dispatch on {game_id, session_id} before decoding the body.
// session_id is a random uint32 chosen by the host at INVITE time.
// seq increments on each host-originated STATE so late/duplicate
// frames can be detected; player ACTIONs echo the last-seen seq.
struct __attribute__((packed)) GameHdr {
    uint8_t  type;         // MSG_GAME_*
    uint8_t  game_id;      // GAME_*
    uint8_t  host_mac[6];
    uint32_t session_id;
    uint16_t seq;
    uint16_t reserved;
};

// ── Invitation ────────────────────────────────────────────────
// Broadcast when a host opens a table. Recipients flash their
// screen, beep, and show an accept/decline modal. An invite
// without any joins times out client-side after INVITE_TTL_MS.
struct __attribute__((packed)) GameInvite {
    GameHdr  hdr;                   // type = MSG_GAME_INVITE
    char     host_name[GAME_NAME_LEN];
    uint8_t  max_players;           // including host
    uint8_t  min_players;           // host won't START below this
    uint8_t  ttl_sec;               // invite window
    uint8_t  _pad;
};

// ── Join request ──────────────────────────────────────────────
// Player -> host. Host replies with a STATE that includes the
// player in the seat list (implicit ACK). Declines are silent.
struct __attribute__((packed)) GameJoin {
    GameHdr  hdr;                   // type = MSG_GAME_JOIN
    uint8_t  player_mac[6];
    char     player_name[GAME_NAME_LEN];
};

// ── Start ─────────────────────────────────────────────────────
// Host -> broadcast once seats are locked. Contains the seat
// order so every client renders the same table orientation.
struct __attribute__((packed)) GameStart {
    GameHdr  hdr;                   // type = MSG_GAME_START
    uint8_t  seat_count;
    uint8_t  seats[GAME_MAX_PLAYERS][6];   // MACs in turn order
    char     names[GAME_MAX_PLAYERS][GAME_NAME_LEN];
};

// ── State broadcast ───────────────────────────────────────────
// Host authoritative. Body is opaque; the game module interprets
// state_buf based on game_id. active_seat names whose turn it is
// (0xFF = no active turn, e.g. between rounds).
struct __attribute__((packed)) GameState {
    GameHdr  hdr;                   // type = MSG_GAME_STATE
    uint8_t  active_seat;
    uint8_t  phase;                 // game-defined phase byte
    uint16_t state_len;
    uint8_t  state_buf[GAME_STATE_MAX];
};

// ── Action ────────────────────────────────────────────────────
// Player -> host. action_buf is game-defined (e.g. for blackjack:
// {HIT, STAND, DOUBLE}; for liar's dice: a {count, face} bid).
struct __attribute__((packed)) GameAction {
    GameHdr  hdr;                   // type = MSG_GAME_ACTION
    uint8_t  action_id;
    uint8_t  action_len;
    uint8_t  action_buf[GAME_ACTION_MAX];
};

// ── Nudge ─────────────────────────────────────────────────────
// Host -> one player. Sent when the active seat hasn't acted in
// NUDGE_IDLE_MS. Recipient flashes screen + beeps even if the
// game screen is backgrounded (hence the wake_screen flag,
// interpreted by the UI layer).
struct __attribute__((packed)) GameNudge {
    GameHdr  hdr;                   // type = MSG_GAME_NUDGE
    uint8_t  target_mac[6];
    uint8_t  wake_screen;           // 1 = force game screen to front
    uint8_t  reason;                // 0 = your turn, 1 = round ending soon
};

// ── End ───────────────────────────────────────────────────────
// Host -> broadcast. Winners + short summary string. After END,
// session_id is retired; further frames with it are ignored.
struct __attribute__((packed)) GameEnd {
    GameHdr  hdr;                   // type = MSG_GAME_END
    uint8_t  winner_count;
    uint8_t  winner_seats[GAME_MAX_PLAYERS];
    char     summary[64];
};

// ── Timings ───────────────────────────────────────────────────
#define GAME_INVITE_TTL_MS      30000UL   // invite modal window
#define GAME_NUDGE_IDLE_MS      30000UL   // first nudge after silence
#define GAME_NUDGE_REPEAT_MS    15000UL   // subsequent nudges
#define GAME_NUDGE_GIVEUP_MS    90000UL   // host auto-folds/skips this seat
#define GAME_FLASH_COUNT        3         // screen inverts N times on invite/nudge
#define GAME_FLASH_PERIOD_MS    400

// ── Module hooks a game implementation fills in ───────────────
// Each game (blackjack.cpp, liars_dice.cpp, ...) registers a
// GameModule at boot. The router dispatches incoming host/player
// frames to the matching game_id. The protocol layer handles
// invites, joins, nudges, flashes — the module only cares about
// state transitions and rendering.
typedef void (*game_on_state_fn)(const GameState* s, bool is_host);
typedef void (*game_on_action_fn)(const GameAction* a, const uint8_t* from_mac);
typedef void (*game_host_tick_fn)(void);     // called ~10Hz on host
typedef void (*game_ui_build_fn)(void);      // open the game screen
typedef void (*game_on_start_fn)(uint8_t seat_count);  // host + player sides

struct GameModule {
    uint8_t          game_id;
    const char*      display_name;
    game_on_state_fn  on_state;
    game_on_action_fn on_action;
    game_host_tick_fn host_tick;
    game_ui_build_fn  build_ui;
    game_on_start_fn  on_start;
};

// Public API -- implemented in game_proto.cpp
void game_register(const GameModule* m);
void game_host_invite(uint8_t game_id, uint8_t min_p, uint8_t max_p);
void game_player_accept(uint32_t session_id);
void game_player_decline(uint32_t session_id);
void game_send_action(uint8_t action_id, const uint8_t* buf, uint8_t len);
void game_host_broadcast_state(uint8_t active_seat, uint8_t phase,
                               const uint8_t* state, uint16_t len);
void game_host_end(const uint8_t* winner_seats, uint8_t n,
                   const char* summary);
void game_on_rx(const uint8_t* mac, const uint8_t* data, int len);
void game_poll(void);

// Host: lock the table and broadcast MSG_GAME_START to all joined
// seats, then invoke the module's on_start callback so it can build
// initial state + broadcast the first STATE. No-op if called from a
// non-host role or with fewer than the session's min_players.
void game_host_start(void);

// Cancel a host-side invite (before START). Broadcasts MSG_GAME_END
// with an empty winner list so any accepted peers tear down too.
void game_host_cancel(void);

// Inspect the current seat roster. Returns the number of seats (0 if
// no session). out_macs / out_names can be NULL if the caller only
// wants the count. out_names is a flat 20-byte-per-entry buffer.
uint8_t game_seat_count(void);
void    game_seats(uint8_t out_macs[][6], char out_names[][GAME_NAME_LEN]);

// Which seat index are we on the current session's table? Returns
// 0xFF if not seated (or not in a session).
uint8_t game_my_seat(void);

// True while a host-initiated invite is open (pre-START).
bool game_host_is_inviting(void);

// True for the host once game_host_start() has fired.
bool game_host_started(void);

// True if this device is currently part of an active game session
// (either as host or player). Used by lobby UI to detect cancellation.
bool game_in_session(void);

// Player: tear down session state so new invites can be received.
// Call when the player leaves the lobby (back/timeout) or exits the
// game screen. No-op if not in ROLE_PLAYER.
void game_player_leave(void);
