#pragma once

#include "defs.h"

#include <library/cpp/actors/core/actor.h>
#include <library/cpp/actors/core/actorsystem.h>
#include <library/cpp/actors/core/interconnect.h>
#include <library/cpp/actors/core/mailbox.h>
#include <library/cpp/actors/core/scheduler_queue.h>
#include <library/cpp/actors/interconnect/interconnect_common.h>
#include <ydb/core/base/appdata.h>
#include <ydb/core/base/tablet.h>
#include <ydb/core/base/tablet_pipe.h>
#include <util/system/env.h>

#include "single_thread_ic_mock.h"

namespace NKikimr {

class TTestActorSystem {
    class TTestSchedulerThread : public ISchedulerThread {
        TTestActorSystem *Context;
        volatile ui64 *CurrentTimestampPtr = nullptr;
        volatile ui64 *CurrentMonotonicPtr = nullptr;
        std::vector<NSchedulerQueue::TReader*> Readers;
        const ui32 NodeId;

    public:
        TTestSchedulerThread(TTestActorSystem *context, ui32 nodeId)
            : Context(context)
            , NodeId(nodeId)
        {}

        void Prepare(TActorSystem* /*actorSystem*/, volatile ui64 *currentTimestamp, volatile ui64 *currentMonotonic) override {
            CurrentTimestampPtr = currentTimestamp;
            CurrentMonotonicPtr = currentMonotonic;
        }

        void PrepareSchedules(NSchedulerQueue::TReader **readers, ui32 scheduleReadersCount) override { 
            Readers = {readers, readers + scheduleReadersCount};
        }

        void AdjustClock(TInstant clock) {
            *CurrentTimestampPtr = clock.MicroSeconds();
            *CurrentMonotonicPtr = clock.MicroSeconds();
        }

        bool TransferSchedule() {
            bool res = false;
            for (NSchedulerQueue::TReader *reader : Readers) {
                while (NSchedulerQueue::TEntry *e = reader->Pop()) {
                    Context->Schedule(TInstant::MicroSeconds(e->InstantMicroseconds), e->Ev, e->Cookie, NodeId);
                    res = true;
                }
            }
            return res;
        }

        void Start() override {}
        void PrepareStop() override {}
        void Stop() override {}
    };

    struct TScheduleItem {
        std::unique_ptr<IEventHandle> Event;
        ISchedulerCookie *Cookie;
        ui32 NodeId;

        TScheduleItem(TAutoPtr<IEventHandle> ev, ISchedulerCookie *cookie, ui32 nodeId)
            : Event(ev.Release())
            , Cookie(cookie)
            , NodeId(nodeId)
        {}
    };

    struct TMailboxId : std::tuple<ui32, ui32, ui32> { // NodeId, PoolId, Hint
        TMailboxId(const TActorId& actorId)
            : TMailboxId(actorId.NodeId(), actorId.PoolID(), actorId.Hint())
        {}

        TMailboxId(ui32 nodeId, ui32 poolId, ui32 hint)
            : std::tuple<ui32, ui32, ui32>(nodeId, poolId, hint)
        {}
    };

    struct TPerNodeInfo {
        std::unique_ptr<TActorSystem> ActorSystem;
        std::unique_ptr<TMailboxTable> MailboxTable;
        std::unique_ptr<TExecutorThread> ExecutorThread;
        std::unordered_map<ui32, TActorId> InterconnectProxy;
        TTestSchedulerThread *SchedulerThread;
        ui32 NextHint = 1;
    };

    struct TMailboxInfo {
        TMailboxHeader Header{TMailboxType::Simple};
        ui64 ActorLocalId = 1;
    };

