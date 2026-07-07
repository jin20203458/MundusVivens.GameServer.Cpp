#pragma once
#include <entt/entt.hpp>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <array>
#include "AsyncGrpcClient.h"
#include "SpatialHashGrid.h"
#include "GridMap.h"
#include "Components.h"
#include "TcpServer.h"
#include "mundus_vivens.pb.h"
#include <boost/container/small_vector.hpp>

class TcpServer;
class GrpcResultQueue;

// =========================================================================
// 공유 유틸리티 및 템플릿 헬퍼 함수 (Header Inline)
// =========================================================================

// 헬퍼 함수: 에이전트 문자열 ID와 정수 ID 간의 양방향 매핑 (컨텍스트 레지스트리 참조)
inline uint32_t GetAgentNumericId(const entt::registry& reg, const std::string& string_id) {
    if (reg.ctx().contains<AgentIdMapper>()) {
        const auto& mapper = reg.ctx().get<AgentIdMapper>();
        auto it = mapper.string_to_numeric.find(string_id);
        if (it != mapper.string_to_numeric.end()) {
            return it->second;
        }
    }
    return 0;
}

inline std::string GetAgentStringId(const entt::registry& reg, uint32_t numeric_id) {
    if (reg.ctx().contains<AgentIdMapper>()) {
        const auto& mapper = reg.ctx().get<AgentIdMapper>();
        auto it = mapper.numeric_to_string.find(numeric_id);
        if (it != mapper.numeric_to_string.end()) {
            return it->second;
        }
    }
    return "";
}

// 헬퍼 함수: NPC가 현재 진행 중인 활동(Activity)에 집중하여 대화를 피할지를 하드 판정
inline bool IsNPCFocusedOnActivity(const std::string& activity, double roll) {
    if (activity.find("취침") != std::string::npos || activity.find("휴식") != std::string::npos) {
        // 취침/휴식 중에는 대화 불가
        return true;
    }
    if (activity.find("기도") != std::string::npos || activity.find("명상") != std::string::npos) {
        // 기도/명상 중에는 80% 확률로 대화 불가
        return roll < 0.8;
    }
    // 일반 활동(산책, 노동 등)은 무조건 허용
    return false;
}

// Protobuf 메시지 직렬화 및 전송을 위한 템플릿 헬퍼 (송신 Zero-Allocation)
template <typename T>
inline bool SendProto(TcpServer& tcp, uint32_t session_index, uint16_t packet_id, const T& message) {
    thread_local static std::array<uint8_t, MAX_PACKET_SIZE> buffer;
    size_t size = message.ByteSizeLong();
    if (size > MAX_PACKET_SIZE || !message.SerializeToArray(buffer.data(), static_cast<int>(size))) {
        return false;
    }
    tcp.SendTo(session_index, packet_id, buffer.data(), size);
    return true;
}

template <typename T>
inline bool BroadcastProto(TcpServer& tcp, uint16_t packet_id, const T& message) {
    thread_local static std::array<uint8_t, MAX_PACKET_SIZE> buffer;
    size_t size = message.ByteSizeLong();
    if (size > MAX_PACKET_SIZE || !message.SerializeToArray(buffer.data(), static_cast<int>(size))) {
        return false;
    }
    tcp.BroadcastPacket(packet_id, buffer.data(), size);
    return true;
}

// =========================================================================
// 시스템 함수 선언
// =========================================================================

void SystemEmotionDecay(entt::registry& reg);

void SystemJobDriver(entt::registry& reg, SpatialHashGrid& grid, int current_tick, MundusVivens::AsyncGrpcClient& client, GrpcResultQueue& grpc_queue);

void SystemPathfinding(entt::registry& reg, const GridMap& map);

void SystemMovement(entt::registry& reg, SpatialHashGrid& grid, int tick);

float GetLocationSocialModifier(const std::string& location_name);

void SystemSocialInteraction(entt::registry& reg, SpatialHashGrid& grid, TcpServer& tcp,
                                  MundusVivens::AsyncGrpcClient& client, int tick,
                                  std::mt19937& gen, std::uniform_real_distribution<>& dis,
                                  GrpcResultQueue& grpc_queue);

void SystemNetworkSync(entt::registry& reg, MundusVivens::AsyncGrpcClient& client, GrpcResultQueue& grpc_queue);

void SystemCleanupDisconnectedPlayerDialogues(entt::registry& reg, SpatialHashGrid& grid, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client, GrpcResultQueue& grpc_queue);

void SystemPlayerCommands(entt::registry& reg, SpatialHashGrid& grid, TcpServer& tcp,
                          MundusVivens::AsyncGrpcClient& async_client, int tick, GrpcResultQueue& grpc_queue);

void SystemBroadcastWorldSnapshot(entt::registry& reg, TcpServer& tcp, int tick);

void SystemBusyAmbient(entt::registry& reg, float deltaTime);

void SystemSurvivalOverride(entt::registry& reg, SpatialHashGrid& grid, int current_tick, MundusVivens::AsyncGrpcClient& client, GrpcResultQueue& grpc_queue);

void SystemAffordanceResolver(entt::registry& reg, SpatialHashGrid& grid, int current_tick, MundusVivens::AsyncGrpcClient& client, GrpcResultQueue& grpc_queue);


