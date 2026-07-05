#include "MundusVivensClient.h"
#include <grpcpp/create_channel.h>
#include <iostream>

namespace MundusVivens {

    MundusVivensClient::MundusVivensClient(std::shared_ptr<grpc::Channel> channel) {
        // 프로토콜 버퍼로 자동 생성된 인프라 스텁(Stub) 인스턴스를 생성하여 멤버 변수에 바인딩합니다.
        stub_ = mundusvivens::MundusVivensGrpc::NewStub(channel);
    }

    DialogueResult MundusVivensClient::TriggerDialogue(uint32_t agent_id_a, uint32_t agent_id_b) {
        // 1. 원격 서버로 송신할 요청(Request) 패킷 데이터 설정
        mundusvivens::TriggerDialogueRequest request;
        request.set_agent_id_a(agent_id_a);
        request.set_agent_id_b(agent_id_b);

        mundusvivens::TriggerDialogueResponse response;
        grpc::ClientContext context; // RPC 호출의 타임아웃, 메타데이터 등을 관리하는 컨텍스트 객체

        // 2. 스텁 포인터를 통해 C# 서버의 원격 함수를 호출 (네트워크 송수신 발생)
        grpc::Status status = stub_->TriggerDialogue(&context, request, &response);

        // 3. 수신된 바이너리 패킷을 C++ 전용 구조체(DialogueResult)로 복사 및 반환
        DialogueResult result;
        if (status.ok()) {
            result.task_id = response.task_id();
            result.is_queued = response.is_queued();
            result.completed_immediately = response.completed_immediately();
            result.dialogue_summary = response.dialogue_summary();

            // protobuf가 뱉은 가변 배열(Repeated Field) 요소를 C++ std::vector에 순차적으로 밀어 넣음
            for (int i = 0; i < response.dialogue_lines_size(); ++i) {
                result.dialogue_lines.push_back(response.dialogue_lines(i));
            }

            // 🆕 구조화된 대화 데이터 바인딩 추가
            for (int i = 0; i < response.structured_lines_size(); ++i) {
                const auto& proto_line = response.structured_lines(i);
                DialogueLine line;
                line.speaker_id = proto_line.speaker_id();
                line.speaker_name = proto_line.speaker_name();
                line.text = proto_line.text();
                result.structured_lines.push_back(line);
            }

            // 🆕 감정 업데이트 정보 바인딩
            for (int i = 0; i < response.emotion_updates_size(); ++i) {
                const auto& proto_update = response.emotion_updates(i);
                AgentEmotionUpdate update;
                update.agent_id = proto_update.agent_id();
                update.new_emotion = proto_update.new_emotion();
                update.intensity = static_cast<int>(proto_update.intensity());
                result.emotion_updates.push_back(update);
            }

            // 🆕 다음 행동 계획 바인딩
            for (int i = 0; i < response.next_jobs_size(); ++i) {
                const auto& proto_job = response.next_jobs(i);
                JobPayload job;
                job.npc_id = proto_job.npc_id();
                job.job_id = proto_job.job_id();
                job.target_location = proto_job.target_location().name();
                job.target_x = proto_job.target_location().position().x();
                job.target_y = proto_job.target_location().position().y();
                job.target_z = proto_job.target_location().position().z();
                job.intent = proto_job.intent();
                job.target_agent_id = proto_job.target_agent_id();
                job.priority = proto_job.priority();
                result.next_jobs.push_back(job);
            }

            result.is_completed = response.completed_immediately();
        }
        else {
            std::cerr << "[대화 트리거 에러] gRPC 통신 실패: " << status.error_message() << std::endl;
            result.dialogue_summary = "gRPC 에러 발생: " + status.error_message();
        }

        return result;
    }

    AgentStatus MundusVivensClient::GetAgentStatus(uint32_t agent_id) {
        // 1. 특정 에이전트 식별자(ID)를 요청 패킷에 세팅
        mundusvivens::GetAgentStatusRequest request;
        request.set_agent_id(agent_id);

        mundusvivens::GetAgentStatusResponse response;
        grpc::ClientContext context;

        // 2. C# 서버로부터 해당 에이전트의 상태 데이터를 긁어옴
        grpc::Status status = stub_->GetAgentStatus(&context, request, &response);

        // 3. 수신 데이터를 C++용 상용 구조체(AgentStatus)로 매핑
        AgentStatus result;
        if (status.ok()) {
            result.name = response.name();
            result.location = response.location().name();
            result.x = response.location().position().x();
            result.y = response.location().position().y();
            result.z = response.location().position().z();
            result.emotion = response.emotion();
            result.activity = response.activity();

            // 에피소드 기억 목록을 std::vector에 누적
            for (int i = 0; i < response.memories_size(); ++i) {
                result.memories.push_back(response.memories(i));
            }
        }
        else {
            std::cerr << "[상태 조회 에러] gRPC 통신 실패: " << status.error_message() << std::endl;
        }

        return result;
    }