    const ui32 MaxNodeId;
    std::map<TInstant, std::deque<TScheduleItem>> ScheduleQ;
    TInstant Clock = TInstant::Zero();
    std::unordered_map<TMailboxId, TMailboxInfo, THash<std::tuple<ui32, ui32, ui32>>> Mailboxes;
    TProgramShouldContinue ProgramShouldContinue;
    TAppData AppData;
    TIntrusivePtr<NLog::TSettings> LoggerSettings_;
    TActorId CurrentRecipient;
    ui32 CurrentNodeId = 0;
    ui64 EventsProcessed = 0;
    std::unordered_map<ui32, TPerNodeInfo> PerNodeInfo;
    TSingleThreadInterconnectMock InterconnectMock;
    std::set<TActorId> LoggerActorIds;

    static thread_local TTestActorSystem *CurrentTestActorSystem;

    struct TEventProcessingStat {
        ui64 HitCount;
        TDuration TotalTime;
    };
    std::unordered_map<std::pair<TString, ui32>, TEventProcessingStat> EventProcessingStats;
    std::unordered_map<ui32, TString> EventName;

    struct TActorStats {
        ui64 Created = 0;
        ui64 Destroyed = 0;

        friend bool operator <(const TActorStats& x, const TActorStats& y) {
            return x.Created - x.Destroyed > y.Created - y.Destroyed;
        }
    };
    std::unordered_map<TString, TActorStats> ActorStats;
    std::unordered_map<IActor*, TString> ActorName;

    class TEdgeActor : public TActor<TEdgeActor> {
        std::unique_ptr<IEventHandle> *HandlePtr = nullptr;
        TString Tag;

    public:
        TEdgeActor(const char *file, int line)
            : TActor(&TThis::StateFunc)
            , Tag(TStringBuilder() << file << ":" << line)
        {}

        void WaitForEvent(std::unique_ptr<IEventHandle> *handlePtr) {
            Y_VERIFY(!HandlePtr);
            HandlePtr = handlePtr;
        }

        void StopWaitingForEvent() {
            Y_VERIFY(HandlePtr);
            HandlePtr = nullptr;
        }

        void StateFunc(TAutoPtr<IEventHandle>& ev, const TActorContext& /*ctx*/) {
            Y_VERIFY(HandlePtr, "event is not being captured by this actor Tag# %s", Tag.data());
            Y_VERIFY(!*HandlePtr);
            HandlePtr->reset(ev.Release());
        }
    };

public:
    std::function<bool(ui32, IEventHandle&)> FilterFunction;
    IOutputStream *LogStream = &Cerr;

public:
    TTestActorSystem(ui32 numNodes, NLog::EPriority defaultPrio = NLog::PRI_ERROR)
        : MaxNodeId(numNodes)
        , AppData(0, 0, 0, 0, {{"IC", 0}}, nullptr, nullptr, nullptr, &ProgramShouldContinue)
        , LoggerSettings_(MakeIntrusive<NLog::TSettings>(TActorId(0, "logger"), NKikimrServices::LOGGER, defaultPrio))
        , InterconnectMock(0, Max<ui64>(), this) // burst capacity (bytes), bytes per second
    {
        AppData.Counters = MakeIntrusive<NMonitoring::TDynamicCounters>();
        AppData.DomainsInfo = MakeIntrusive<TDomainsInfo>();
        LoggerSettings_->Append(
            NActorsServices::EServiceCommon_MIN,
            NActorsServices::EServiceCommon_MAX,
            NActorsServices::EServiceCommon_Name
        );
        LoggerSettings_->Append(
            NKikimrServices::EServiceKikimr_MIN,
            NKikimrServices::EServiceKikimr_MAX,
            NKikimrServices::EServiceKikimr_Name
        );
        for (ui32 i = 0; i < numNodes; ++i) {
            PerNodeInfo.emplace(i + 1, TPerNodeInfo());
        }

        Y_VERIFY(!CurrentTestActorSystem);
        CurrentTestActorSystem = this;
    }

    ~TTestActorSystem() {
        Y_VERIFY(CurrentTestActorSystem == this);
        CurrentTestActorSystem = nullptr;
    }

    static TIntrusivePtr<ITimeProvider> CreateTimeProvider();

    TAppData *GetAppData() {
        return &AppData;
    }

