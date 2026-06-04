// ============================================================
// Vault 89 PDI-Boy -- Apps backend (state + lifecycle)
// Lives outside of any single screen so Pomodoro, presence,
// pager, and quest state persist across navigation.
// ============================================================
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include "apps.h"
#include "sound_fx.h"
#include "net_state.h"
#include "pipboy_data.h"

// ── Globals ──

// Per-user custom display name (overrides the selected archetype's
// name on the network + UI when non-empty). Persisted to NVS.
char g_custom_name[CUSTOM_NAME_LEN] = {0};

const char* user_display_name(void) {
    if (g_custom_name[0]) return g_custom_name;
    if (g_dweller_idx >= 0 && g_dweller_idx < TEAM_COUNT)
        return team[g_dweller_idx].name;
    return "DWELLER";
}

void user_name_save(const char* name) {
    if (!name) name = "";
    strncpy(g_custom_name, name, CUSTOM_NAME_LEN - 1);
    g_custom_name[CUSTOM_NAME_LEN - 1] = '\0';
    Preferences p;
    if (p.begin("user", false)) {
        p.putString("name", g_custom_name);
        p.end();
    }
    Serial.printf("[user] custom name saved: '%s'\n", g_custom_name);
}

void user_name_load(void) {
    Preferences p;
    if (p.begin("user", true)) {
        String n = p.getString("name", "");
        p.end();
        strncpy(g_custom_name, n.c_str(), CUSTOM_NAME_LEN - 1);
        g_custom_name[CUSTOM_NAME_LEN - 1] = '\0';
    } else {
        g_custom_name[0] = '\0';
    }
    Serial.printf("[user] custom name loaded: '%s'\n", g_custom_name);
}

PomoState     g_pomo     = {};
PresenceState g_presence = {};
PagerMsg      g_pager[PAGER_MAX] = {};
int           g_pager_count  = 0;
int           g_pager_unread = 0;
QuestItem     g_quests[QUEST_MAX] = {};
int           g_quest_count  = 0;

// ── Pomodoro ─────────────────────────────────────────────────────────────────

void pomo_init(void) {
    memset(&g_pomo, 0, sizeof(g_pomo));
    Preferences p;
    if (p.begin("pomo", true)) {
        g_pomo.work_len_s  = p.getUInt("w", 25 * 60);
        g_pomo.break_len_s = p.getUInt("b", 5 * 60);
        g_pomo.long_break_s = p.getUInt("lb", 15 * 60);
        p.end();
    } else {
        g_pomo.work_len_s  = 25 * 60;
        g_pomo.break_len_s = 5 * 60;
        g_pomo.long_break_s = 15 * 60;
    }
    g_pomo.phase = POMO_IDLE;
    Serial.printf("[pomo] init: work=%us break=%us\n",
                  g_pomo.work_len_s, g_pomo.break_len_s);
}

static void pomo_begin_phase(PomoPhase phase) {
    g_pomo.phase = phase;
    g_pomo.phase_start_ms = millis();
    uint32_t len_s = 0;
    switch (phase) {
        case POMO_WORK:    len_s = g_pomo.work_len_s;  break;
        case POMO_BREAK:   len_s = g_pomo.break_len_s; break;
        case POMO_LONGBRK: len_s = g_pomo.long_break_s; break;
        default: len_s = 0;
    }
    g_pomo.phase_len_ms = len_s * 1000UL;
    g_pomo.running = (phase != POMO_IDLE);
    g_pomo.just_rolled = true;
}

void pomo_start_work(void) {
    pomo_begin_phase(POMO_WORK);
    Serial.println("[pomo] -> WORK");
}

void pomo_toggle_pause(void) {
    if (g_pomo.phase == POMO_IDLE) {
        pomo_start_work();
        return;
    }
    if (g_pomo.running) {
        g_pomo.running = false;
        g_pomo.paused_at_ms = millis();
        Serial.println("[pomo] pause");
    } else {
        // Shift phase_start_ms forward by pause duration so remaining is stable
        uint32_t paused = millis() - g_pomo.paused_at_ms;
        g_pomo.phase_start_ms += paused;
        g_pomo.running = true;
        Serial.println("[pomo] resume");
    }
}

