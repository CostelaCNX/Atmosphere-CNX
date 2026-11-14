// SPDX-License-Identifier: GPL-2.0-or-later
#include "usb_system_power_core.hpp"
#include <stratosphere/usb/usb_sleep_core.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
using namespace ams::mitm::usb_storage::system_power;
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
struct Backend {
    bool closed{},storage_failed{},queued{},finished{};unsigned background_active{},storage_active{};
    unsigned drains{},prepares{},resumes{},acks{},opens{},failures{};
    std::uint32_t drain_result{},prepare_result{},reserve_result{},resume_result{},ack_result{},last_failure{};
    std::string order;
    void CloseBackground(){closed=true;order+='C';}
    std::uint32_t DrainBackground(){++drains;order+='D';CHECK(closed);return background_active?10:drain_result;}
    std::uint32_t PrepareStorage(){++prepares;order+='P';CHECK(closed&&!background_active);return storage_active?11:prepare_result;}
    std::uint32_t ReserveWake(){++resumes;order+='R';CHECK(closed&&!background_active&&!storage_active&&!storage_failed);return reserve_result;}
    void StartRecovery(){CHECK(!queued&&closed&&!ack_result);queued=true;finished=false;order+='Q';}
    bool TakeWakeResult(std::uint32_t &result){
        if(!finished)return false;
        CHECK(queued);queued=false;finished=false;result=resume_result;return true;
    }
    std::uint32_t BusyResult(){return 11;}
    bool IsBackgroundQuiescent(){return background_active==0;}
    bool IsStorageQuiescent(){return !storage_active&&(!queued||finished);}
    void OpenBackground(){++opens;closed=false;order+='O';}
    void FailStorage(std::uint32_t r){++failures;storage_failed=true;last_failure=r;order+='F';}
    std::uint32_t Acknowledge(std::uint32_t state){++acks;order+='A';
        if(state>=1&&state<=5)CHECK(closed&&!background_active&&!storage_active);
        return ack_result;}
    std::uint32_t InvalidStateResult(){return 12;}
};
static void NominalAndCancellation(){
    Coordinator c;Backend b;auto outcome=c.Handle(0,b);CHECK(outcome.ack_attempted&&!outcome.result&&b.order=="A");
    for(unsigned cycle=0;cycle<1000;++cycle){b.order.clear();outcome=c.Handle(2,b);
        CHECK(outcome.ack_attempted&&!outcome.result&&c.GetPhase()==Phase::Asleep&&b.order=="CDPA"&&b.closed);
        const auto prepares=b.prepares,resumes=b.resumes,opens=b.opens;
        for(unsigned state:{2u,3u,4u,1u,2u,3u,4u,1u}){b.order.clear();outcome=c.Handle(state,b);
            CHECK(outcome.ack_attempted&&!outcome.result&&c.GetPhase()==Phase::Asleep&&b.closed&&b.order=="A");}
        CHECK(b.prepares==prepares&&b.resumes==resumes&&b.opens==opens);
        b.order.clear();outcome=c.Handle(0,b);
        CHECK(outcome.ack_attempted&&!outcome.result&&c.GetPhase()==Phase::Resuming&&b.closed&&b.order=="RAQ");
        b.order.clear();outcome=c.Handle(0,b);
        CHECK(outcome.ack_attempted&&!outcome.result&&b.closed&&b.order=="A"&&b.resumes==resumes+1);
        b.finished=true;c.PollWake(b);CHECK(c.GetPhase()==Phase::Awake&&!b.closed&&b.order=="AO");
        b.order.clear();outcome=c.Handle(0,b);CHECK(outcome.ack_attempted&&!outcome.result&&b.order=="A");
    }
    Coordinator shutdown;Backend s;outcome=shutdown.Handle(5,s);CHECK(outcome.ack_attempted&&s.closed&&shutdown.GetPhase()==Phase::Asleep);
}
static void GameReadAcrossPartialWake() {
    namespace sleep = ams::usb::sleep;
    struct GameBackend : Backend {
        sleep::Gate admission;
        std::uint32_t PrepareStorage() {
            const auto result = Backend::PrepareStorage();
            if (!result) CHECK(admission.BeginSleep() && admission.Park());
            return result;
        }
        std::uint32_t ReserveWake() {
            CHECK(admission.BeginWake());
            return Backend::ReserveWake();
        }
    } b;
    Coordinator c;
    CHECK(b.admission.Enter() == sleep::Admission::Entered); // Game is running before sleep.
    CHECK(b.admission.Leave());
    // The first request can arrive after the game has already started. The
    // last eight nibbles are the sequence observed on the SL3 field unit.
    for (unsigned state : {1u, 2u, 3u, 4u, 1u, 2u, 3u, 4u, 1u}) {
        const auto outcome = c.Handle(state, b);
        CHECK(outcome.ack_attempted && !outcome.result && b.closed);
        CHECK(b.prepares == 1 && !b.queued && !b.resumes && !b.opens);
        CHECK(b.admission.Enter() == sleep::Admission::Wait);
    }
    b.order.clear();
    CHECK(c.Handle(0, b).ack_attempted && b.order == "RAQ" && b.resumes == 1);
    CHECK(b.admission.Enter() == sleep::Admission::Wait);
    CHECK(b.admission.CompleteWake()); // Storage reports verified media.
    b.finished = true;
    c.PollWake(b);
    CHECK(!b.closed && b.opens == 1 && b.admission.Enter() == sleep::Admission::Entered);
}
static void MinimumAwakeDrainsBeforeAck() {
    Coordinator c;
    Backend b;
    b.storage_active = 1;
    const auto begin = c.Handle(1, b);
    CHECK(!begin.ack_attempted && !b.acks && b.closed && c.GetPhase() == Phase::Preparing);
    b.storage_active = 0;
    CHECK(c.Handle(1, b).ack_attempted && c.GetPhase() == Phase::Asleep && b.acks == 1);
    // Cancellation before deep sleep still validates once on FullAwake.
    CHECK(c.Handle(0, b).ack_attempted && b.queued && b.resumes == 1);
    CHECK(!c.Handle(1, b).ack_attempted && b.acks == 2);
    b.finished = true;
    CHECK(c.Handle(1, b).ack_attempted && b.closed && c.GetPhase() == Phase::Asleep);
}
static void DrainAndFailures(){
    Coordinator c;Backend b;b.background_active=1;auto outcome=c.Handle(2,b);
    CHECK(!outcome.ack_attempted&&outcome.result==10&&b.closed&&!b.prepares&&!b.acks&&c.GetPhase()==Phase::Preparing);
    b.background_active=0;b.storage_active=2;outcome=c.Handle(2,b);
    CHECK(!outcome.ack_attempted&&outcome.result==11&&b.prepares==1&&!b.acks);
    b.storage_active=1;outcome=c.Handle(2,b);CHECK(!outcome.ack_attempted&&b.prepares==2&&!b.acks);
    b.storage_active=0;outcome=c.Handle(2,b);CHECK(outcome.ack_attempted&&!outcome.result&&b.prepares==3&&b.acks==1);
    outcome=c.Handle(0,b);CHECK(outcome.ack_attempted&&!outcome.result&&b.resumes==1&&b.closed);
    const auto prior_acks=b.acks;
    outcome=c.Handle(2,b);CHECK(!outcome.ack_attempted&&b.acks==prior_acks&&b.closed);
    b.finished=true;outcome=c.Handle(2,b);CHECK(outcome.ack_attempted&&b.closed&&c.GetPhase()==Phase::Asleep);
    Coordinator malformed;Backend m;m.background_active=1;CHECK(!malformed.Handle(2,m).ack_attempted);
    outcome=malformed.Handle(0,m);CHECK(!outcome.ack_attempted&&outcome.result==12&&malformed.GetPhase()==Phase::Failed&&m.closed&&m.storage_failed&&!m.resumes);
    m.background_active=0;outcome=malformed.Handle(0,m);CHECK(outcome.ack_attempted&&outcome.result==12&&malformed.GetPhase()==Phase::Failed&&!m.closed&&!m.resumes); // SD status only
    for(unsigned state:{1u,2u,3u,4u,5u}){
        Coordinator failed;Backend f;f.prepare_result=70;outcome=failed.Handle(state,f);
        CHECK(outcome.ack_attempted&&outcome.result==70&&f.storage_failed&&f.closed&&failed.GetPhase()==Phase::Failed&&failed.Failure()==70);
        const auto prepare_count=f.prepares;outcome=failed.Handle(1,f);CHECK(outcome.ack_attempted&&outcome.result==70&&f.closed&&!f.resumes&&f.prepares==prepare_count);
        outcome=failed.Handle(0,f);CHECK(outcome.ack_attempted&&outcome.result==70&&!f.closed&&!f.resumes);
        f.background_active=1;outcome=failed.Handle(2,f);CHECK(!outcome.ack_attempted&&f.closed);
        f.background_active=0;outcome=failed.Handle(2,f);CHECK(outcome.ack_attempted&&outcome.result==70&&f.closed&&!f.resumes);
    }
    {
        Coordinator failed;Backend f;CHECK(failed.Handle(2,f).ack_attempted);f.resume_result=71;f.order.clear();outcome=failed.Handle(0,f);
        CHECK(outcome.ack_attempted&&!outcome.result&&f.queued&&f.closed&&f.order=="RAQ");
        f.finished=true;failed.PollWake(f);
        CHECK(failed.GetPhase()==Phase::Failed&&f.storage_failed&&!f.closed&&f.order=="RAQCFO");
        CHECK(failed.Failure()==71&&f.resumes==1);outcome=failed.Handle(0,f);CHECK(outcome.ack_attempted&&outcome.result==71&&f.resumes==1);
    }
    for(unsigned fail_at=0;fail_at<3;++fail_at){Coordinator failed;Backend f;
        if(fail_at==1){CHECK(failed.Handle(2,f).ack_attempted);}
        if(fail_at==2){failed.Fail(73,f);}
        f.ack_result=72;const auto old_open=f.opens;outcome=failed.Handle(fail_at?0:2,f);
        CHECK(outcome.ack_attempted&&failed.GetPhase()==Phase::Failed&&f.closed&&f.storage_failed&&f.opens==old_open);
        CHECK(failed.Failure()==(fail_at==2?73u:72u));
        CHECK(!f.queued);
    }
    Coordinator refused;Backend rf;CHECK(refused.Handle(2,rf).ack_attempted);
    rf.reserve_result=78;outcome=refused.Handle(0,rf);
    CHECK(outcome.ack_attempted&&outcome.result==78&&!rf.queued&&!rf.closed&&rf.storage_failed);
    Coordinator interrupted;Backend it;CHECK(interrupted.Handle(2,it).ack_attempted);
    CHECK(interrupted.Handle(0,it).ack_attempted&&it.queued);it.ack_result=72;
    CHECK(!interrupted.Handle(1,it).ack_attempted&&it.closed&&!it.storage_failed);
    it.finished=true;CHECK(interrupted.Handle(1,it).ack_attempted&&it.closed&&it.storage_failed);
    CHECK(interrupted.GetPhase()==Phase::Failed);
    Coordinator failed;Backend f;failed.Fail(75,f);failed.Fail(76,f);CHECK(failed.Failure()==75&&f.closed&&f.storage_failed);
    f.storage_active=1;outcome=failed.Handle(0,f);CHECK(!outcome.ack_attempted&&!f.opens);
    f.storage_active=0;outcome=failed.Handle(0,f);CHECK(outcome.ack_attempted&&outcome.result==75&&!f.closed);
    Coordinator unknown;Backend u;outcome=unknown.Handle(255,u);CHECK(outcome.ack_attempted&&outcome.result==12&&u.storage_failed&&u.closed&&unknown.GetPhase()==Phase::Failed);
    Coordinator drain_fail;Backend d;d.drain_result=77;outcome=drain_fail.Handle(2,d);CHECK(!outcome.ack_attempted&&outcome.result==77&&d.closed&&!d.prepares&&!d.acks);
    d.drain_result=0;outcome=drain_fail.Handle(2,d);CHECK(outcome.ack_attempted&&!outcome.result&&d.prepares==1);
}
static void ActivityRace(){
    ActivityGate gate(true);CHECK(gate.Closed()&&!gate.Active()&&!gate.TryEnter());gate.Open();
    CHECK(gate.TryEnter()&&gate.TryEnter()&&gate.Active()==2);gate.Close();CHECK(gate.Closed()&&gate.Active()==2&&!gate.TryEnter());
    gate.Leave();gate.Leave();CHECK(!gate.Active()&&!gate.TryEnter());gate.Open();
    std::atomic<bool> done{},forbidden{},violation{};std::atomic<unsigned> admitted{};
    std::vector<std::thread> workers;for(unsigned i=0;i<4;++i)workers.emplace_back([&]{while(!done.load()){
        if(gate.TryEnter()){if(forbidden.load())violation=true;++admitted;std::this_thread::yield();gate.Leave();}else std::this_thread::yield();}});
    while(admitted.load()<100)std::this_thread::yield();
    for(unsigned cycle=0;cycle<1000;++cycle){gate.Close();while(gate.Active())std::this_thread::yield();forbidden=true;
        CHECK(gate.Closed()&&!gate.Active());for(unsigned attempt=0;attempt<5;++attempt)CHECK(!gate.TryEnter());
        std::this_thread::yield();CHECK(!gate.Active());forbidden=false;gate.Open();std::this_thread::yield();}
    done=true;for(auto&t:workers)t.join();CHECK(!violation.load()&&!gate.Active()&&admitted.load()>=100);
    gate.Close();CHECK(!gate.TryEnter());
}
int main(){NominalAndCancellation();GameReadAcrossPartialWake();MinimumAwakeDrainsBeforeAck();DrainAndFailures();ActivityRace();std::printf("PASS %u PSC lifecycle and concurrent background gate checks\n",checks);return 0;}