    void Start() {
        for (auto& [nodeId, info] : PerNodeInfo) {
            SetupNode(nodeId, info);
        }
        LoggerActorIds.insert(LoggerSettings_->LoggerActorId);
        for (auto& [nodeId, info] : PerNodeInfo) {
            StartNode(nodeId);
        }
    }

    void SetupNode(ui32 nodeId, TPerNodeInfo& info) {
        auto setup = MakeHolder<TActorSystemSetup>();
        setup->NodeId = nodeId;
        setup->ExecutorsCount = 1;
        info.SchedulerThread = new TTestSchedulerThread(this, nodeId);
        setup->Scheduler.Reset(info.SchedulerThread);
        setup->Executors.Reset(new TAutoPtr<IExecutorPool>[setup->ExecutorsCount]);
        IExecutorPool *pool = CreateTestExecutorPool(nodeId);
        setup->Executors[0].Reset(pool);

        // we create this actor for correct service lookup through ActorSystem
        setup->LocalServices.emplace_back(LoggerSettings_->LoggerActorId, TActorSetupCmd(
            new TEdgeActor(__FILE__, __LINE__), TMailboxType::Simple, 0));

        auto common = MakeIntrusive<TInterconnectProxyCommon>();
        auto& proxyActors = setup->Interconnect.ProxyActors;
        proxyActors.resize(MaxNodeId + 1);
        for (const auto& [peerNodeId, peerInfo] : PerNodeInfo) {
            if (peerNodeId != nodeId) {
                proxyActors[peerNodeId] = TActorSetupCmd(InterconnectMock.CreateProxyActor(nodeId, peerNodeId,
                    common).release(), TMailboxType::Simple, 0);
            }
        }

        info.ActorSystem = std::make_unique<TActorSystem>(setup, &AppData, LoggerSettings_);
        info.MailboxTable = std::make_unique<TMailboxTable>();
        info.ExecutorThread = std::make_unique<TExecutorThread>(0, 0, info.ActorSystem.get(), pool,
            info.MailboxTable.get(), "TestExecutor");
    }

    void StartNode(ui32 nodeId) {
        TPerNodeInfo& info = PerNodeInfo.at(nodeId);
        CurrentNodeId = nodeId;
        info.ActorSystem->Start();
        LoggerActorIds.insert(info.ActorSystem->LookupLocalService(LoggerSettings_->LoggerActorId));
        CurrentNodeId = 0;
    }

    void StopNode(ui32 nodeId) {
        TPerNodeInfo& info = PerNodeInfo.at(nodeId);
        info.ActorSystem->Stop();

        for (;;) {
            // delete all mailboxes from this node (expecting that new one can be created during deletion)
            const TMailboxId from(nodeId, 0, 0);
            const TMailboxId to(nodeId + 1, 0, 0);
            bool found = false;
            for (auto it = Mailboxes.begin(); it != Mailboxes.end(); ) {
                if (from <= it->first && it->first < to) {
                    TMailboxInfo& mbox = it->second;
                    mbox.Header.ForEach([&](ui64 /*actorId*/, IActor *actor) { ActorName.erase(actor); });
                    mbox.Header.CleanupActors();
                    it = Mailboxes.erase(it);
                    found = true;
                } else {
                    ++it;
                }
            }
            if (found) {
                continue;
            }

            std::deque<TScheduleItem> deleteQueue;
            auto it = ScheduleQ.begin();
            while (it != ScheduleQ.end()) {
                auto& queue = it->second;
                bool found = false;
                for (auto& item : queue) {
                    if (item.NodeId == nodeId) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    std::deque<TScheduleItem> newQueue;
                    for (auto& item : queue) {
                        (item.NodeId == nodeId ? deleteQueue : newQueue).push_back(std::move(item));
                    }
                    queue.swap(newQueue);
                    it = queue.empty() ? ScheduleQ.erase(it) : std::next(it);
                } else {
                    ++it;
                }
            }
            if (deleteQueue.empty()) {
                break;
            }
        }

        PerNodeInfo.erase(nodeId);
        SetupNode(nodeId, PerNodeInfo[nodeId]);
    }

