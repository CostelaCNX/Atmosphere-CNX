#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
namespace ams {
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;using u64=uint64_t;using s32=int32_t;using s64=int64_t;using Result=int;
inline Result ResultSuccess(){return 0;}
template<int Code>struct Error {constexpr operator Result()const{return Code;}static bool Includes(Result rc){return rc==Code;}};
namespace kvdb {using ResultAllocationFailed=Error<0x814>;using ResultOutOfKeyResource=Error<0x214>;using ResultKeyNotFound=Error<0x414>;using ResultInvalidKeyValue=Error<0x614>;}
namespace util {
template<class T>using is_pod=std::conjunction<std::is_trivial<T>,std::is_standard_layout<T>>;
inline size_t Strnlen(const char*s,size_t n){size_t l=0;while(l<n&&s[l])++l;return l;}
inline size_t Strlcpy(char*d,const char*s,size_t n){const auto l=strlen(s);if(n){const auto t=std::min(l,n-1);memcpy(d,s,t);d[t]=0;}return l;}
inline int VSNPrintf(char*d,size_t n,const char*f,va_list a){return vsnprintf(d,n,f,a);}
inline int SNPrintf(char*d,size_t n,const char*f,...){va_list a;va_start(a,f);const auto r=vsnprintf(d,n,f,a);va_end(a);return r;}
inline int Strncmp(const char*a,const char*b,size_t n){return strncmp(a,b,n);}
template<class T>constexpr T AlignUp(T n,size_t a){return static_cast<T>((n+a-1)&~(a-1));}
}
template<class F>struct ScopeGuard {F function;bool enabled=true;~ScopeGuard(){if(enabled)function();}void Cancel(){enabled=false;}};
struct GuardMaker {template<class F>auto operator+(F f)const{return ScopeGuard<F>{f};}};
template<class F>struct ResultGuard {Result& result;F function;~ResultGuard(){if(result!=0)function();}};
struct ResultGuardMaker {Result& result;template<class F>auto operator+(F f)const{return ResultGuard<F>{result,f};}};
}
inline ams::Result __TmpCurrentResultReference=0;
#define NON_COPYABLE(T) T(const T&)=delete;T& operator=(const T&)=delete
#define NON_MOVEABLE(T) T(T&&)=delete;T& operator=(T&&)=delete
#define AMS_UNUSED(...) static_cast<void>((__VA_ARGS__))
#define AMS_ABORT_UNLESS(e) do{if(!(e))std::abort();}while(0)
#define AMS_ASSERT(e) assert(e)
#define R_ABORT_UNLESS(e) AMS_ABORT_UNLESS((e)==0)
#define R_SUCCEEDED(e) ((e)==0)
#define R_FAILED(e) ((e)!=0)
#define R_RETURN(e) {const ams::Result rc_return_=(e);if constexpr(std::same_as<decltype(__TmpCurrentResultReference),ams::Result&>){__TmpCurrentResultReference=rc_return_;}return rc_return_;}
#define R_SUCCEED() R_RETURN(0)
#define R_THROW(e) R_RETURN(e)
#define R_UNLESS(e,r) do{if(!(e)){R_RETURN(r);}}while(0)
#define R_TRY(e) do{const ams::Result rc_try_=(e);if(rc_try_){R_RETURN(rc_try_);}}while(0)
#define R_SUCCEED_IF(e) do{if(e){R_SUCCEED();}}while(0)
#define R_DISCARD(e) static_cast<void>((e))
#define R_TRY_CATCH(e) {const ams::Result caught_result_=(e);[[maybe_unused]]bool caught_handled_=false;if(caught_result_!=0)
#define R_CONVERT(t,v) if(t::Includes(caught_result_)){R_RETURN(v);}
#define R_CATCH(t) if(t::Includes(caught_result_)&&(caught_handled_=true))
#define R_END_TRY_CATCH if(caught_result_!=0&&!caught_handled_){R_RETURN(caught_result_);} }
#define CAT_INNER(a,b) a##b
#define CAT(a,b) CAT_INNER(a,b)
#define ON_SCOPE_EXIT auto CAT(scope_guard_,__LINE__)=ams::GuardMaker{}+[&]()
#define SCOPE_GUARD ams::GuardMaker{}+[&]()
#define DECLARE_RESULT_REFERENCE_I(n) [[maybe_unused]] constexpr bool CAT(has_prev_,n)=std::same_as<decltype(__TmpCurrentResultReference),ams::Result&>;[[maybe_unused]] auto& CAT(prev_,n)=__TmpCurrentResultReference;[[maybe_unused]] ams::Result CAT(result_,n)=0;ams::Result& __TmpCurrentResultReference=CAT(has_prev_,n)?CAT(prev_,n):CAT(result_,n)
#define DECLARE_RESULT_REFERENCE(n) DECLARE_RESULT_REFERENCE_I(n)
#define ON_RESULT_FAILURE DECLARE_RESULT_REFERENCE(__COUNTER__);auto CAT(result_guard_,__LINE__)=ams::ResultGuardMaker{__TmpCurrentResultReference}+[&]()
