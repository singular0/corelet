#pragma once

#include <QtGlobal>
#include <cstddef>
#include <cstdint>

// Companion protocol constants, mirroring coreletd's src/companion/frames.h
// (which in turn mirrors the upstream MeshCore firmware). These are wire
// numbers: never renumber them, and only add at the end.
namespace proto {

enum Cmd : quint8 {
    CmdAppStart = 1,
    CmdSendTxtMsg = 2,
    CmdSendChannelTxtMsg = 3,
    CmdGetContacts = 4,
    CmdGetDeviceTime = 5,
    CmdSetDeviceTime = 6,
    CmdSendSelfAdvert = 7,
    CmdSetAdvertName = 8,
    CmdSyncNextMessage = 10,
    CmdSetRadioParams = 11,
    CmdSetRadioTxPower = 12,
    CmdSetAdvertLatLon = 14,
    CmdGetBatteryVoltage = 20,
    CmdSetTuningParams = 21,
    CmdDeviceQuery = 22,
    CmdGetContactByKey = 30,
    CmdGetChannel = 31,
    CmdSetChannel = 32,
    CmdGetTuningParams = 43,
};

enum Resp : quint8 {
    RespOk = 0,
    RespErr = 1,
    RespContactsStart = 2,
    RespContact = 3,
    RespEndOfContacts = 4,
    RespSelfInfo = 5,
    RespSent = 6,
    RespContactMsgRecv = 7,
    RespChannelMsgRecv = 8,
    RespCurrTime = 9,
    RespNoMoreMessages = 10,
    RespExportContact = 11,
    RespBatteryVoltage = 12,
    RespDeviceInfo = 13,
    RespPrivateKey = 14,
    RespDisabled = 15,
    RespContactMsgRecvV3 = 16,
    RespChannelMsgRecvV3 = 17,
    RespChannelInfo = 18,
    RespTuningParams = 23,
};

enum Push : quint8 {
    PushAdvert = 0x80,
    PushPathUpdated = 0x81,
    PushSendConfirmed = 0x82,
    PushMsgWaiting = 0x83,
    PushRawData = 0x84,
    PushLoginSuccess = 0x85,
    PushLoginFail = 0x86,
    PushStatusResponse = 0x87,
    PushLogRxData = 0x88,
    PushTraceData = 0x89,
    PushNewAdvert = 0x8A,
    PushTelemetryResponse = 0x8B,
};

enum Err : quint8 {
    ErrUnsupportedCmd = 1,
    ErrNotFound = 2,
    ErrTableFull = 3,
    ErrBadState = 4,
    ErrFileIoError = 5,
    ErrIllegalArg = 6,
};

// Everything from 0x80 up is an unsolicited push, which is what lets an
// untagged protocol work at all: anything below is the reply to whichever
// command is currently in flight.
constexpr bool isPush(quint8 code) { return code >= 0x80; }

inline constexpr quint8 FrameToDevice = '<';
inline constexpr quint8 FrameToApp = '>';
inline constexpr int MaxFrameSize = 8192;

inline constexpr int ChannelNameField = 32;
inline constexpr int ChannelSecretSize = 16;
inline constexpr int MaxChannels = 8;

inline constexpr int ContactNameField = 32;
// Only the first out_path_len bytes are meaningful, but the field is always
// here in full: reading out_path_len bytes instead leaves every later field of
// a contact frame short by the padding.
inline constexpr int ContactPathField = 64;

inline constexpr quint8 TxtPlain = 0;

// What this app tells DEVICE_QUERY it understands. Upstream firmware frames a
// received message in the V3 shape -- the only one parsed here -- only for an
// app that has said 3 or more, and decides when the message arrives, not when
// it is collected.
inline constexpr quint8 AppProtocolVersion = 3;

// The bounds SET_RADIO_PARAMS itself puts on a centre frequency, in MHz.
// coreletd's SX1262 stops at 960; upstream firmware also drives 2.4 GHz radios,
// so the command's own range is the honest one here and the far end refuses
// what its radio cannot tune.
inline constexpr double MinFreqMhz = 150.0;
inline constexpr double MaxFreqMhz = 2500.0;

// The only centre frequencies at which an app may turn repeat on, in kHz as
// SET_RADIO_PARAMS carries them: upstream's repeat_freq_ranges, which coreletd
// mirrors. Anywhere else the whole command is refused, frequency and all.
inline constexpr quint32 RepeatFreqsKhz[] = {433000, 869495, 918000};

constexpr bool repeatAllowedAt(quint32 freqKhz) {
    for (const quint32 allowed : RepeatFreqsKhz)
        if (allowed == freqKhz) return true;
    return false;
}

// SET_RADIO_TX_POWER's floor, the same on both ends. The ceiling is the
// radio's and SELF_INFO reports it.
inline constexpr int MinTxPowerDbm = -9;

// SET_TUNING_PARAMS carries the airtime factor in thousandths, and both ends
// hold it to 0-9: the node keeps quiet for that many times the airtime of each
// transmission, so the duty cycle is 100 / (1 + af) percent.
inline constexpr int MaxAirtimeFactorMilli = 9000;

// The hash a direct message's recipient answers with, carried by RESP_SENT and
// handed back by PUSH_SEND_CONFIRMED. Four bytes is short enough to collide --
// the firmware notes that the same ack can even arrive twice -- so it is only
// ever matched against sends this session started, never treated as an identity.
inline constexpr int AckHashSize = 4;

// How much text fits in those fields is in protocol/text_limits.h: it is a
// count of encoded bytes rather than a wire number, and derived from these.

}  // namespace proto