    void Stop() {
        ProgramShouldContinue.ShouldStop();
        for (auto& [nodeId, info] : PerNodeInfo) {
            info.ActorSystem->Stop();
        }
        for (;;) {
            // exchange container to prevent side-effects while destroying actors (they may use actor system in dtors);
            // do this in cycle because actor destructor code may spawn more actors
            auto temp = std::exchange(Mailboxes, {});
            ActorName.clear();
            if (temp.empty()) {
                break;
            }
            temp.clear();

            auto temp1 = std::exchange(ScheduleQ, {});
            temp1.clear();
        }
        // dump event processing stats
        int dump;
        if (TryFromString(GetEnv("TESTACTORSYS_DUMP_TIMESTATS", "0"), dump) && dump) {
            std::vector<std::pair<TDuration, TString>> items;
            for (auto& [key, stats] : std::exchange(EventProcessingStats, {})) {
                const auto& [actorName, type] = key;
                items.emplace_back(stats.TotalTime, TStringBuilder() << actorName << "." << GetEventName(type)
                    << " " << stats.TotalTime << " " << stats.HitCount);
            }
            std::sort(items.begin(), items.end());
            for (const auto& [key, value] : items) {
                Cerr << value << Endl;
            }
        }
    }

    TString GetEventName(ui32 type) const {
        const auto it = EventName.find(type);
        return it != EventName.end() ? it->second : Sprintf("0x%08" PRIx32, type);
    }

    void SetLogPriority(NActors::NLog::EComponent component, NActors::NLog::EPriority priority) {
        TString explanation;
        int res = LoggerSettings_->SetLevel(priority, component, explanation);
        Y_VERIFY(!res, "failed to set log level: %s", explanation.data());
    }

    bool Send(IEventHandle *ev, ui32 nodeId = 0) {
        TAutoPtr<IEventHandle> wrapper(ev);
        return Send(wrapper, nodeId);
    }

    bool Send(TAutoPtr<IEventHandle>& ev, ui32 nodeId) {
        if (!ev) {
            return false;
        } else if (LoggerActorIds.count(ev->GetRecipientRewrite()) && ev->GetTypeRewrite() == NLog::TEvLog::EventType) {
            auto *msg = ev->CastAsLocal<NLog::TEvLog>();
            ui64 microsec = Clock.MicroSeconds();
            const unsigned frac = microsec % 1000000;
            microsec /= 1000000;
            const unsigned seconds = microsec % 60;
            microsec /= 60;
            const unsigned minutes = microsec % 60;
            microsec /= 60;
            const unsigned hours = microsec;
            TString clock = Sprintf("%02uh%02um%02u.%06us", hours, minutes, seconds, frac);
            *LogStream << (TStringBuilder() << msg->Stamp << " " << nodeId << " " << clock << " :"
                << LoggerSettings_->ComponentName(msg->Component) << " " << NLog::PriorityToString(msg->Level.ToPrio())
                << ": " << msg->Line << Endl);
            return true;
        }

        nodeId = nodeId ? nodeId : CurrentNodeId;
        Y_VERIFY(nodeId);

        // check if the target actor exists; we have to transform the event recipient early to keep behaviour of real
        // actor system here
        if (GetActor(TransformEvent(ev.Get(), nodeId))) {
            Schedule(Clock, ev, nullptr, nodeId);
            return true;
        } else {
            TAutoPtr<IEventHandle> wrapper(ev->ForwardOnNondelivery(TEvents::TEvUndelivered::ReasonActorUnknown));
            Send(wrapper, nodeId);
            return false;
        }
    }

    IActor *GetActor(const TActorId& actorId, TMailboxHeader **header = nullptr) {
        if (const auto it = Mailboxes.find(actorId); it != Mailboxes.end()) {
            TMailboxInfo& mbox = it->second;
            if (header) {
                *header = &mbox.Header;
            }
            return mbox.Header.FindActor(actorId.LocalId());
        } else {
            return nullptr;
        }
    }

