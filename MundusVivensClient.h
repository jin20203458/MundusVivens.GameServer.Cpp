#pragma once

#include <string>
#include <vector>
#include <memory>
#include <grpcpp/grpcpp.h>
#include "mundus_vivens.grpc.pb.h"

namespace MundusVivens {

	/// 일회성 초기화 데이터 (이후 런타임 메모리(ECS Registry)로 변환)

	// 관계 초기값
	struct RelationshipSnapshot {
		uint32_t target_agent_id;
		int32_t liking;
		int32_t trust;
	};

	// 에이전트 상태 초기값
	struct InitialAgentState {
		uint32_t agent_id;
		std::string string_id;
		std::string name;

		std::string location;
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;

		// 감정, 활동, 외향성 초기값
		std::string emotion;
		std::string activity;
		float extroversion = 0.5f;

		std::vector<RelationshipSnapshot> relationships;
	};

	struct LocationData {
		std::string name;
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		uint8_t type = 0;
		uint32_t region_id = 0;
		uint32_t territory_id = 0;
	};

	struct FurnitureData {
		std::string name;
		uint8_t type = 0;
		std::string parent_location;
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		bool is_temporary = false;
	};

	struct WorldBootstrapData {
		std::vector<LocationData> locations;
		std::vector<InitialAgentState> agents;
		std::vector<FurnitureData> furniture;
		float npc_speed = 0.0f;
		uint32_t ticks_per_game_hour = 0;
	};


	/// 실시간 대화 및 결과 전파

	struct AgentEmotionUpdate {
		uint32_t agent_id;
		std::string new_emotion;
		int intensity = 0;     // 0: Unspecified, 1: Low, 2: Medium, 3: High
		uint8_t category = 0;  // 0: Unspecified, 1: Neutral, 2: Anger, 3: Hostility, 4: Fear
	};

	/// 물리 행동 및 실시간 동기화 

	struct JobPayload {
		uint32_t npc_id;               //  대상 NPC ID
		uint64_t job_id;               //  고유 Job ID
		std::string target_location;   //  목적지  이름
		float target_x = 0.0f;         //  목적지 x 좌표
		float target_y = 0.0f;         //  목적지 y 좌표
		float target_z = 0.0f;         //  목적지 z 좌표
		std::string intent;            //  행동 의도 
		uint32_t target_agent_id = 0;  //  상호작용 대상 NPC ID
		int32_t priority = 0;          //  우선순위
		uint8_t category = 0;          //  Job 카테고리 enum
	};

	struct DialogueLine {
		uint32_t speaker_id;
		std::string speaker_name;
		std::string text;
	};

	struct DialogueResult {
		uint64_t task_id;
		std::string dialogue_summary;
		std::vector<DialogueLine> structured_lines;
		std::vector<AgentEmotionUpdate> emotion_updates;
		std::vector<JobPayload> next_jobs;               // 대화로 파생된 후속 일감
		std::vector<std::string> keywords;               // 대화 키워드 목록(엿듣기)
		bool success = false;                            // 대화 생성 성공 여부
		std::string error_message;                       // 대화 에러 메시지
	};


	// 실시간 관계 변동치
	struct RelationshipDelta {
		uint32_t from_agent_id;
		uint32_t to_agent_id;
		int32_t liking;
		int32_t trust;
	};

	//  에이전트 상태를 배치 업데이트할 때 전달되는 구조체
	struct AgentStatusUpdate {
		uint32_t agent_id;
		std::string location;
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		std::string emotion;
		std::string activity;
	};

	//  [예약] 에이전트의 실시간 상태를 조회할 때 반환되는 구조체
	struct AgentStatus {
		std::string name;
		std::string location;
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		std::string emotion;
		std::string activity;
		std::vector<std::string> memories;
	};


	class MundusVivensClient {
	public:
		// C# AI 서버의 공유 gRPC 채널을 전달받아 클라이언트를 생성합니다.
		MundusVivensClient(std::shared_ptr<grpc::Channel> channel);


		// [예약] 특정 에이전트의 실시간 상태(위치, 감정, 에피소드 기억 요약 등)를 조회합니다.
		// 유니티 ◄► C++ 간의 TCP 브릿지 패킷(예: CS_INSPECT_NPC_MEMORIES) 추가 시 활성화할 예정입니다.
		AgentStatus GetAgentStatus(uint32_t agent_id);

		// 특정 에이전트에게 믿음(소문)을 강제로 주입합니다.
		bool InjectBelief(uint32_t target_agent_id, uint32_t subject_id, const std::string& content, mundusvivens::ProtoBeliefType belief_type, uint32_t source_agent_id, std::string& out_message);


		//  에이전트 상태 배치 업데이트 RPC 
		bool BatchUpdateAgentStatus(const std::vector<AgentStatusUpdate>& updates, int32_t& out_updated_count, std::string& out_message);


		//  월드 부트스트랩 데이터 조회
		WorldBootstrapData GetWorldBootstrap();


		using JobPayload = MundusVivens::JobPayload;

	private:
		std::unique_ptr<mundusvivens::MundusVivensGrpc::Stub> stub_;
	};

} // namespace MundusVivens