void pomo_reset(void) {
    g_pomo.phase        = POMO_IDLE;
    g_pomo.running      = false;
    g_pomo.phase_len_ms = 0;
    g_pomo.cycle_count  = 0;
    g_pomo.just_rolled  = true;
    Serial.println("[pomo] reset");
}

uint32_t pomo_remaining_ms(void) {
    if (g_pomo.phase == POMO_IDLE || g_pomo.phase_len_ms == 0) return 0;
    uint32_t elapsed;
    if (g_pomo.running) {
        elapsed = millis() - g_pomo.phase_start_ms;
    } else {
        elapsed = g_pomo.paused_at_ms - g_pomo.phase_start_ms;
    }
    if (elapsed >= g_pomo.phase_len_ms) return 0;
    return g_pomo.phase_len_ms - elapsed;
}

void pomo_tick(void) {
    if (!g_pomo.running || g_pomo.phase == POMO_IDLE) return;
    if (pomo_remaining_ms() > 0) return;

    // Phase complete — advance.
    PomoPhase old = g_pomo.phase;
    if (old == POMO_WORK) {
        g_pomo.cycle_count++;
        // Every 4 work sessions → long break, else short break
        if ((g_pomo.cycle_count % 4) == 0) pomo_begin_phase(POMO_LONGBRK);
        else                               pomo_begin_phase(POMO_BREAK);
        sfx_play(SFX_VAULT_ALERT);
        Serial.printf("[pomo] WORK done (#%u) -> break\n", g_pomo.cycle_count);
    } else {
        pomo_begin_phase(POMO_WORK);
        sfx_play(SFX_CONNECT);
        Serial.println("[pomo] BREAK done -> work");
    }
}

// ── Presence / DND ───────────────────────────────────────────────────────────

void presence_init(void) {
    memset(&g_presence, 0, sizeof(g_presence));
    g_presence.mode = PRESENCE_ACTIVE;
}

const char* presence_label(PresenceMode m) {
    switch (m) {
        case PRESENCE_ACTIVE: return "ACTIVE";
        case PRESENCE_AWAY:   return "AWAY";
        case PRESENCE_BUSY:   return "BUSY";
        case PRESENCE_DND:    return "DND";
    }
    return "?";
}

void presence_set(PresenceMode m) {
    if (m == g_presence.mode) return;
    g_presence.mode = m;
    g_presence.last_broadcast_ms = 0;  // force immediate rebroadcast
    Serial.printf("[presence] -> %s\n", presence_label(m));
}

// Broadcast presence once every 30s (and on any state change).
void presence_poll(void) {
    uint32_t now = millis();
    if (g_presence.last_broadcast_ms != 0 &&
        (now - g_presence.last_broadcast_ms) < 30000UL) return;
    g_presence.last_broadcast_ms = now;
    // TODO: send ESP-NOW presence frame + WS presence packet.
    // Implementation stays soft for now — the UI updates just fine
    // without a network round-trip.
}

// ── Pager ────────────────────────────────────────────────────────────────────

void pager_init(void) {
    memset(g_pager, 0, sizeof(g_pager));
    g_pager_count  = 0;
    g_pager_unread = 0;
}

