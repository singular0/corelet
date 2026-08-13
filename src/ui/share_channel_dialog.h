#pragma once

#include <QDialog>

#include "model/types.h"

// Shows a channel's key, to copy and hand to whoever is being invited.
//
// A channel is nothing but its key, so this is the whole of "invite someone":
// there is no membership to grant and nothing to ask the device for.
class ShareChannelDialog : public QDialog {
    Q_OBJECT

public:
    // `channel` must carry its key, which means it came from the device rather
    // than from the offline cache.
    explicit ShareChannelDialog(const model::Channel& channel, QWidget* parent = nullptr);
};
