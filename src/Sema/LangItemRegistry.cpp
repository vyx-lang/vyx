#include "LangItemRegistry.h"

namespace vyx {

void LangItemRegistry::registerItem(const std::string& slot,
                                    const Decl* decl,
                                    DiagnosticsEngine& diag,
                                    SourceLocation loc) {
    if (slot.empty() || decl == nullptr) return;

    auto it = byName_.find(slot);
    if (it != byName_.end()) {
        // Duplicate registration: the first winner wins.  Emit a warning
        // so the user can de-dup explicitly (two stdlib modules both
        // claiming `@[lang_item("string")]` is almost always a library
        // configuration mistake rather than an intentional override).
        diag.warning(loc,
            "duplicate @[lang_item(\"{}\")] - already bound to `{}`; this declaration is ignored",
            slot, it->second->name);
        return;
    }

    byName_.emplace(slot, decl);
    entries_.emplace_back(slot, decl);
}

const Decl* LangItemRegistry::find(const std::string& slot) const {
    auto it = byName_.find(slot);
    return it == byName_.end() ? nullptr : it->second;
}

std::string LangItemRegistry::slotOf(const Decl* decl) const {
    if (!decl) return {};
    for (auto& [slot, d] : entries_) {
        if (d == decl) return slot;
    }
    return {};
}

void LangItemRegistry::clear() {
    byName_.clear();
    entries_.clear();
}

} // namespace vyx