    bool MundusVivensClient::InjectBelief(uint32_t target_agent_id, uint32_t subject_id, const std::string& content, mundusvivens::ProtoBeliefType belief_type, std::string& out_message) {
        // 1. 믿음(소문)을 주입할 대상 NPC, 소문의 주인공, 내용 패킷 세팅
        mundusvivens::InjectBeliefRequest request;
        request.set_target_agent_id(target_agent_id);
        request.set_subject_id(subject_id);
        request.set_content(content);
        request.set_belief_type(belief_type);

        mundusvivens::InjectBeliefResponse response;
        grpc::ClientContext context;

        // 2. C# AI 세계관 내부로 믿음 데이터를 강제 주입 지시
        grpc::Status status = stub_->InjectBelief(&context, request, &response);

        // 3. 통신 상태 및 비즈니스 로직 처리 결과 반환
        if (status.ok()) {
            out_message = response.message(); // 서버 측 처리 결과 메시지 바인딩
            return response.success();        // 주입 성공 여부 (true/false)
        }
        else {
            out_message = "gRPC 에러 발생: " + status.error_message();
            return false;
        }
    }



    bool MundusVivensClient::BatchUpdateAgentStatus(const std::vector<AgentStatusUpdate>& updates, int32_t& out_updated_count, std::string& out_message) {
        mundusvivens::BatchUpdateAgentStatusRequest request;

        for (const auto& update : updates) {
            auto* agent_req = request.add_agents();
            agent_req->set_agent_id(update.agent_id);
            auto* loc = agent_req->mutable_location();
            loc->set_name(update.location);
            auto* pos = loc->mutable_position();
            pos->set_x(update.x);
            pos->set_y(update.y);
            pos->set_z(update.z);
            agent_req->set_emotion(update.emotion);
            agent_req->set_activity(update.activity);
        }

        mundusvivens::BatchUpdateAgentStatusResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->BatchUpdateAgentStatus(&context, request, &response);

        if (status.ok()) {
            out_updated_count = response.updated_count();
            out_message = "배치 상태 동기화 성공: " + std::to_string(out_updated_count) + "개 에이전트 업데이트 완료.";
            return true;
        }
        else {
            out_updated_count = 0;
            out_message = "gRPC 에러 발생: " + status.error_message();
            return false;
        }
    }

    bool MundusVivensClient::ProcessWorldTick(int32_t tick_number, std::string& out_message, std::vector<uint32_t>& out_busy_agent_ids) {
        // 1. C++ 메인 루프에서 전진한 현재의 월드 틱 넘버를 패킷에 세팅
        mundusvivens::ProcessWorldTickRequest request;
        request.set_tick_number(tick_number);

        mundusvivens::ProcessWorldTickResponse response;
        grpc::ClientContext context;

        // 2. AI 서버에 글로벌 시간(Tick) 동기화 신호를 전송
        grpc::Status status = stub_->ProcessWorldTick(&context, request, &response);

        // 3. 시간 동기화 완료 여부 반환
        if (status.ok()) {
            out_message = response.message();
            out_busy_agent_ids.clear();
            for (int i = 0; i < response.busy_agent_ids_size(); ++i) {
                out_busy_agent_ids.push_back(response.busy_agent_ids(i));
            }
            return response.success();
        }
        else {
            out_message = "gRPC 에러 발생: " + status.error_message();
            return false;
        }
    }


