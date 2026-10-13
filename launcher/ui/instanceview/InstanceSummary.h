// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QString>

class BaseInstance;

/// Short, human-readable facts about an instance, shared by the instance tiles,
/// the side panel, the "Continue playing" card and the instance search.
namespace InstanceSummary {

/// The middle-dot separator used to join short facts on one line.
QString separator();

/// Minecraft version, e.g. "1.21.1"; empty when unknown.
QString minecraftVersion(BaseInstance* instance);

/// Display name of the mod loader, e.g. "Fabric"; empty for vanilla.
QString modLoader(BaseInstance* instance);

/// "1.21.1 - Fabric" style summary (joined with separator()), "1.21.1", or empty.
QString versionLine(BaseInstance* instance);

/// Number of mod files in the instance's mods folder, disabled ones included.
int modCount(BaseInstance* instance);

/// Lower-case text the instance search matches against: name, group, version and loader.
QString searchText(BaseInstance* instance, const QString& group);

/// "just now", "5 minutes ago", "yesterday", "3 days ago", or a short date.
QString relativeTime(qint64 msecsSinceEpoch);

}  // namespace InstanceSummary