    void Schedule(TInstant ts, TAutoPtr<IEventHandle> ev, ISchedulerCookie *cookie, ui32 nodeId) {
        Y_VERIFY(ts >= Clock);
        nodeId = nodeId ? nodeId : CurrentNodeId;
        Y_VERIFY(nodeId);
        if (ev && ev->HasEvent() && ev->GetTypeRewrite() == ev->Type && !EventName.count(ev->Type)) {
            EventName.emplace(ev->Type, TypeName(*ev->GetBase()));
        }
        ScheduleQ[ts].emplace_back(ev, cookie, nodeId);
    }

    void Schedule(TDuration timeout, TAutoPtr<IEventHandle> ev, ISchedulerCookie *cookie, ui32 nodeId) {
        Schedule(Clock + timeout, ev, cookie, nodeId);
    }

    TActorId Register(IActor *actor, const TActorId& parentId = TActorId(), ui32 poolId = 0, std::optional<ui32> hint = std::nullopt,
            ui32 nodeId = 0) {
        // count stats
        TString name = TypeName(*actor);
        ++ActorStats[name].Created;
        const bool inserted = ActorName.emplace(actor, std::move(name)).second;
        Y_VERIFY(inserted);

        // specify node id if not provided
        nodeId = nodeId ? nodeId : CurrentNodeId;
        TPerNodeInfo *info = GetNode(nodeId);

        // allocate mailbox id if needed
        const ui32 mboxId = hint.value_or(info->NextHint);
        if (mboxId == info->NextHint) {
            ++info->NextHint;
        }

        // register actor in mailbox
        const auto& it = Mailboxes.try_emplace(TMailboxId(nodeId, poolId, mboxId)).first;
        TMailboxInfo& mbox = it->second;
        mbox.Header.AttachActor(mbox.ActorLocalId, actor);

        // generate actor id
        const TActorId actorId(nodeId, poolId, mbox.ActorLocalId, mboxId);
        ++mbox.ActorLocalId;

        // initialize actor in actor system
        DoActorInit(info->ActorSystem.get(), actor, actorId, parentId ? parentId : CurrentRecipient);

        return actorId;
    }

    TActorId Register(IActor *actor, ui32 nodeId, ui32 poolId = 0) {
        return Register(actor, {}, poolId, {}, nodeId);
    }

    void RegisterService(const TActorId& serviceId, const TActorId& actorId) {
        const ui32 nodeId = actorId.NodeId(); // only at the node with the actor
        GetNode(nodeId)->ActorSystem->RegisterLocalService(serviceId, actorId);
    }

    template<typename TCallback>
    void Sim(TCallback&& callback, std::function<void(IEventHandle&)> witness = {}) {
        bool progress = true;

        while (callback()) {
            Y_VERIFY(progress, "test actor system stalled -- no progress made"); // ensure we are doing progress

            // obtain event with least time
            std::optional<TScheduleItem> item;
            while (!ScheduleQ.empty()) {
                const auto it = ScheduleQ.begin();
                auto& [timestamp, items] = *it;
                Y_VERIFY(timestamp >= Clock || items.empty());
                if (items.empty()) {
                    ScheduleQ.erase(it);
                } else {
                    Clock = timestamp;
                    item.emplace(std::move(items.front()));
                    items.pop_front();
                    break;
                }
            }

            progress = item.has_value();
            if (!item || (item->Cookie && !item->Cookie->Detach())) {
                continue;
            }

            std::unique_ptr<IEventHandle>& event = item->Event;
            if (witness) {
                witness(*event);
            }
            if (FilterFunction && !FilterFunction(item->NodeId, *event)) {
                continue;
            }
            WrapInActorContext(TransformEvent(event.get(), item->NodeId), [&](IActor *actor) {
                TAutoPtr<IEventHandle> ev(event.release());

                const ui32 type = ev->GetTypeRewrite();

                THPTimer timer;
                actor->Receive(ev, TActivationContext::AsActorContext());
                const TDuration timing = TDuration::Seconds(timer.Passed());

                const auto it = ActorName.find(actor);
                Y_VERIFY(it != ActorName.end(), "%p", actor);

                auto& stats = EventProcessingStats[std::make_pair(it->second, type)];
                ++stats.HitCount;
                stats.TotalTime += timing;

                ++EventsProcessed;
            });
        }
    }

