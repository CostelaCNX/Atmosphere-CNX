// SPDX-License-Identifier: GPL-2.0-or-later
#include <stratosphere/usb/usb_eject.hpp>
#include <cstdio>
#include <cstdlib>
using namespace ams::usb::eject;
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

// Issue #9: a failed eject must not block the drive until reboot.
static void FailedEjectRecovers(){
    Gate g;g.NewGeneration(7);
    CHECK(g.Request(1,7)&&g.Blocked());
    g.At(Stage::DeviceCache);g.Fail(0x1234);
    CHECK(g.Blocked()&&g.Get().phase==Phase::Failed&&!g.Acquire(7));
    CHECK(!g.Resume());               // Resume is only for a completed eject.
    CHECK(g.RecoverFromFailure());
    CHECK(!g.Blocked()&&g.Get().phase==Phase::Online&&g.Get().stage==Stage::None);
    CHECK(g.Get().result==0x1234&&g.Get().request==0); // Failure stays visible.
    CHECK(!g.RecoverFromFailure());
    g.NewGeneration(8);CHECK(g.Acquire(8));g.Release(8);
    CHECK(g.Request(2,8));            // A new eject can be attempted.
}
static void OnlyFailedRecovers(){
    Gate online;online.NewGeneration(1);CHECK(!online.RecoverFromFailure()&&!online.Blocked());
    Gate pending;pending.NewGeneration(1);CHECK(pending.Request(1,1)&&!pending.RecoverFromFailure()&&pending.Blocked());
    Gate safe;safe.NewGeneration(1);CHECK(safe.Request(1,1));safe.Complete();
    CHECK(!safe.RecoverFromFailure()&&safe.Get().phase==Phase::Safe&&safe.Resume()&&!safe.Blocked());
    Gate busy;busy.NewGeneration(3);CHECK(busy.Acquire(3)&&!busy.Request(1,3));busy.Release(3);CHECK(busy.Request(1,3));
}
int main(){FailedEjectRecovers();OnlyFailedRecovers();std::printf("PASS %u eject gate checks\n",checks);return 0;}
