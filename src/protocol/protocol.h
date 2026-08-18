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
    CmdGetBatteryVoltage = 20,
    CmdDeviceQuery = 22,
    CmdGetContactByKey = 30,
    CmdGetChannel = 31,
    CmdSetChannel = 32,
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

// How much text fits in those fields is in protocol/text_limits.h: it is a
// count of encoded bytes rather than a wire number, and derived from these.

}  // namespace proto