    template<typename TCallback>
    void WrapInActorContext(TActorId actorId, TCallback&& callback) {
        const auto mboxIt = Mailboxes.find(actorId);
        if (mboxIt == Mailboxes.end()) {
            return;
        }
        TMailboxInfo& mbox = mboxIt->second;
        if (IActor *actor = mbox.Header.FindActor(actorId.LocalId())) {
            // obtain node info for this actor
            TPerNodeInfo *info = GetNode(actorId.NodeId());

            // adjust clock for correct operation
            info->SchedulerThread->AdjustClock(Clock);

            // allocate context and store its reference in TLS
            TActorContext ctx(mbox.Header, *info->ExecutorThread, GetCycleCountFast(), actorId);
            TlsActivationContext = &ctx;
            CurrentRecipient = actorId;
            CurrentNodeId = actorId.NodeId();

            // invoke the callback
            if constexpr (std::is_invocable_v<TCallback, IActor*>) {
                std::invoke(std::forward<TCallback>(callback), actor);
            } else {
                std::invoke(std::forward<TCallback>(callback));
            }

            // forget about the context
            TlsActivationContext = nullptr;
            CurrentRecipient = {};
            CurrentNodeId = 0;

            // read possibly updated schedule
            info->SchedulerThread->TransferSchedule();

            // remove destroyed actors from the mailbox
            for (const auto& actor : info->ExecutorThread->GetUnregistered()) {
                const TActorId& actorId = actor->SelfId();
                Y_VERIFY(TMailboxId(actorId) == mboxIt->first);
                const auto nameIt = ActorName.find(actor.Get());
                Y_VERIFY(nameIt != ActorName.end());
                ++ActorStats[nameIt->second].Destroyed;
                ActorName.erase(nameIt);
            }

            // terminate dead actors
            info->ExecutorThread->DropUnregistered();

            // drop the mailbox if no actors remain there
            if (mbox.Header.IsEmpty()) {
                Mailboxes.erase(mboxIt);
            }
        }
    }

    TActorId AllocateEdgeActor(ui32 nodeId, const char *file = "", int line = 0) {
        return Register(new TEdgeActor(file, line), TActorId(), 0, std::nullopt, nodeId);
    }

    std::unique_ptr<IEventHandle> WaitForEdgeActorEvent(const std::set<TActorId>& edgeActorIds) {
        std::unique_ptr<IEventHandle> res;
        std::vector<TEdgeActor*> edges;
        for (const TActorId& edgeActorId : edgeActorIds) {
            TEdgeActor *edge = dynamic_cast<TEdgeActor*>(GetActor(edgeActorId));
            Y_VERIFY(edge);
            edge->WaitForEvent(&res);
            edges.push_back(edge);
        }
        Sim([&] { return !res; });
        for (TEdgeActor *edge : edges) {
            edge->StopWaitingForEvent();
        }
        return res;
    }

    void DestroyActor(TActorId actorId) {
        // find per-node info for this actor
        TPerNodeInfo *info = GetNode(actorId.NodeId());
        Y_VERIFY(info);

        // find mailbox
        auto it = Mailboxes.find(actorId);
        Y_VERIFY(it != Mailboxes.end());
        TMailboxInfo& mbox = it->second;

        // update stats
        const auto nameIt = ActorName.find(mbox.Header.FindActor(actorId.LocalId()));
        Y_VERIFY(nameIt != ActorName.end());
        ++ActorStats[nameIt->second].Destroyed;
        ActorName.erase(nameIt);

        // unregister actor through the executor
        info->ExecutorThread->UnregisterActor(&mbox.Header, actorId.LocalId());

        // terminate unregistered actor
        info->ExecutorThread->DropUnregistered();

        // delete mailbox if empty
        if (mbox.Header.IsEmpty()) {
            Mailboxes.erase(actorId);
        }
    }

