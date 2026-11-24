#pragma once

// SPDX-License-Identifier: GPL-2.0-only
//
// AveraUI — a reusable Qt 6 Quick "10-foot" media shell.
//
// The library is deliberately free of any player dependency: a host application
// fills an AveraHub with cards, implements AveraPlayerBackend for its own
// engine, and pushes decoded frames / PCM into AveraVideoItem and
// AveraAudioOutput. Nothing here knows what a UMD is.

#include <QString>

namespace avera {

/// Semantic version of the library, "major.minor.patch".
QString version();

/// Register the QML types (Avera.UI 1.0) with the Qt meta-object system.
/// Call once, before loading any QML that uses them.
void registerTypes();

/// Register the fonts bundled with the library (Plus Jakarta Sans). Call once,
/// after the QGuiApplication exists and before loading QML.
void loadFonts();

}  // namespace avera
