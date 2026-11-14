#include <cstdio>
#include <cstdlib>
#include "ncm_paged_content_meta_database_impl.hpp"
#include "ncm_content_meta_database_impl.hpp"

using namespace ams;
using namespace ams::ncm;

/* Exact layout documented by Nexus Manager's legacy forwarder recognition:
 * 8-byte header, 16-byte extension, three 24-byte content infos, no dependency.
 * The old generator incorrectly declared one 16-byte dependency. */
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const int expected_paged = std::atoi(argv[1]);
    kvdb::MemoryKeyValueStore<ContentMetaKey> memory;
    ContentMetaDatabaseImpl old(&memory, "db", StorageId::SdCard);
    PagedContentMetaDatabaseImpl paged(StorageId::SdCard);
    if (paged.Initialize("db:/", "db", fs::SaveDataSpaceId::SdSystem, 1) != 0) return 3;
    ContentMetaKey key{0x0100000012340000ULL, 0, ContentMetaType::Application, ContentInstallType::Full, {}};
    struct Legacy { ContentMetaHeader header; ApplicationMetaExtendedHeader extended; ContentInfo infos[3]; } value{};
    static_assert(sizeof(value) == 0x60);
    value.header = {16, 3, 1, 5, ContentMetaPlatform::Nx};
    value.extended = {{key.id | 0x800}, 123, 456};
    value.infos[0].type = ContentType::Meta;
    value.infos[1].type = ContentType::Program;
    value.infos[2].type = ContentType::Control;
    if (old.Set(key, {&value, sizeof(value)}) != 0 || paged.Set(key, {&value, sizeof(value)}) != 0) return 4;
    bool passed = true;
    auto check = [&](const char *name, Result a, Result b) {
        std::printf("%s legacy=%d paged=%d\n", name, int(a), int(b));
        passed &= int(a) == 0 && int(b) == expected_paged;
    };
    std::printf("Legacy forwarder stored=96 declared_end=112; host error 390 = production 0x30C05 (2005-0390)\n");
    s32 total{}, written{}; ApplicationContentMetaKey apps[2]{}; u8 attrs{}; ContentInfo info{}; u32 version{}; ContentMetaPlatform platform{};
    check("ListApplication", old.ListApplication(&total, &written, {apps, 2}, ContentMetaType::Unknown), paged.ListApplication(&total, &written, {apps, 2}, ContentMetaType::Unknown));
    check("GetAttributes", old.GetAttributes(&attrs, key), paged.GetAttributes(&attrs, key));
    check("HasAttributes", old.HasAttributes(&attrs, 7), paged.HasAttributes(&attrs, 7));
    check("GetPlatform", old.GetPlatform(&platform, key), paged.GetPlatform(&platform, key));
    check("GetContentInfoByType", old.GetContentInfoByType(&info, key, ContentType::Program), paged.GetContentInfoByType(&info, key, ContentType::Program));
    check("GetRequiredSystemVersion", old.GetRequiredSystemVersion(&version, key), paged.GetRequiredSystemVersion(&version, key));
    ContentMetaInfo missing{};
    const Result missing_result = paged.ListContentMetaInfo(&written, {&missing, 1}, key, 0);
    std::printf("Requested missing dependency paged=%d (must remain 390)\n", int(missing_result));
    passed &= missing_result == ResultInvalidContentMetaFileSize();
    Legacy raw{}; u64 size{};
    passed &= paged.Get(&size, key, {&raw, sizeof(raw)}) == 0 && size == sizeof(raw) && std::memcmp(&raw, &value, sizeof(raw)) == 0;
    std::printf("Raw record unchanged; expected before/after behavior %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
