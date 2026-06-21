#include "MundusVivensClient.h"
#include <grpcpp/create_channel.h>
#include <iostream>

namespace MundusVivens {

    MundusVivensClient::MundusVivensClient(const std::string& server_address) {
        // Kestrel gRPC 서버와의 로컬 통신을 위해 보안 인증서가 없는 비보안(Insecure) 채널을 개설합니다.
        auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());

        // 프로토콜 버퍼로 자동 생성된 인프라 스텁(Stub) 인스턴스를 생성하여 멤버 변수에 바인딩합니다.
        stub_ = mundusvivens::MundusVivensGrpc::NewStub(channel);
    }

    DialogueResult MundusVivensClient::TriggerDialogue(const std::string& agent_id_a, const std::string& agent_id_b, bool wait_for_completion) {
        // 1. 원격 서버로 송신할 요청(Request) 패킷 데이터 설정
        mundusvivens::TriggerDialogueRequest request;
        request.set_agent_id_a(agent_id_a);
        request.set_agent_id_b(agent_id_b);
        request.set_wait_for_completion(wait_for_completion);

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
                result.emotion_updates.push_back(update);
            }

            result.is_completed = response.completed_immediately();
        }
        else {
            std::cerr << "[대화 트리거 에러] gRPC 통신 실패: " << status.error_message() << std::endl;
            result.dialogue_summary = "gRPC 에러 발생: " + status.error_message();
        }

        return result;
    }

    AgentStatus MundusVivensClient::GetAgentStatus(const std::string& agent_id) {
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
            result.location = response.location();
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

    bool MundusVivensClient::InjectGossip(const std::string& target_agent_id, const std::string& subject_id, const std::string& content, std::string& out_message) {
        // 1. 소문을 주입할 대상 NPC, 소문의 주인공, 소문 내용 패킷 세팅
        mundusvivens::InjectGossipRequest request;
        request.set_target_agent_id(target_agent_id);
        request.set_subject_id(subject_id);
        request.set_content(content);

        mundusvivens::InjectGossipResponse response;
        grpc::ClientContext context;

        // 2. C# AI 세계관 내부로 소문 데이터를 강제 주입 지시
        grpc::Status status = stub_->InjectGossip(&context, request, &response);

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

    bool MundusVivensClient::UpdateAgentStatus(const std::string& agent_id, const std::string& location, const std::string& emotion, const std::string& activity, std::string& out_message) {
        // 1. C++ 게임 월드에서 결정된 NPC의 최신 상태(위치, 감정, 현재 행동)를 패킷에 세팅
        mundusvivens::UpdateAgentStatusRequest request;
        request.set_agent_id(agent_id);
        request.set_location(location);
        request.set_emotion(emotion);
        request.set_activity(activity);

        mundusvivens::UpdateAgentStatusResponse response;
        grpc::ClientContext context;

        // 2. AI 서버 측의 에이전트 인스턴스 인메모리 데이터를 물리 월드 상태와 동기화
        grpc::Status status = stub_->UpdateAgentStatus(&context, request, &response);

        // 3. 동기화 성공 여부 정산
        if (status.ok()) {
            out_message = response.message();
            return response.success();
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
            agent_req->set_location(update.location);
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

    bool MundusVivensClient::ProcessWorldTick(int32_t tick_number, std::string& out_message) {
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
            return response.success();
        }
        else {
            out_message = "gRPC 에러 발생: " + status.error_message();
            return false;
        }
    }

    DialogueResult MundusVivensClient::TriggerDialogueAsync(const std::string& agent_id_a, const std::string& agent_id_b) {
        return TriggerDialogue(agent_id_a, agent_id_b, false);
    }

    DialogueResult MundusVivensClient::PollDialogueResult(const std::string& task_id) {
        mundusvivens::GetDialogueResultRequest request;
        request.set_task_id(task_id);

        mundusvivens::GetDialogueResultResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetDialogueResult(&context, request, &response);

        DialogueResult result;
        result.task_id = task_id;
        if (status.ok()) {
            result.is_completed = response.is_completed();
            result.dialogue_summary = response.dialogue_summary();
            
            if (result.is_completed) {
                // 구조화된 대화 데이터 바인딩
                for (int i = 0; i < response.lines_size(); ++i) {
                    const auto& proto_line = response.lines(i);
                    DialogueLine line;
                    line.speaker_id = proto_line.speaker_id();
                    line.speaker_name = proto_line.speaker_name();
                    line.text = proto_line.text();
                    result.structured_lines.push_back(line);

                    // 하위 호환성을 위해 텍스트 리스트 형태로도 채워줌
                    result.dialogue_lines.push_back(proto_line.speaker_name() + ": " + proto_line.text());
                }

                // 🆕 감정 업데이트 정보 바인딩
                for (int i = 0; i < response.emotion_updates_size(); ++i) {
                    const auto& proto_update = response.emotion_updates(i);
                    AgentEmotionUpdate update;
                    update.agent_id = proto_update.agent_id();
                    update.new_emotion = proto_update.new_emotion();
                    result.emotion_updates.push_back(update);
                }
            }
        }
        else {
            std::cerr << "[대화 결과 조회 에러] gRPC 통신 실패: " << status.error_message() << std::endl;
            result.is_completed = false; // 에러 발생 시 타임아웃 처리를 위해 완료 처리 지연
            result.has_error = true;     // 🆕 에러 상태 플래그 설정
            result.dialogue_summary = "gRPC 에러 발생: " + status.error_message();
        }

        return result;
    }

    WorldBootstrapData MundusVivensClient::GetWorldBootstrap() {
        mundusvivens::GetWorldBootstrapRequest request;
        mundusvivens::GetWorldBootstrapResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetWorldBootstrap(&context, request, &response);

        WorldBootstrapData result;
        if (status.ok()) {
            for (int i = 0; i < response.locations_size(); ++i) {
                result.locations.push_back(response.locations(i));
            }
            for (int i = 0; i < response.agents_size(); ++i) {
                const auto& proto_agent = response.agents(i);
                InitialAgentState agent;
                agent.agent_id = proto_agent.agent_id();
                agent.name = proto_agent.name();
                agent.location = proto_agent.location();
                agent.emotion = proto_agent.emotion();
                agent.activity = proto_agent.activity();
                result.agents.push_back(agent);
            }
        }
        else {
            std::cerr << "[부트스트랩 에러] gRPC 통신 실패: " << status.error_message() << std::endl;
        }
        return result;
    }

    std::vector<DailySchedule> MundusVivensClient::GetDailySchedules(int32_t current_tick) {
        mundusvivens::GetDailySchedulesRequest request;
        request.set_current_tick(current_tick);

        mundusvivens::GetDailySchedulesResponse response;
        grpc::ClientContext context;

        grpc::Status status = stub_->GetDailySchedules(&context, request, &response);

        std::vector<DailySchedule> result;
        if (status.ok()) {
            for (int i = 0; i < response.schedules_size(); ++i) {
                const auto& proto_sched = response.schedules(i);
                DailySchedule schedule;
                schedule.agent_id = proto_sched.agent_id();
                
                for (int j = 0; j < proto_sched.items_size(); ++j) {
                    const auto& proto_item = proto_sched.items(j);
                    DailyScheduleItem item;
                    item.start_hour = proto_item.start_hour();
                    item.end_hour = proto_item.end_hour();
                    item.target_location = proto_item.target_location();
                    item.activity = proto_item.activity();
                    schedule.items.push_back(item);
                }
                result.push_back(schedule);
            }
        }
        else {
            std::cerr << "[스케줄 조회 에러] gRPC 통신 실패: " << status.error_message() << std::endl;
        }
        return result;
    }

} // namespace MundusVivens