    std::set<ui32> GetNodes() const {
        std::set<ui32> res;
        for (const auto& [nodeId, info] : PerNodeInfo) {
            res.insert(nodeId);
        }
        return res;
    }

    ui32 GetNodeCount() const {
        return PerNodeInfo.size();
    }

    NLog::TSettings *LoggerSettings() const {
        return LoggerSettings_.Get();
    }

    void DumpActorCount(IOutputStream& s, const TString& prefix, const TString& suffix) {
        std::vector<std::pair<TString, TActorStats>> v(ActorStats.begin(), ActorStats.end());
        auto comp = [](const auto& x, const auto& y) {
            return x.second < y.second || (!(y.second < x.second) && x.first < y.first);
        };
        std::sort(v.begin(), v.end(), comp);
        size_t maxLen = 0;
        for (const auto& [name, stats] : v) {
            maxLen = Max(maxLen, name.length());
        }
        for (const auto& [name, stats] : v) {
            s << prefix << name;
            for (size_t i = name.length(); i < maxLen; ++i) {
                s << ' ';
            }
            s << " Created# " << stats.Created << " Destroyed# " << stats.Destroyed << " Alive# "
                << stats.Created - stats.Destroyed << suffix;
        }
    }

    TInstant GetClock() const { return Clock; }
    ui64 GetEventsProcessed() const { return EventsProcessed; }

    ////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    // tablet-related utility functions

    void SetupTabletRuntime(bool isMirror3dc = false, ui32 stateStorageNodeId = 0, ui32 targetNodeId = 0);
    static NTabletPipe::TClientConfig GetPipeConfigWithRetries();
    void SendToPipe(ui64 tabletId, const TActorId& sender, IEventBase* payload, ui64 cookie, const NKikimr::NTabletPipe::TClientConfig& pipeConfig);
    static TTabletStorageInfo *CreateTestTabletInfo(ui64 tabletId, TTabletTypes::EType tabletType, TBlobStorageGroupType::EErasureSpecies erasure, ui32 groupId);
    TActorId CreateTestBootstrapper(TTabletStorageInfo *info, std::function<IActor*(TActorId, TTabletStorageInfo*)> op, ui32 nodeId);

private:
    void SetupStateStorage(ui32 nodeId, ui32 stateStorageNodeId);
    void SetupTabletResolver(ui32 nodeId);

    IExecutorPool *CreateTestExecutorPool(ui32 nodeId);

    TActorId TransformEvent(IEventHandle *ev, ui32 nodeId) {
        Y_VERIFY(nodeId);
        TActorId recip = ev->GetRecipientRewrite();
        if (recip.NodeId() && recip.NodeId() != nodeId) {
            Y_VERIFY(!ev->HasEvent() || ev->GetBase()->IsSerializable(), "event can't pass through interconnect");
            Y_VERIFY(ev->Recipient == recip, "original recipient actor id lost");
            recip = GetNode(nodeId)->ActorSystem->InterconnectProxy(recip.NodeId());
            ev->Rewrite(TEvInterconnect::EvForward, recip);
        } else if (recip.IsService()) {
            Y_VERIFY(!recip.NodeId() || recip.NodeId() == nodeId, "recipient node mismatch");
            recip = GetNode(nodeId)->ActorSystem->LookupLocalService(recip);
            ev->Rewrite(ev->GetTypeRewrite(), recip);
        }
        Y_VERIFY(!recip || (recip.NodeId() == nodeId && !recip.IsService()));
        return recip;
    }

    TPerNodeInfo *GetNode(ui32 nodeId) {
        const auto nodeIt = PerNodeInfo.find(nodeId);
        Y_VERIFY(nodeIt != PerNodeInfo.end());
        return &nodeIt->second;
    }
};

class TFakeSchedulerCookie : public ISchedulerCookie {
public:
    bool Detach() noexcept override { delete this; return false; }
    bool DetachEvent() noexcept override { Y_FAIL(); }
    bool IsArmed() noexcept override { Y_FAIL(); }
};

} // NKikimr
