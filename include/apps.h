// ============================================================
// Vault 89 PDI-Boy -- Apps subsystem shared state / API
// Cross-app features that need to outlive their screen:
//   - Pomodoro timer (runs in background so you can switch away)
//   - DND indicator (broadcast via ESP-NOW + WS)
//   - Mesh presence (active/away/busy)
//   - Server -> device pager (inbound notifications)
//   - Shared quest board
// ============================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

// ── Pomodoro ─────────────────────────────────────────────────────────────────
enum PomoPhase {
    POMO_IDLE     = 0,
    POMO_WORK     = 1,
    POMO_BREAK    = 2,
    POMO_LONGBRK  = 3,
};

struct PomoState {
    PomoPhase phase;
    uint32_t  phase_start_ms;   // millis() when the current phase started
    uint32_t  phase_len_ms;     // length of current phase in ms
    uint32_t  work_len_s;       // configured work length (default 1500 = 25min)
    uint32_t  break_len_s;      // configured break length (default 300 = 5min)
    uint32_t  long_break_s;     // long break length (default 900 = 15min)
    uint8_t   cycle_count;      // number of work sessions completed this run
    bool      running;          // true = timer active (not paused)
    uint32_t  paused_at_ms;     // millis() when paused (for resume math)
    bool      just_rolled;      // set by tick when a phase transitioned; cleared by UI
};

extern PomoState g_pomo;

void pomo_init(void);
void pomo_tick(void);               // call from main loop; advances phases
void pomo_start_work(void);         // begin a fresh work session
void pomo_toggle_pause(void);
void pomo_reset(void);
uint32_t pomo_remaining_ms(void);   // 0 when idle

// ── Presence / DND ───────────────────────────────────────────────────────────
enum PresenceMode {
    PRESENCE_ACTIVE = 0,
    PRESENCE_AWAY   = 1,
    PRESENCE_BUSY   = 2,   // "DND" — suppresses pager buzz
    PRESENCE_DND    = 3,   // explicit meeting/DND
};

struct PresenceState {
    PresenceMode mode;
    char         custom[24];    // optional user-set status string
    uint32_t     last_broadcast_ms;
};

extern PresenceState g_presence;

void presence_init(void);
void presence_set(PresenceMode m);
const char* presence_label(PresenceMode m);
void presence_poll(void);   // periodic broadcast

// ── Pager (inbound notifications from server or mesh) ───────────────────────
#define PAGER_MAX    6
#define PAGER_TEXT_LEN 40

struct PagerMsg {
    char     from[16];
    char     text[PAGER_TEXT_LEN];
    uint32_t rx_ms;
    bool     seen;
};

extern PagerMsg g_pager[PAGER_MAX];
extern int      g_pager_count;
extern int      g_pager_unread;

void pager_init(void);
void pager_push(const char* from, const char* text);
void pager_mark_all_read(void);

// ── Shared quest board ──────────────────────────────────────────────────────
#define QUEST_MAX     8
#define QUEST_TEXT_LEN 40

struct QuestItem {
    char     text[QUEST_TEXT_LEN];
    char     owner[16];
    bool     done;
    uint32_t updated_ms;
    uint32_t uid;           // monotonically increasing unique id (owner+count)
};

extern QuestItem g_quests[QUEST_MAX];
extern int       g_quest_count;

void quests_init(void);
int  quests_add_local(const char* text);        // returns slot, -1 full
void quests_toggle(int slot);
void quests_remove(int slot);
void quests_broadcast_sync(void);                // push whole list via mesh
void quests_apply_remote(const char* owner, uint32_t uid, const char* text,
                         bool done, uint32_t updated_ms);
