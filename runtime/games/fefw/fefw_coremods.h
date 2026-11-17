// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

struct Ctx;

namespace nxrt::games::fefw::coremods {

// Rates are fifths: 5 means 1.0x. Growth bonus is in percentage points.
enum class Setting : uint8_t {
    GrowthMode, GrowthBonus, Experience, Support, Mount, Weapon, Skill,
    Fragment, Money, MoveCost, ClassSuccessBonus, ClassLevelAdjustment, SpecialClassCount,
    InfiniteWeapons, InfiniteMagic, InfiniteMedicine, InfiniteFood,
    QuickBird, QuickMeal, QuickTraining, QuickCapture, QuickService,
    RecruitLevelOne, PerfectExplore,
    UnlimitedWeeklyActivities, PreserveActivityFlames, Count
};

int get(Setting setting);
bool set(Setting setting, int value, std::string* error = nullptr);
int minimum(Setting setting);
int maximum(Setting setting);
int initial(Setting setting);
const char* label(Setting setting);

// Guest hooks are registered once when play mode starts. The settings are
// global to this PC installation and read live by the hooks.
void install(uint64_t main_base);
void remove();

int scale_positive(int delta, int fifths);
int fragment_total(int current, int delta, int fifths);

// Renown belongs to the loaded game save. The F1 menu reads a guest-frame
// snapshot and queues changes for the next guest frame.
bool renown_available();
int renown_current();
int renown_cap();
bool set_renown(int value);
void renown_frame(::Ctx* context);

}  // namespace nxrt::games::fefw::coremods