void pager_push(const char* from, const char* text) {
    if (!from || !text) return;
    // Shift older messages down, newest at index 0
    int keep = g_pager_count;
    if (keep >= PAGER_MAX) keep = PAGER_MAX - 1;
    for (int i = keep; i > 0; i--) g_pager[i] = g_pager[i - 1];
    strncpy(g_pager[0].from, from, sizeof(g_pager[0].from) - 1);
    g_pager[0].from[sizeof(g_pager[0].from) - 1] = '\0';
    strncpy(g_pager[0].text, text, sizeof(g_pager[0].text) - 1);
    g_pager[0].text[sizeof(g_pager[0].text) - 1] = '\0';
    g_pager[0].rx_ms = millis();
    g_pager[0].seen  = false;
    if (g_pager_count < PAGER_MAX) g_pager_count++;
    g_pager_unread++;
    // Respect DND: suppress buzz when DND or BUSY
    if (g_presence.mode != PRESENCE_DND && g_presence.mode != PRESENCE_BUSY) {
        sfx_play(SFX_MSG_IN);
    }
    Serial.printf("[pager] <- %s: %s\n", from, text);
}

void pager_mark_all_read(void) {
    for (int i = 0; i < g_pager_count; i++) g_pager[i].seen = true;
    g_pager_unread = 0;
}

// ── Quest board ──────────────────────────────────────────────────────────────

static uint32_t s_quest_uid_seq = 1;

void quests_init(void) {
    memset(g_quests, 0, sizeof(g_quests));
    g_quest_count = 0;
}

int quests_add_local(const char* text) {
    if (!text || g_quest_count >= QUEST_MAX) return -1;
    int slot = g_quest_count++;
    strncpy(g_quests[slot].text, text, QUEST_TEXT_LEN - 1);
    g_quests[slot].text[QUEST_TEXT_LEN - 1] = '\0';
    // Use the local dweller name as owner (handled externally); leave blank
    // for now and let the broadcast code fill it.
    strncpy(g_quests[slot].owner, "ME", sizeof(g_quests[slot].owner) - 1);
    g_quests[slot].done       = false;
    g_quests[slot].updated_ms = millis();
    g_quests[slot].uid        = s_quest_uid_seq++;
    quests_broadcast_sync();
    return slot;
}

void quests_toggle(int slot) {
    if (slot < 0 || slot >= g_quest_count) return;
    g_quests[slot].done       = !g_quests[slot].done;
    g_quests[slot].updated_ms = millis();
    quests_broadcast_sync();
}

void quests_remove(int slot) {
    if (slot < 0 || slot >= g_quest_count) return;
    for (int i = slot; i < g_quest_count - 1; i++) g_quests[i] = g_quests[i + 1];
    g_quest_count--;
    memset(&g_quests[g_quest_count], 0, sizeof(QuestItem));
    quests_broadcast_sync();
}

// Broadcast stub. Real implementation would serialize + send via ESP-NOW /
// WS with a per-item {owner, uid, text, done, updated_ms} payload. For now
// this is a local-only list (mesh wiring comes in a follow-up).
void quests_broadcast_sync(void) { /* TODO */ }

void quests_apply_remote(const char* owner, uint32_t uid, const char* text,
                         bool done, uint32_t updated_ms) {
    // Find existing by (owner, uid) — update if newer, else append.
    for (int i = 0; i < g_quest_count; i++) {
        if (g_quests[i].uid == uid && strcmp(g_quests[i].owner, owner) == 0) {
            if (updated_ms > g_quests[i].updated_ms) {
                strncpy(g_quests[i].text, text, QUEST_TEXT_LEN - 1);
                g_quests[i].text[QUEST_TEXT_LEN - 1] = '\0';
                g_quests[i].done       = done;
                g_quests[i].updated_ms = updated_ms;
            }
            return;
        }
    }
    if (g_quest_count >= QUEST_MAX) return;
    int slot = g_quest_count++;
    strncpy(g_quests[slot].owner, owner, sizeof(g_quests[slot].owner) - 1);
    g_quests[slot].owner[sizeof(g_quests[slot].owner) - 1] = '\0';
    strncpy(g_quests[slot].text, text, QUEST_TEXT_LEN - 1);
    g_quests[slot].text[QUEST_TEXT_LEN - 1] = '\0';
    g_quests[slot].done       = done;
    g_quests[slot].updated_ms = updated_ms;
    g_quests[slot].uid        = uid;
}
