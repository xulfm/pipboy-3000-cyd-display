#pragma once
// ============================================================
// PIP-BOY Firmware -- Theme / Branding Configuration
// ------------------------------------------------------------
// All user-facing branding lives here so the firmware can be
// re-themed without touching screen logic. Edit these macros to
// rebrand the device, vault, and organization.
//
// C string-literal concatenation composes the full titles at
// compile time, e.g. BRAND_TITLE -> "VAULT 111 PIP-BOY 3000".
// ============================================================

// ── Core identity ─────────────────────────────────────────────
#define ORG_NAME      "VAULT-TEC"        // organization (was "PDI")
#define DEVICE_NAME   "PIP-BOY 3000"     // the handheld device
#define VAULT_NUM     "111"              // vault number (change per build)
#define VAULT_ID      "VAULT " VAULT_NUM // "VAULT 111"

// ── Composed titles ───────────────────────────────────────────
#define BRAND_TITLE   VAULT_ID " " DEVICE_NAME   // "VAULT 111 PIP-BOY 3000"
#define BRAND_TITLE_BANNER  "** " BRAND_TITLE " **"

// Per-screen headers (kept as macros so they stay consistent).
#define TITLE_MAINMENU  BRAND_TITLE_BANNER
#define TITLE_LOGIN     VAULT_ID " -- " DEVICE_NAME
#define TITLE_SETTINGS  "** " VAULT_ID " SETTINGS **"
#define TITLE_CHRONO    "** " VAULT_ID " CHRONOMETER **"
#define TITLE_CHRONO_STATUS  VAULT_ID " CHRONOMETER"
#define TITLE_WIFI      VAULT_ID " -- WIFI SETUP"
#define TITLE_APPS      "** " ORG_NAME " APPLICATIONS **"

// Mesh / peer wording.
#define MESH_BRAND      DEVICE_NAME " MESH"
#define PEER_PLURAL     "PIP-BOYS"

// Dweller affiliation line shown on the STAT screen.
#define DWELLER_AFFIL   VAULT_ID " DWELLER"

// ── Temperature-probe feature (was "GRILL") ───────────────────
// The BLE thermometer feature is generic temperature monitoring.
#define PROBE_FEATURE_NAME  "TEMP PROBE"
#define PROBE_MONITOR_TITLE "PROBE MONITOR"
