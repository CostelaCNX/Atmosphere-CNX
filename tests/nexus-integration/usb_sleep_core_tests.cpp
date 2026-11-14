// SPDX-License-Identifier: GPL-2.0-or-later
#include <stratosphere/usb/usb_sleep_core.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>
#include <tuple>
#include <vector>
using namespace ams::usb::sleep;
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

static void DrainAndWake(){
    Gate gate;CHECK(gate.GetPhase()==Phase::Awake&&!gate.Paused()&&!gate.Active());CHECK(!gate.Leave());
    for(unsigned i=0;i<64;++i)CHECK(gate.Enter()==Admission::Entered);
    CHECK(gate.BeginSleep()&&gate.Paused()&&gate.Active()==64);CHECK(!gate.BeginSleep());
    for(unsigned i=64;i;--i){CHECK(gate.Enter()==Admission::Wait);CHECK(gate.Active()==i);CHECK(!gate.Park());CHECK(!gate.BeginWake());CHECK(gate.Leave());}
    CHECK(!gate.Leave());CHECK(gate.Park()&&gate.GetPhase()==Phase::Suspended);CHECK(!gate.Park());
    for(unsigned i=0;i<100;++i)CHECK(gate.Enter()==Admission::Wait);
    CHECK(!gate.CompleteWake());CHECK(gate.BeginWake());CHECK(gate.Enter()==Admission::Wait);CHECK(!gate.BeginWake());
    CHECK(gate.CompleteWake()&&!gate.Paused());CHECK(gate.Enter()==Admission::Entered);CHECK(gate.Leave());
    // Cancelled sleep still passes the same clean park/wake barrier, without a
    // physical power transition. Neither BeginWake nor CompleteWake skips drain.
    CHECK(gate.Enter()==Admission::Entered);CHECK(gate.BeginSleep());CHECK(!gate.BeginWake());CHECK(!gate.CompleteWake());
    CHECK(gate.Leave()&&gate.Park()&&gate.BeginWake()&&gate.CompleteWake());
    for(unsigned cycle=0;cycle<10000;++cycle){CHECK(gate.BeginSleep()&&gate.Park());CHECK(gate.Enter()==Admission::Wait);
        CHECK(gate.BeginWake()&&gate.CompleteWake());CHECK(gate.Enter()==Admission::Entered&&gate.Leave());}
}
static void StopAndFailure(){
    for(unsigned phase=0;phase<4;++phase){Gate gate;CHECK(gate.Enter()==Admission::Entered);
        if(phase){CHECK(gate.BeginSleep());if(phase>1){CHECK(gate.Leave()&&gate.Park());if(phase>2)CHECK(gate.BeginWake());}}
        const auto active=gate.Active();gate.Stop();CHECK(gate.GetPhase()==Phase::Stopped);CHECK(gate.Enter()==Admission::Rejected);
        gate.Fail();CHECK(gate.GetPhase()==Phase::Stopped);
        if(active){CHECK(!gate.Restart());CHECK(gate.Leave());}
        CHECK(gate.Restart()&&gate.Enter()==Admission::Entered&&gate.Leave());CHECK(!gate.Restart());
    }
    Gate active;CHECK(active.Enter()==Admission::Entered&&active.BeginSleep());active.Fail();
    CHECK(active.GetPhase()==Phase::Failed&&active.Enter()==Admission::Rejected);CHECK(!active.Restart());
    CHECK(active.Leave());CHECK(!active.Restart());active.Stop();CHECK(active.Restart());CHECK(active.Enter()==Admission::Entered&&active.Leave());
    Gate sleeping;CHECK(sleeping.BeginSleep()&&sleeping.Park()&&sleeping.BeginWake());sleeping.Fail();
    CHECK(!sleeping.CompleteWake()&&sleeping.Enter()==Admission::Rejected);CHECK(!sleeping.Restart());sleeping.Stop();CHECK(sleeping.Restart());
}
static void ReachableStateInvariants(){
    std::vector<Gate> pending(1);std::set<std::tuple<unsigned,unsigned>> seen;
    while(!pending.empty()){
        const auto g=pending.back();pending.pop_back();const auto key=std::make_tuple(unsigned(g.GetPhase()),g.Active());if(!seen.insert(key).second)continue;
        for(unsigned op=0;op<9;++op){auto next=g;const auto before=g.Active();const auto phase=g.GetPhase();
            switch(op){
                case 0:if(before==4)continue;{const auto admitted=next.Enter();if(phase!=Phase::Awake)CHECK(admitted!=Admission::Entered);
                    CHECK(next.Active()==before+(admitted==Admission::Entered?1u:0u));}break;
                case 1:{const bool left=next.Leave();CHECK(left==(before!=0));CHECK(next.Active()==before-(left?1u:0u));}break;
                case 2:next.BeginSleep();break;case 3:next.Park();break;case 4:next.BeginWake();break;
                case 5:next.CompleteWake();break;case 6:next.Fail();break;case 7:next.Stop();break;
                case 8:next.Restart();break;
            }
            if(op>1)CHECK(next.Active()==before);
            if(next.GetPhase()==Phase::Suspended||next.GetPhase()==Phase::Resuming)CHECK(!next.Active());
            if(phase==Phase::Stopped&&op!=8)CHECK(next.GetPhase()==Phase::Stopped);
            if(phase==Phase::Failed&&op!=7)CHECK(next.GetPhase()==Phase::Failed);
            if(before&&phase!=Phase::Awake)CHECK(next.GetPhase()!=Phase::Awake);
            pending.push_back(next);
        }
    }
    CHECK(seen.size()>=18);
}
static std::array<std::uint8_t,18> Sense(bool descriptor,unsigned key,unsigned asc,unsigned ascq){
    std::array<std::uint8_t,18> p{};p[0]=descriptor?0x72:0x70;p[7]=descriptor?0:10;
    if(descriptor){p[1]=key;p[2]=asc;p[3]=ascq;}else{p[2]=key;p[12]=asc;p[13]=ascq;}return p;
}
static void NarrowSensePolicy(){
    for(bool descriptor:{false,true})for(unsigned key=0;key<16;++key)for(unsigned asc=0;asc<256;++asc)for(unsigned ascq=0;ascq<8;++ascq){
        const auto p=Sense(descriptor,key,asc,ascq);const auto size=descriptor?8u:18u;const bool acceptable=key==6&&asc==0x29&&ascq<=4;
        CHECK(IsPowerResetAttention(p.data(),size)==acceptable);ReadRetry ordinary{};CHECK(!ordinary.RetryReset(p.data(),size)&&!ordinary.reset_consumed);
        ReadRetry clean{true};CHECK(clean.RetryReset(p.data(),size)==acceptable);CHECK(clean.reset_consumed==acceptable);
        if(acceptable)CHECK(!clean.RetryReset(p.data(),size));
    }
    for(bool descriptor:{false,true}){
        auto p=Sense(descriptor,6,0x29,0);const auto complete=descriptor?8u:18u;
        for(unsigned n=0;n<complete;++n)CHECK(!IsPowerResetAttention(p.data(),n));
        p[0]++;CHECK(!IsPowerResetAttention(p.data(),complete)); // deferred sense
        p=Sense(descriptor,6,0x29,0);p[7]=255;CHECK(!IsPowerResetAttention(p.data(),18));
        p=Sense(descriptor,6,0x29,0);p[0]=0;CHECK(!IsPowerResetAttention(p.data(),complete));
    }
    CHECK(!IsPowerResetAttention(nullptr,UINT32_MAX));
    ReadRetry ready{true};for(unsigned i=0;i<20;++i)CHECK(ready.RetryReady());
    for(unsigned i=0;i<20;++i){CHECK(!ready.RetryReady());}
    CHECK(ready.ready_retries==20);
    const auto p=Sense(false,6,0x29,2);CHECK(ready.RetryReset(p.data(),p.size()));CHECK(!ready.RetryReset(p.data(),p.size()));
}
int main(){
    DrainAndWake();StopAndFailure();ReachableStateInvariants();NarrowSensePolicy();
    CHECK(DrainBudgetNs==5000000000ULL&&ProbeBudgetNs==5000000000ULL);
    CHECK(Deadline(0,0)==0&&Deadline(1,2)==3&&Deadline(UINT64_MAX,1)==UINT64_MAX);
    CHECK(Deadline(UINT64_MAX-10,10)==UINT64_MAX&&Deadline(UINT64_MAX-9,10)==UINT64_MAX);
    CHECK(Deadline(1,UINT64_MAX)==UINT64_MAX);
    std::printf("PASS %u sleep admission and clean-resume policy checks\n",checks);return 0;
}
