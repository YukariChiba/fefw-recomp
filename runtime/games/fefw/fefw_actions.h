// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::games::fefw::actions {

enum class ItemCategory : uint8_t { Weapon, Cooking, Upgrade, Gift, Growth, Other, Count };

struct Item {
    std::string iid;
    std::string name;
    std::string search_text;
    int item_index = -1;
    ItemCategory category = ItemCategory::Other;
};

void install(uint64_t main_base);
void remove();

const std::vector<Item>& item_catalog();
bool queue_item(const std::string& iid, int quantity);
bool queue_gift(int amount);
void cancel_item();
void cancel_gift();
std::string item_status();
std::string gift_status();
// The one score event explicitly replaced by the F1 gift action must not be
// multiplied again by the standing support-rate control.
bool consume_gift_override();

}  // namespace nxrt::games::fefw::actions
