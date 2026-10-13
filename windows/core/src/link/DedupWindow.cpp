#include "md/link/LinkSession.h"

namespace md::link {

bool DedupWindow::accept(uint64_t id) {
    if (id == 0) return false;
    if (highest_ >= cap_ && id <= highest_ - cap_) return false; // too old to tell: treat as duplicate
    if (!seen_.insert(id).second) return false;
    if (id > highest_) highest_ = id;
    while (seen_.size() > cap_) seen_.erase(seen_.begin());
    return true;
}

void DedupWindow::reset() {
    highest_ = 0;
    seen_.clear();
}

} // namespace md::link