    WorldBootstrapData MundusVivensClient::GetWorldBootstrap() {
        mundusvivens::GetWorldBootstrapRequest request;
        mundusvivens::GetWorldBootstrapResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetWorldBootstrap(&context, request, &response);

        WorldBootstrapData result;
        if (status.ok()) {
            for (int i = 0; i < response.locations_size(); ++i) {
                const auto& proto_loc = response.locations(i);
                LocationData loc;
                loc.name = proto_loc.name();
                loc.x = proto_loc.position().x();
                loc.y = proto_loc.position().y();
                loc.z = proto_loc.position().z();
                result.locations.push_back(loc);
            }
            for (int i = 0; i < response.agents_size(); ++i) {
                const auto& proto_agent = response.agents(i);
                InitialAgentState agent;
                agent.agent_id = proto_agent.agent_id();
                agent.name = proto_agent.name();
                agent.location = proto_agent.location().name();
                agent.x = proto_agent.location().position().x();
                agent.y = proto_agent.location().position().y();
                agent.z = proto_agent.location().position().z();
                agent.emotion = proto_agent.emotion();
                agent.activity = proto_agent.activity();
                agent.extroversion = proto_agent.extroversion();
                agent.string_id = proto_agent.string_id();
                for (int j = 0; j < proto_agent.relationships_size(); ++j) {
                    const auto& proto_rel = proto_agent.relationships(j);
                    RelationshipSnapshot snapshot;
                    snapshot.target_agent_id = proto_rel.target_agent_id();
                    snapshot.liking = proto_rel.liking();
                    snapshot.trust = proto_rel.trust();
                    agent.relationships.push_back(snapshot);
                }
                result.agents.push_back(agent);
            }
            for (int i = 0; i < response.furniture_size(); ++i) {
                const auto& proto_furn = response.furniture(i);
                FurnitureData furn;
                furn.name = proto_furn.name();
                furn.type = proto_furn.type();
                furn.parent_location = proto_furn.parent_location();
                furn.x = proto_furn.position().x();
                furn.y = proto_furn.position().y();
                furn.z = proto_furn.position().z();
                result.furniture.push_back(furn);
            }
        }
        else {
            std::cerr << "[부트스트랩 에러] gRPC 통신 실패: " << status.error_message() << std::endl;
        }
        return result;
    }



    std::vector<MundusVivensClient::JobPayload> MundusVivensClient::GetPendingJobs(int32_t current_tick) {
        mundusvivens::GetPendingJobsRequest request;
        request.set_current_tick(current_tick);

        mundusvivens::GetPendingJobsResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetPendingJobs(&context, request, &response);

        std::vector<JobPayload> result;
        if (status.ok()) {
            for (int i = 0; i < response.jobs_size(); ++i) {
                const auto& proto_job = response.jobs(i);
                JobPayload job;
                job.npc_id = proto_job.npc_id();
                job.job_id = proto_job.job_id();
                job.target_location = proto_job.target_location().name();
                job.target_x = proto_job.target_location().position().x();
                job.target_y = proto_job.target_location().position().y();
                job.target_z = proto_job.target_location().position().z();
                job.intent = proto_job.intent();
                job.target_agent_id = proto_job.target_agent_id();
                job.priority = proto_job.priority();
                result.push_back(job);
            }
        } else {
            std::cerr << "[Pending Jobs 조회 에러] gRPC 통신 실패: " << status.error_message() << std::endl;
        }
        return result;
    }

    bool MundusVivensClient::ReportJobStatus(uint32_t npc_id, uint64_t job_id, int32_t status_val, mundusvivens::InterruptReason reason_code, const std::string& detailed_context, int32_t current_tick, JobPayload& out_new_job) {
        mundusvivens::ReportJobStatusRequest request;
        request.set_npc_id(npc_id);
        request.set_job_id(job_id);
        request.set_status(static_cast<mundusvivens::ReportJobStatusRequest_JobStatus>(status_val));
        request.set_reason_code(reason_code);
        request.set_detailed_context(detailed_context);
        request.set_current_tick(current_tick);

        mundusvivens::ReportJobStatusResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->ReportJobStatus(&context, request, &response);

        if (status.ok() && response.success()) {
            if (response.has_new_job()) {
                const auto& proto_job = response.new_job();
                out_new_job.npc_id = proto_job.npc_id();
                out_new_job.job_id = proto_job.job_id();
                out_new_job.target_location = proto_job.target_location().name();
                out_new_job.target_x = proto_job.target_location().position().x();
                out_new_job.target_y = proto_job.target_location().position().y();
                out_new_job.target_z = proto_job.target_location().position().z();
                out_new_job.intent = proto_job.intent();
                out_new_job.target_agent_id = proto_job.target_agent_id();
                out_new_job.priority = proto_job.priority();
            }
            return true;
        } else {
            std::cerr << "[Job 상태 보고 에러] gRPC 통신 실패: " << status.error_message() << std::endl;
            return false;
        }
    }

} // namespace MundusVivens