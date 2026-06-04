#pragma once
// ============================================================
// PIP-BOY Firmware -- Data
// Generic Vault-Tec roster, quests, and inventory. All branding
// lives in theme_config.h; the per-user display name can be
// overridden at login (see g_custom_name / user_display_name()).
// ============================================================

#include "theme_config.h"

//  Colors (match HTML CSS vars)
#define CLR_BG     lv_color_hex(0x050f06)
#define CLR_GREEN  lv_color_hex(0x00e855)
#define CLR_DIM    lv_color_hex(0x00a83e)
#define CLR_DARK   lv_color_hex(0x005c22)
#define CLR_HEADER lv_color_hex(0x003510)
#define CLR_SEL    lv_color_hex(0x002a10)
#define CLR_GOLD   lv_color_hex(0xf5c518)
#define CLR_GOLDD  lv_color_hex(0xb8920f)
#define CLR_RAD    lv_color_hex(0xc8ff00)
#define CLR_HI     lv_color_hex(0x80ffb0)

//  Team member struct 
struct Dweller {
    const char* init;
    const char* name;
    const char* title;
    uint8_t saf, prc, eff, crf, inn, adp, ldr;
    const char* p1;
    const char* p2;
    const char* p3;
    uint8_t hp;      // 0-100
    uint8_t morale;  // 0-100
    uint8_t rad;     // 0-100
    uint8_t hunger;  // 0-100
    bool    female;  // false=male, true=female
};

// Generic Vault-Tec dweller archetypes. Pick one at login; the
// display name can then be overridden with a custom name. Stats are
// saf/prc/eff/crf/inn/adp/ldr (Vault aptitude scores, 0-10).
static const Dweller team[] = {
    {"OV","OVERSEER",     "VAULT ADMIN",     9,  9,  9,  7,  8,  8,  10, "CHAIN OF COMMAND",  "VAULT AUTHORITY",   DWELLER_AFFIL, 95, 92, 2,  20, false},
    {"WN","WANDERER",     "LONE SURVIVOR",   9,  9,  8,  8,  8,  10, 7,  "WASTELAND SCOUT",   "LIGHT STEP",        DWELLER_AFFIL, 92, 85, 14, 38, false},
    {"MD","VAULT MEDIC",  "FIELD DOCTOR",    8,  9,  9,  7,  9,  8,  8,  "FIELD SURGEON",     "CHEM RESISTANT",    DWELLER_AFFIL, 90, 88, 6,  28, true},
    {"SC","SCRIBE",       "ARCHIVIST",       7,  10, 9,  8,  10, 7,  7,  "RESEARCHER",        "DATA RECOVERY",     DWELLER_AFFIL, 88, 86, 5,  30, true},
    {"RG","RANGER",       "RECON SCOUT",     10, 10, 8,  8,  7,  9,  8,  "EAGLE EYE",         "SURVIVAL EXPERT",   DWELLER_AFFIL, 93, 87, 9,  34, false},
    {"MK","MECHANIST",    "ENGINEER",        8,  8,  10, 9,  9,  8,  7,  "GEAR HEAD",         "REPAIR MASTER",     DWELLER_AFFIL, 91, 84, 7,  32, false},
    {"TK","TINKERER",     "TECH SPECIALIST", 7,  9,  9,  10, 10, 8,  6,  "JURY RIGGER",       "HACKER",            DWELLER_AFFIL, 89, 83, 8,  36, false},
    {"PR","PROSPECTOR",   "SCAVENGER",       8,  8,  9,  9,  7,  10, 6,  "PACK RAT",          "SCROUNGER",         DWELLER_AFFIL, 87, 82, 11, 40, false},
    {"SN","SENTRY",       "VAULT SECURITY",  10, 9,  8,  8,  6,  8,  8,  "TOUGHNESS",         "QUICK DRAW",        DWELLER_AFFIL, 94, 85, 6,  26, false},
    {"CR","COURIER",      "DISPATCH",        8,  9,  10, 7,  8,  10, 8,  "FLEET FOOTED",      "ROUTE PLANNER",     DWELLER_AFFIL, 90, 88, 5,  30, true},
    {"VT","VAULT-TEC REP","RECRUITER",       7,  9,  10, 6,  9,  10, 9,  "FAST TALKER",       "INSPIRATION",       DWELLER_AFFIL, 92, 90, 3,  24, true},
    {"NK","NIGHTKIN",     "WANDERING TRADER",8,  8,  8,  9,  8,  9,  7,  "BARTERER",          "STEADY HANDS",      DWELLER_AFFIL, 89, 84, 10, 35, false},
};
static const int TEAM_COUNT = 12;

// Current logged-in dweller index (set at login)
extern int g_dweller_idx;

// Optional per-user custom display name (overrides team[idx].name on
// the network + UI when non-empty). Persisted to NVS. Empty = use the
// selected archetype's name.
#define CUSTOM_NAME_LEN 20
extern char g_custom_name[CUSTOM_NAME_LEN];

// Returns the custom name if set, else the selected dweller's name.
// Single source of truth for "who am I" across UI + mesh frames.
const char* user_display_name(void);

// Persist / load the custom name (NVS namespace "user", key "name").
void user_name_save(const char* name);
void user_name_load(void);

//  Quest struct 
struct Quest {
    const char* name;
    const char* location;
    bool        main_quest;
    bool        done;
    uint16_t    xp_reward;
    const char* obj1;
    const char* obj2;
    const char* obj3;
    bool        obj1_done;
    bool        obj2_done;
    bool        obj3_done;
};

