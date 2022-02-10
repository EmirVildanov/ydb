#include "grpc_request_proxy.h"

#include "rpc_calls.h"
#include "rpc_common.h"
#include "rpc_kqp_base.h"

#include <ydb/core/grpc_services/local_rpc/local_rpc.h>
 
#include <ydb/library/yql/public/issue/yql_issue_message.h>
#include <ydb/library/yql/public/issue/yql_issue.h>

namespace NKikimr {
namespace NGRpcService {

using namespace NActors;
using namespace Ydb;
using namespace NKqp;

class TCreateSessionRPC : public TRpcKqpRequestActor<TCreateSessionRPC, TEvCreateSessionRequest> {
    using TBase = TRpcKqpRequestActor<TCreateSessionRPC, TEvCreateSessionRequest>;

public:
    TCreateSessionRPC(IRequestOpCtx* msg)
        : TBase(msg) {}

    void Bootstrap(const TActorContext& ctx) {
        TBase::Bootstrap(ctx);

        Become(&TCreateSessionRPC::StateWork);

        auto now = TInstant::Now();

        if (Request().GetDeadline() <= now) {
            LOG_WARN_S(*TlsActivationContext, NKikimrServices::GRPC_PROXY,
                SelfId() << " Request deadline has expired for " << now - Request().GetDeadline() << " seconds");

            Reply(Ydb::StatusIds::TIMEOUT, ctx);
            return;
        }

        auto selfId = this->SelfId();
        auto as = TActivationContext::ActorSystem();

        Request_->SetClientLostAction([selfId, as]() {
            as->Send(selfId, new TEvents::TEvWakeup(EWakeupTag::WakeupTagClientLost));
        });

        CreateSessionImpl();
    }

private:
    void CreateSessionImpl() {
        const auto traceId = Request().GetTraceId();
        auto ev = MakeHolder<NKqp::TEvKqp::TEvCreateSessionRequest>();

        ev->Record.SetDeadlineUs(Request().GetDeadline().MicroSeconds());

        if (traceId) {
            ev->Record.SetTraceId(traceId.GetRef());
        }

        SetDatabase(ev, Request());

        Send(NKqp::MakeKqpProxyID(SelfId().NodeId()), ev.Release());
    }

    void StateWork(TAutoPtr<IEventHandle>& ev, const TActorContext& ctx) {
        switch (ev->GetTypeRewrite()) {
            HFunc(NKqp::TEvKqp::TEvCreateSessionResponse, Handle);
            HFunc(TEvents::TEvWakeup, Handle);
            // Overide default forget action which terminate this actor on client disconnect
            hFunc(TRpcServices::TEvForgetOperation, HandleForget);
            default: TBase::StateWork(ev, ctx);
        }
    }

    void HandleForget(TRpcServices::TEvForgetOperation::TPtr &ev) {
        Y_UNUSED(ev);
    }

    void Handle(TEvents::TEvWakeup::TPtr& ev, const TActorContext& ctx) {
        switch ((EWakeupTag) ev->Get()->Tag) {
            case EWakeupTag::WakeupTagClientLost:
                return HandleClientLost();
            default: TBase::HandleWakeup(ev, ctx);
        }
    }

    void HandleClientLost() {
        ClientLost = true;
    }

    void DoCloseSession(const TActorContext& ctx, const TString& sessionId) {
        Ydb::Table::DeleteSessionRequest request;
        request.set_session_id(sessionId);

        auto cb = [](const Ydb::Table::DeleteSessionResponse&){};

        auto database = Request_->GetDatabaseName().GetOrElse("");

        auto actorId = NRpcService::DoLocalRpcSameMailbox<NKikimr::NGRpcService::TEvDeleteSessionRequest>(
            std::move(request), std::move(cb), database, Request_->GetInternalToken(), ctx);

        LOG_NOTICE_S(*TlsActivationContext, NKikimrServices::GRPC_PROXY,
            SelfId() << " Client lost, session " << sessionId << " will be closed by " << actorId);
    }

    void Handle(NKqp::TEvKqp::TEvCreateSessionResponse::TPtr& ev, const TActorContext& ctx) {
        const auto& record = ev->Get()->Record;
        if (record.GetResourceExhausted()) {
            Request().ReplyWithRpcStatus(grpc::StatusCode::RESOURCE_EXHAUSTED, record.GetError());
            Die(ctx);
            return;
        }

        if (record.GetYdbStatus() == Ydb::StatusIds::SUCCESS) {
            const auto& kqpResponse = record.GetResponse();
            Ydb::Table::CreateSessionResult result;
            if (ClientLost) {
                DoCloseSession(ctx, kqpResponse.GetSessionId());
                // We already lost the client, so the client should not see this status
                Reply(Ydb::StatusIds::INTERNAL_ERROR, ctx);
            } else {
                result.set_session_id(kqpResponse.GetSessionId());
                return ReplyWithResult(Ydb::StatusIds::SUCCESS, result, ctx);
            }
        } else {
            return OnQueryResponseError(record, ctx);
        }
    }
private:

    bool ClientLost = false;

};

void TGRpcRequestProxy::Handle(TEvCreateSessionRequest::TPtr& ev, const TActorContext& ctx) {
    ctx.Register(new TCreateSessionRPC(ev->Release().Release()));
}

template<>
IActor* TEvCreateSessionRequest::CreateRpcActor(NKikimr::NGRpcService::IRequestOpCtx* msg) {
    return new TCreateSessionRPC(msg);
}

} // namespace NGRpcService
} // namespace NKikimr
