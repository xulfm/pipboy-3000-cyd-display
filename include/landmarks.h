#pragma once
#include <stdint.h>
// ============================================================
// PIP-BOY -- Wasteland landmarks
// Fixed map locations a device can "anchor" to (Settings ->
// act as anchor). Coordinates are map-pixel centers and MUST
// match the boxes drawn in screen_map.cpp. Used both for the
// anchor picker and for RSSI multilateration of roaming peers.
// Map drawing space is ~316 wide x ~180 tall.
// ============================================================

struct Landmark {
    const char* name;
    int16_t     x;
    int16_t     y;
};

// Well-spread settlements/landmarks (hostile zones like the raider
// camp are intentionally excluded). VAULT is the home/center anchor.
static const Landmark LANDMARKS[] = {
    {"VAULT",       160,  75},
    {"SANCTUARY",    46,  34},
    {"RED ROCKET",   46,  68},
    {"TRADE POST",  268,  38},
    {"POWER PLANT", 268,  66},
    {"WATCHTOWER",  270,   9},
    {"BUNKER",      160, 130},
    {"DINER",       268, 130},
    {"SCRAPYARD",    48, 158},
    {"OUTPOST",     268, 158},
};
static const int LANDMARK_COUNT =
    (int)(sizeof(LANDMARKS) / sizeof(LANDMARKS[0]));