static Quest quests[] = {
    {"ESCAPE THE VAULT",            "VAULT DOOR",    true,  false, 500, "Reach the vault door",     "Override the lock",       "Step into the wastes",   true,  true,  false},
    {"SECURE CLEAN WATER",          "WATER PLANT",   true,  false, 300, "Locate the pump station",  "Replace the filter",      "Restore the supply",     true,  false, false},
    {"CLEAR THE RAIDERS",           "RUINED OVERPASS",false,false, 150, "Scout the camp",           "Disable their turrets",   "Drive them out",         true,  false, false},
    {"REPAIR THE GENERATOR",        "POWER STATION", false, false, 100, "Find spare parts",         "Splice the conduit",      "Bring it online",        false, false, false},
    {"MAP THE WASTELAND",           "OVERWORLD",     false, false, 200, "Mark 5 landmarks",         "Tag the settlements",     "Sync to the network",    true,  true,  false},
    {"TRADE WITH THE CARAVAN",      "MARKET RUINS",  false, true,  250, "Find the trade route",     "Barter for supplies",     "Establish a contact",    true,  true,  true},
    {"SCAVENGE FOR PARTS",          "SCRAPYARD",     false, false, 75,  "Salvage electronics",      "Recover a fusion cell",   "Stock the workbench",    true,  false, false},
    {"RADIATION CHECK",             "ANYWHERE",      false, false, 50,  "Monitor your rad level",   NULL,                      NULL,                     false, false, false},
};
static const int QUEST_COUNT = 8;

//  Inventory item struct 
struct InvItem {
    const char* icon;  // single char symbol
    const char* name;
    uint8_t     qty;
    const char* type;
    const char* condition;
    const char* bonus;
    const char* desc;
    bool        equipped;
};

// TOOLS / GEAR
static InvItem inv_tools[] = {
    {"Z", "Voltage Tester",     1, "TOOL",      "WORN",      "+2 PRC", "Non-contact AC/DC tester. Check before you touch.",  true},
    {"~", "Multi-Tool",         1, "TOOL",      "GOOD",      "+1 CRF", "Folding tool. Pliers, drivers, blade.",              false},
    {"=", "Vault Schematics",   3, "FIELD GEAR","CURRENT",   "+2 EFF", "Reactor + corridor blueprints. Rev D.",              true},
    {"O", "Combat Helmet",      1, "ARMOR",     "GOOD",      "+3 SAF", "Salvaged plate. " VAULT_ID " stencil.",              true},
    {"@", "Goggles",            2, "ARMOR",     "NEW",       "+2 SAF", "Sealed lenses. Keeps the grit out.",                 true},
    {"+", "Power Cable 100ft",  1, "TOOL",      "FAIR",      "+1 EFF", "Heavy-gauge run. Splices the dead zones.",           false},
    {"#", "Pack",               1, "CONTAINER", "GOOD",      "+1 ADP", "Reinforced rucksack. Hauls the haul.",               true},
    {".", "Survey Laser",       1, "FIELD GEAR","EXCELLENT", "+3 PRC", "Self-leveling 360 line laser. +/-1/8in at 33ft.",    false},
};
static const int TOOLS_COUNT = 8;

// PROVISIONS
static InvItem inv_provs[] = {
    {"*", "Grilled Radstag",    2, "FOOD",      "HOT",    "+15 HP",      "Spit-roasted over a scrap fire.",            false},
    {"^", "Roasted Corn",       1, "FOOD",      "HOT",    "+8 HP",       "Charred, salted. Wasteland staple.",         false},
    {"-", "Canned Slaw",        1, "FOOD",      "COLD",   "+5 HP",       "Pre-war tin. Still crunchy. Mostly.",        false},
    {"!", "Cold Brew",          6, "BEVERAGE",  "COLD",   "+10 MORALE",  "Ice cold. Limit 2 before a shift.",          false},
    {"~", "Purified Water",     3, "BEVERAGE",  "COLD",   "+5 HP",       "Filtered and rad-free. Stay hydrated.",      false},
    {"$", "Sweet Roll",         1, "DESSERT",   "FRESH",  "+20 MORALE",  "Frosted in phosphor-green icing.",           false},
    {">", "Hot Sauce",          2, "CONSUMABLE","SEALED", "+1 END",      "Proprietary " VAULT_ID " blend.",            false},
};
static const int PROVS_COUNT = 7;

// DOCUMENTS / HOLOTAPES
static InvItem inv_docs[] = {
    {"_", "Safety Protocol",    1, "RECORD",     "CURRENT",  "+5 SAF",    "Vault evacuation + reactor procedures.",     true},
    {"|", "Reactor Permit",     1, "PERMIT",     "APPROVED", "+3 SAF",    "Authorized for Level 3 access. Post it.",    true},
    {"%", "Maintenance Log",    1, "RECORD",     "REV 12",   "+3 EFF",    "Two cycles behind schedule. Shhh.",          false},
    {"?", "Distress Signal",    1, "TRANSMISSION","PENDING", "--",        "Repeating since day 14. Source unknown.",    false},
    {"&", "Field Report",       1, "RECORD",     "TODAY",    "+1 LDR",    "Crew count, work performed. File by dusk.",  false},
    {"T", "Service Citation",   1, "ACHIEVEMENT","FRAMED",   "+5 SAF",    "Zero incidents. " VAULT_ID " proud.",        true},
    {"M", "Wasteland Map",      1, "MAP",        "CURRENT",  "+2 ADP",    "Overworld layout. You are here.",            false},
};
static const int DOCS_COUNT = 7;