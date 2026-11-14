// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::usb_storage::system_power {

// Called on the optional USB monitor thread after MITM initialization and
// configuration, before publishing ams:usb. Never on Horizon's boot thread.
Result Initialize();
bool IsBackgroundPaused();
bool HasFailed();
u32 ReportRevision();
bool TryEnterBackground();
void LeaveBackground();
int Format(char *out, size_t capacity);

class BackgroundActivity {
    bool m_entered;
public:
    BackgroundActivity() : m_entered(TryEnterBackground()) {}
    ~BackgroundActivity() { if (m_entered) LeaveBackground(); }
    BackgroundActivity(const BackgroundActivity &) = delete;
    BackgroundActivity &operator=(const BackgroundActivity &) = delete;
    explicit operator bool() const { return m_entered; }
};

}
