#pragma once

#include <QString>
#include <QtGlobal>

namespace model {

// One region's radio settings, in the units SET_RADIO_PARAMS carries so a
// preset reaches the wire exactly: kHz for the centre frequency, whole hertz for
// the bandwidth.
struct RadioPreset {
    const char* title;
    quint32 freqKhz;
    quint32 bwHz;
    int sf;
    int cr;

    double freqMhz() const { return freqKhz / 1000.0; }
    double bwKhz() const { return bwHz / 1000.0; }
};

// The region presets the official MeshCore app offers. As in MeshCore, presets
// are the app's business: neither the firmware nor coreletd has a band plan,
// and both store whatever they are sent. So this list is copied rather than
// invented, and a mesh's choice lives where every other client already looks.
//
// Taken from suggested_radio_settings in https://api.meshcore.nz/api/v1/config,
// which the official app reads at startup, on 2026-10-30, in its order and with
// its titles. Not fetched at runtime: the uConsole is as often off any network
// as on one, and a mesh radio is the thing it is using instead. Some entries
// there also carry a path-hash size, which is left out here: nothing in this app
// sets the path-hash mode, and coreletd answers its command as unsupported.
inline constexpr RadioPreset RadioPresets[] = {
    {"Australia", 915800, 250000, 10, 5},
    {"Australia (Narrow)", 916575, 62500, 7, 7},
    {"Australia (Mid)", 915075, 125000, 9, 5},
    {"Australia: SA, WA", 923125, 62500, 8, 8},
    {"Australia: QLD", 923125, 62500, 8, 5},
    {"Brazil", 923125, 62500, 8, 8},
    {"Canada", 910525, 62500, 7, 5},
    {"Costa Rica", 910525, 125000, 11, 5},
    {"EU/UK (Narrow)", 869618, 62500, 8, 8},
    {"EU/UK (Deprecated)", 869525, 250000, 11, 5},
    {"Czech Republic (Narrow)", 869432, 62500, 7, 5},
    {"EU 433MHz (Long Range)", 433650, 250000, 11, 5},
    {"EU 433MHz (Narrow)", 433650, 62500, 8, 8},
    {"Hungary", 869618, 62500, 7, 5},
    {"Netherlands", 869618, 62500, 7, 5},
    {"Netherlands (Limburg)", 869618, 62500, 8, 8},
    {"New Zealand (Narrow)", 917375, 62500, 7, 5},
    {"New Zealand (Gisborne)", 917375, 250000, 11, 5},
    {"Portugal 433", 433375, 62500, 9, 6},
    {"Portugal 868", 869618, 62500, 7, 6},
    {"Slovakia", 869618, 62500, 7, 5},
    {"Switzerland", 869618, 62500, 8, 8},
    {"USA", 910525, 62500, 7, 5},
    {"USA - Southern California", 927875, 62500, 7, 5},
    {"Vietnam (Narrow)", 920250, 62500, 8, 5},
    {"Vietnam (Deprecated)", 920250, 250000, 11, 5},
};

// The LoRa bandwidths, in the whole hertz apps send for them: 7800 for the
// SX1262's 7.81 kHz, 41700 for its 41.67. The far end snaps each to its own
// within 1%, so these are names for the radio's settings rather than exact
// values of them.
inline constexpr quint32 LoraBandwidthsHz[] = {
    7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000,
};

}  // namespace model
