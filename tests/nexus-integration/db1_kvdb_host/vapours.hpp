#pragma once
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace ams {
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s64 = int64_t;
using Result = int;
inline unsigned failure_epoch = 0;
inline Result ResultSuccess() { return 0; }
inline Result ReturnResult(Result value) { if (value) ++failure_epoch; return value; }
template<int Code> struct Error {
    constexpr operator Result() const { return Code; }
    static bool Includes(Result result) { return result == Code; }
};
namespace kvdb {
using ResultAllocationFailed = Error<0x814>;
using ResultOutOfKeyResource = Error<0x214>;
using ResultKeyNotFound = Error<0x414>;
using ResultInvalidKeyValue = Error<0x614>;
}
namespace util {
template<class T> using is_pod = std::conjunction<std::is_trivial<T>, std::is_standard_layout<T>>;
inline size_t Strnlen(const char *s, size_t n) { size_t length = 0; while (length < n && s[length]) ++length; return length; }
inline size_t Strlcpy(char *dst, const char *src, size_t n) { const auto size = strlen(src); if(n) { const auto take = std::min(size, n-1); memcpy(dst, src, take); dst[take]=0; } return size; }
inline int VSNPrintf(char *dst, size_t n, const char *fmt, va_list args) { return vsnprintf(dst, n, fmt, args); }
inline int Strncmp(const char *a, const char *b, size_t n) { return strncmp(a,b,n); }
}
class MemoryResource {
public:
    virtual ~MemoryResource() = default;
    virtual void *Allocate(size_t) = 0;
    virtual void Deallocate(void *, size_t) = 0;
};
template<class F> struct Guard {
    F function;
    bool failure_only;
    unsigned epoch;
    ~Guard() { if (!failure_only || epoch != failure_epoch) function(); }
};
struct GuardMaker { bool failure_only; template<class F> auto operator+(F f) const { return Guard<F>{f, failure_only, failure_epoch}; } };
}
#define NON_COPYABLE(T) T(const T &) = delete; T &operator=(const T &) = delete
#define NON_MOVEABLE(T) T(T &&) = delete; T &operator=(T &&) = delete
#define AMS_ABORT_UNLESS(expr) do { if (!(expr)) std::abort(); } while (0)
#define R_ABORT_UNLESS(expr) AMS_ABORT_UNLESS((expr) == 0)
#define R_RETURN(expr) return ams::ReturnResult((expr))
#define R_SUCCEED() return 0
#define R_UNLESS(expr, result) do { if (!(expr)) { R_RETURN(result); } } while (0)
#define R_TRY(expr) do { const ams::Result result_ = (expr); if (result_) { R_RETURN(result_); } } while (0)
#define R_DISCARD(expr) static_cast<void>((expr))
#define R_TRY_CATCH(expr) { const ams::Result caught_result_ = (expr); if (caught_result_ != 0)
#define R_CONVERT(type, value) if (type::Includes(caught_result_)) { R_RETURN(value); }
#define R_END_TRY_CATCH if (caught_result_ != 0) { R_RETURN(caught_result_); } }
#define CONCAT_INNER(a,b) a##b
#define CONCAT(a,b) CONCAT_INNER(a,b)
#define ON_SCOPE_EXIT auto CONCAT(scope_guard_, __LINE__) = ams::GuardMaker{false} + [&]()
#define ON_RESULT_FAILURE auto CONCAT(failure_guard_, __LINE__) = ams::GuardMaker{true} + [&]()
