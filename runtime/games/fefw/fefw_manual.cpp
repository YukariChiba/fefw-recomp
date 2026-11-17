// SPDX-License-Identifier: GPL-3.0-or-later
// Ten native save destinations for game update 131072. All I/O is performed by
// the game's own save and load routines; this module only selects a path.
#include "fefw_manual.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "play_selftest.h"

namespace nxrt::games::fefw::manual {
namespace {
enum Operation : uint32_t { Save, Suspend, Load, Quit };
struct Item {
    uint8_t native[0xa0];
    uint64_t origin;
    uint32_t slot;
    uint32_t operation;
    uint32_t valid;
    uint32_t pending;
    char label[128];
    uint8_t table[0x248];
};
static_assert(offsetof(Item, table) % 8 == 0);
struct State {
    uint64_t base = mem::kMainBase;
    bool enabled = false;
    bool native_quit = false;
    std::atomic<int> selected{1};
    std::array<uint32_t, 48> hooks{};
    size_t count = 0;
};
State& st() { static State s; return s; }
thread_local bool t_original_factory = false;
thread_local bool t_adding_quit = false;
thread_local uint64_t t_scratch = 0;

template<class T> bool read(uint64_t a, T* out) {
    if (!a || !mem::is_mapped(a, sizeof(T))) return false;
    std::memcpy(out, reinterpret_cast<const void*>(a), sizeof(T));
    return true;
}
template<class T> bool write(uint64_t a, const T& value) {
    if (!a || !mem::is_mapped(a, sizeof(T))) return false;
    std::memcpy(reinterpret_cast<void*>(a), &value, sizeof(T));
    return true;
}
uint64_t scratch() {
    if (!t_scratch) t_scratch = mem::alloc_kernel(0x1000);
    return t_scratch;
}
unsigned path(int slot, char out[13]) {
    if (slot < 0 || slot > 10) return 0;
    if (slot == 0) { std::memcpy(out, "000/Auto", 9); return 8; }
    const int n = std::snprintf(out, 13, "000/Manual%d", slot);
    return n > 0 && n < 13 ? static_cast<unsigned>(n) : 0;
}
bool guest_path(int slot, uint64_t* data, uint64_t* view) {
    const uint64_t p = scratch();
    if (!p || !mem::is_mapped(p, 0x200)) return false;
    char name[13]{};
    const unsigned n = path(slot, name);
    if (!n) return false;
    std::memcpy(reinterpret_cast<void*>(p), name, n + 1);
    // Native ManualView is {data, length}, in that order.
    write(p + 0x20, p);
    write(p + 0x28, static_cast<uint64_t>(n));
    *data = p;
    *view = p + 0x20;
    return true;
}
uint64_t call(::Ctx* c, uint64_t off, uint64_t x0 = 0, uint64_t x1 = 0,
              uint64_t x2 = 0, uint64_t x8 = 0, uint64_t x3 = 0) {
    ::Ctx nested = *c;
    nested.x[0] = x0; nested.x[1] = x1; nested.x[2] = x2;
    nested.x[3] = x3;
    if (x8) nested.x[8] = x8;
    dispatch_call(&nested, st().base + off);
    return nested.x[0];
}
bool is_ours(uint64_t a) {
    uint64_t vtable = 0;
    uint32_t slot = 99, operation = 99;
    return read(a, &vtable) && vtable == a + offsetof(Item, table) + 0x10 &&
           read(a + offsetof(Item, slot), &slot) && slot <= 10 &&
           read(a + offsetof(Item, operation), &operation) && operation <= Quit;
}
uint64_t parent(uint64_t item) {
    uint64_t binding = 0, nested = 0;
    if (!read(item + 8, &binding) || !binding) return 0;
    if (read(binding + 0x98, &nested) && nested) return nested;
    return binding;
}
bool summary(::Ctx* c, int slot, unsigned* part, unsigned* chapter) {
    uint64_t data = 0, view = 0;
    if (!guest_path(slot, &data, &view) || !fn_lookup(st().base + 0x880700)) return false;
    const uint64_t out = scratch() + 0x40;
    std::memset(reinterpret_cast<void*>(out), 0, 0x70);
    call(c, 0x880700, view, 0, 0, out);
    uint8_t bytes[0x70]{};
    if (!read(out, &bytes) || bytes[0] < 3) return false;
    *part = bytes[0xb]; *chapter = bytes[0xc];
    return true;
}
void update_label(::Ctx* c, Item* item) {
    if (item->operation == Quit) {
        std::snprintf(item->label, sizeof item->label, "%s",
                      item->pending ? "再次选择以确认结束游戏" : "结束游戏");
        return;
    }
    unsigned part = 0, chapter = 0;
    item->valid = summary(c, static_cast<int>(item->slot), &part, &chapter);
    const char* name = item->slot == 0 ? "自动存档" : "手动存档";
    if (item->valid) {
        if (item->slot) std::snprintf(item->label, sizeof item->label,
                                     "%s %u：第%u部 第%u章", name, item->slot, part, chapter);
        else std::snprintf(item->label, sizeof item->label,
                           "%s：第%u部 第%u章", name, part, chapter);
    } else {
        if (item->slot) std::snprintf(item->label, sizeof item->label,
                                     "%s %u：空", name, item->slot);
        else std::snprintf(item->label, sizeof item->label, "%s：空", name);
        if (item->operation == Load) item->native[0x51] = item->native[0x52] = 0;
    }
}
uint64_t make_item(::Ctx* c, uint64_t origin, Operation op, unsigned slot) {
    uint64_t heap_ref = 0, heap = 0;
    const uint64_t b = st().base;
    if (!read(b + 0x2169fa8, &heap_ref) ||
        !read(heap_ref, &heap) || !heap ||
        !mem::is_mapped(b + 0x2128c78, 0x248)) return 0;
    const uint64_t a = call(c, 0x652310, heap, sizeof(Item), 0x10, 0, 0);
    if (!a || !mem::is_mapped(a, sizeof(Item))) return 0;
    auto* item = reinterpret_cast<Item*>(a);
    std::memset(item, 0, sizeof(Item));
    std::memcpy(item->table, reinterpret_cast<const void*>(b + 0x2128c78), 0x248);
    write(a, a + offsetof(Item, table) + 0x10);
    // The copied Config row still points at its own "environment settings"
    // callbacks. Replace only these three virtual methods for our rows.
    write(a + offsetof(Item, table) + 0x10 + 0x10, b + 0x8755b0);
    write(a + offsetof(Item, table) + 0x10 + 0xc0, b + 0x874ca0);
    write(a + offsetof(Item, table) + 0x10 + 0xc8, b + 0x874cb0);
    // The native Config row leaves byte +0x53 clear; setting it marks the row
    // for a different UI lifecycle and can dispatch every row on transition.
    const uint32_t enabled = 0x00010101;
    write(a + 0x50, enabled);
    item->origin = origin;
    item->slot = slot; item->operation = op;
    update_label(c, item);
    if (slot <= 1) NXLOG_I("manual", "made slot %u valid=%u text=%s", slot, item->valid, item->label);
    return a;
}
uint64_t chooser(::Ctx* c, uint64_t p, uint64_t origin, Operation op) {
    const uint64_t s = scratch();
    if (!p || !s || !mem::is_mapped(s + 0x300, 24)) return 0;
    const std::array<uint64_t, 3> info{origin, 1, 0};
    write(s + 0x300, info);
    const uint64_t menu = call(c, 0x171fa20, p, s + 0x300);
    if (!menu) return 0;
    unsigned added = 0;
    for (unsigned i = 0; i < 11; ++i) {
        const unsigned slot = op == Load ? i : (i == 10 ? 0 : i + 1);
        const uint64_t row = make_item(c, p, op, slot);
        if (row) { call(c, 0x1714cc0, menu, row); ++added; }
    }
    NXLOG_I("manual", "slot chooser operation %u: %u rows", unsigned(op), added);
    return menu;
}
int save_factory(::Ctx* c, uint64_t site, void*) {
    if (t_original_factory) return 0;
    const uint64_t menu = chooser(c, c->x[0], 0,
                                  site == st().base + 0x16dabf0 ? Suspend : Save);
    if (!menu) return 0;
    c->x[0] = menu;
    return 1;
}
int title_continue(::Ctx* c, uint64_t, void*) {
    const uint64_t item = c->x[0], p = parent(item);
    uint8_t enabled = 0;
    if (!p || !read(item + 0x52, &enabled) || enabled != 1) return 0;
    if (!chooser(c, p, item, Load)) return 0;
    c->x[0] = 12;
    return 1;
}
void title_enabled(::Ctx* c, uint64_t site, void*) {
    if (st().enabled) { c->x[0] = 1; return; }
    ::Ctx nested = *c;
    nested.x[30] = site + 4;
    dispatch_call(&nested, st().base + 0x87f0e0);
    c->x[0] = nested.x[0];
}
int row_label(::Ctx* c, uint64_t, void*) {
    static std::atomic<unsigned> seen{0};
    if (seen.fetch_add(1) < 3) {
        uint64_t table = 0;
        read(c->x[0], &table);
        NXLOG_I("manual", "label callback row=0x%llx table=0x%llx",
                static_cast<unsigned long long>(c->x[0]),
                static_cast<unsigned long long>(table));
    }
    if (!is_ours(c->x[0])) return 0;
    c->x[0] += offsetof(Item, label);
    return 1;
}
int row_resume(::Ctx* c, uint64_t, void*) {
    const uint64_t a = c->x[0];
    if (!is_ours(a)) return 0;
    uint64_t native_resume = 0;
    if (read(st().base + 0x2128c78 + 0x10 + 0xc0, &native_resume) &&
        native_resume && native_resume != st().base + 0x874ca0 &&
        fn_lookup(native_resume)) {
        ::Ctx nested = *c;
        nested.x[0] = a;
        dispatch_call(&nested, native_resume);
    }
    auto* item = reinterpret_cast<Item*>(a);
    if (item->operation == Quit) return 1;
    const uint64_t p = parent(a);
    uint8_t busy = 1;
    if (item->pending && p && read(p + 0x8a, &busy) && !busy) {
        item->pending = 0;
        update_label(c, item);
        call(c, 0x1720330, a);
    }
    return 1;
}
int row_confirm(::Ctx* c, uint64_t, void*) {
    const uint64_t a = c->x[0];
    if (!is_ours(a)) return 0;
    auto* item = reinterpret_cast<Item*>(a);
    // BasicSubMenu also invokes this vtable slot for every row while closing.
    // Only the selection dispatcher may start a save or load operation.
    if (c->x[30] == st().base + 0x171485c) { c->x[0] = 1; return 1; }
    NXLOG_I("manual", "confirm operation=%u slot=%u valid=%u enabled=%u",
            item->operation, item->slot, item->valid, item->native[0x52]);
    const uint64_t p = parent(a);
    if (item->native[0x52] != 1 || !p) { c->x[0] = 1; return 1; }
    if (item->operation == Quit) {
        if (item->pending) {
            nxrt::play::exit_process(0, "pass", "native system menu quit");
        }
        item->pending = 1;
        update_label(c, item);
        call(c, 0x1720330, a);
        c->x[0] = 1;
        return 1;
    }
    if (item->operation == Load) {
        uint64_t data = 0, view = 0;
        if (!item->valid || !guest_path(item->slot, &data, &view)) {
            c->x[0] = 1; return 1;
        }
        const uint64_t fn = scratch() + 0x100;
        std::array<uint64_t, 6> words{};
        words[0] = st().base + 0x20c30e8;
        words[4] = fn;
        write(fn, words);
        const uint64_t started = call(c, 0x881540, p, view, fn);
        NXLOG_I("manual", "native load slot=%u result=0x%llx parent=0x%llx",
                item->slot, static_cast<unsigned long long>(started),
                static_cast<unsigned long long>(p));
        if (!started) {
            c->x[0] = 1; return 1;
        }
    } else {
        st().selected.store(static_cast<int>(item->slot));
        item->pending = 1;
        t_original_factory = true;
        call(c, item->operation == Suspend ? 0x16dabf0 : 0x16da940, p);
        t_original_factory = false;
    }
    c->x[0] = 12;
    return 1;
}
void task_append(::Ctx* c, uint64_t site, void*) {
    const uint64_t vector = c->x[0], action = c->x[1], type = c->x[2];
    uint64_t old_end = 0, outer = 0;
    const bool had_end = read(vector + 8, &old_end);
    read(c->x[29] + 8, &outer);
    ::Ctx nested = *c;
    nested.x[30] = site + 4;
    dispatch_call(&nested, st().base + 0x87bf30);
    c->x[0] = nested.x[0];
    uint64_t new_end = 0;
    const uint64_t caller = outer & 0x0000ffffffffffffull;
    if (!st().enabled || !had_end || !read(vector + 8, &new_end) || new_end == old_end ||
        action != 1 || type != 2 ||
        (caller != st().base + 0x16dc954 && caller != st().base + 0x16dd364)) return;
    uint64_t data = 0, view = 0;
    if (guest_path(st().selected.load(), &data, &view))
        call(c, 0x60a020, new_end - 0x20 + 8, data);
}
void descriptor(::Ctx* c, uint64_t site, void*) {
    uint64_t type = c->x[0], index = c->x[1], original_path = c->x[2];
    int slot = st().selected.load();
    if (!st().enabled) slot = -1;
    else if (site == st().base + 0x16dd8f4) {
        uint64_t loader = 0;
        if (read(c->x[19] + 8, &loader) && loader) {
            const uint64_t s = loader + 0x98;
            uint8_t head = 0;
            if (read(s, &head)) {
                uint64_t len = head >> 1, data = s + 1;
                if (head & 1) { read(s + 8, &len); read(s + 16, &data); }
                slot = -1;
                if (len <= 12 && mem::is_mapped(data, len)) {
                    for (int i = 0; i <= 10; ++i) {
                        char name[13]{};
                        if (path(i, name) == len &&
                            !std::memcmp(reinterpret_cast<const void*>(data), name, len)) {
                            slot = i; break;
                        }
                    }
                }
            }
        }
    } else if (type != 2 || static_cast<int32_t>(index) != -1 || original_path)
        slot = -1;
    uint64_t data = 0, view = 0;
    if (slot >= 0 && guest_path(slot, &data, &view)) {
        type = slot ? 3 : 2;
        index = slot ? static_cast<uint64_t>(slot - 1) : ~0ull;
        original_path = data;
    }
    ::Ctx nested = *c;
    nested.x[0] = type; nested.x[1] = index; nested.x[2] = original_path;
    nested.x[30] = site + 4;
    dispatch_call(&nested, st().base + 0x87e7e0);
    c->x[0] = nested.x[0];
}
void remember(uint32_t id) {
    auto& s = st();
    if (id && s.count < s.hooks.size()) s.hooks[s.count++] = id;
}
int append_quit(::Ctx* c, uint64_t, void*) {
    if (!st().native_quit || t_adding_quit || c->x[30] != st().base + 0x188bd30)
        return 0;
    const uint64_t menu = c->x[0];
    const uint64_t row = make_item(c, menu, Quit, 0);
    if (!row) return 0;
    t_adding_quit = true;
    call(c, 0x1714cc0, menu, row);
    t_adding_quit = false;
    NXLOG_I("manual", "native system quit row appended");
    return 0;
}
}  // namespace

void install(uint64_t base) {
    auto& s = st();
    s.base = base;
    s.enabled = config_get_int("coremods", "manual_slots", 0) == 1;
    s.native_quit = config_get_int("coremods", "native_quit", 0) == 1;
    // Guest functions are published after game-hook installation. Check only
    // at the point of use, after the lifted module is registered.
    remember(hook_add_inline(base + 0x16d3068, title_enabled));
    remember(hook_add_inline(base + 0x880bcc, task_append));
    remember(hook_add_inline(base + 0x16dc988, descriptor));
    remember(hook_add_inline(base + 0x16dd398, descriptor));
    remember(hook_add_inline(base + 0x16dd8f4, descriptor));
    if (s.native_quit) remember(hook_add_entry(base + 0x1714cc0, append_quit));
    if (!s.enabled && !s.native_quit) {
        NXLOG_I("manual", "native save calls retained; extra slots disabled");
        return;
    }
    if (s.enabled) {
        remember(hook_add_entry(base + 0x16da940, save_factory));
        remember(hook_add_entry(base + 0x16dabf0, save_factory));
        remember(hook_add_entry(base + 0x16d4ae0, title_continue));
    }
    remember(hook_add_entry(base + 0x8755b0, row_label));
    remember(hook_add_entry(base + 0x874ca0, row_resume));
    remember(hook_add_entry(base + 0x874cb0, row_confirm));
    NXLOG_I("manual", "native menu extensions: saves=%u quit=%u", s.enabled, s.native_quit);
}
void remove() {
    auto& s = st();
    while (s.count) hook_remove(s.hooks[--s.count]);
}
bool enabled() { return st().enabled; }
}  // namespace nxrt::games::fefw::manual
