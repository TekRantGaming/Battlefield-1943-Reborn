// Battlefield 1943 Reborn, SDK v0.11+ port (phase 4): v0.10's rex/system/achievement_store.h. v0.11's achievement
// manager (rex::xam::IAchievementManager, from the title's runtime) has the same calls and the same record
// (rex::Achievement, v0.10's AchievementInfo field for field).
#pragma once

#include <rex/title.h>
#include <rex/xam/achievement_manager.h>

namespace rex::system {
using AchievementInfo = ::rex::Achievement;
using AchievementEvent = ::rex::xam::AchievementEvent;
using AchievementNotification = ::rex::xam::AchievementNotification;
using AchievementUnlockResult = ::rex::xam::AchievementUnlockResult;
using AchievementStore = ::rex::xam::IAchievementManager;
}  // namespace rex::system
