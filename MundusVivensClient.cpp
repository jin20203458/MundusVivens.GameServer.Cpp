#include "MundusVivensClient.h"
#include <grpcpp/create_channel.h>
#include <iostream>

namespace MundusVivens {

    MundusVivensClient::MundusVivensClient(std::shared_ptr<grpc::Channel> channel) {
        // 프로토콜 버퍼로 자동 생성된 인프라 스텁(Stub) 인스턴스를 생성하여 멤버 변수에 바인딩합니다.
        stub_ = mundusvivens::MundusVivensGrpc::NewStub(channel);
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

    bool MundusVivensClient::InjectBelief(uint32_t target_agent_id, uint32_t subject_id, const std::string& content, mundusvivens::ProtoBeliefType belief_type, uint32_t source_agent_id, std::string& out_message) {
        // 1. 믿음(소문)을 주입할 대상 NPC, 소문의 주인공, 내용 패킷 세팅
        mundusvivens::InjectBeliefRequest request;
        request.set_target_agent_id(target_agent_id);
        request.set_subject_id(subject_id);
        request.set_content(content);
        request.set_belief_type(belief_type);
        request.set_source_agent_id(source_agent_id);

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
                loc.type = static_cast<uint8_t>(proto_loc.type());
                loc.region_id = proto_loc.region_id();
                loc.territory_id = proto_loc.territory_id();
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
                furn.type = static_cast<uint8_t>(proto_furn.type());
                furn.parent_location = proto_furn.parent_location();
                furn.x = proto_furn.position().x();
                furn.y = proto_furn.position().y();
                furn.z = proto_furn.position().z();
                furn.is_temporary = proto_furn.is_temporary();
                result.furniture.push_back(furn);
            }
            result.npc_speed = response.npc_speed();
            result.ticks_per_game_hour = response.ticks_per_game_hour();
        }
        else {
            std::cerr << "[부트스트랩 에러] gRPC 통신 실패: " << status.error_message() << std::endl;
        }
        return result;
    }





} // namespace MundusVivens