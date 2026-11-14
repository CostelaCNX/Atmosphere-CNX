// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstring>

namespace ams::usb::metadata {

    /* Positive metadata only. The owner serializes access and clears all
     * entries before mutations: FAT case/short-name aliases and renamed parent
     * directories make invalidating only the caller's spelling insufficient.
     * No handles, allocations, errors or device generations are retained. */
    template<typename Info, std::size_t PathCapacity, std::size_t Capacity = 64>
    class Cache {
        static_assert(PathCapacity > 1 && Capacity > 0);
        struct Entry {
            char path[PathCapacity]{};
            Info info{};
            bool valid{};
        };
        Entry entries[Capacity]{};
        std::size_t next{};

        public:
            bool Find(const char *path, Info *out) const {
                if (!path || !out) return false;
                for (const auto &entry : entries) {
                    if (entry.valid && std::strcmp(entry.path, path) == 0) {
                        *out = entry.info;
                        return true;
                    }
                }
                return false;
            }

            bool Store(const char *path, const Info &info) {
                if (!path) return false;
                std::size_t length = 0;
                while (length < PathCapacity && path[length]) ++length;
                if (length == 0 || length == PathCapacity) return false;
                for (auto &entry : entries) {
                    if (entry.valid && std::strcmp(entry.path, path) == 0) {
                        entry.info = info;
                        return true;
                    }
                }
                auto &entry = entries[next];
                std::memcpy(entry.path, path, length + 1);
                entry.info = info;
                entry.valid = true;
                next = (next + 1) % Capacity;
                return true;
            }

            bool Clear() {
                bool populated = false;
                for (auto &entry : entries) {
                    populated |= entry.valid;
                    entry.valid = false;
                }
                next = 0;
                return populated;
            }
    };

}
