#pragma once
#include <entt/entt.hpp>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include "AsyncGrpcClient.h"
#include "SpatialHashGrid.h"

// 비동기 진행 중인 대화 트래킹을 위한 구조체
struct PendingDialogue {
    std::string task_id;
    entt::entity npc_a = entt::null;
    entt::entity npc_b = entt::null;
    int triggered_tick = 0;
    std::string meeting_location; // 대화 시작 장소 스냅샷
    bool poll_requested = false;  // 🆕 중복 비동기 폴링을 방지하기 위한 플래그
};

// 헬퍼 함수들
std::string GetCooldownKey(const std::string& a, const std::string& b);

bool IsNPCFocusedOnActivity(const std::string& activity, double roll);

// 바쁨 상태 동기화 시스템 (IsNpcBusy 대체)
void SystemUpdateBusyState(entt::registry& reg,
                           const std::unordered_map<std::string, PendingDialogue>& pendings,
                           const std::unordered_set<std::string>& busyAgentIdsFromCSharp);

// 시스템 함수 정의
void SystemEmotionDecay(entt::registry& reg);

void SystemScheduleMovement(entt::registry& reg, SpatialHashGrid& grid, int current_tick);

void SystemPollDialogueResults(entt::registry& reg, SpatialHashGrid& grid,
                               MundusVivens::AsyncGrpcClient& client, int tick,
                               std::unordered_map<std::string, PendingDialogue>& pendingDialogues,
                               std::unordered_map<std::string, int>& dialogueCooldowns);

void SystemSpatialDialogueTrigger(entt::registry& reg, SpatialHashGrid& grid,
                                  MundusVivens::AsyncGrpcClient& client, int tick,
                                  std::unordered_map<std::string, PendingDialogue>& pendingDialogues,
                                  const std::unordered_map<std::string, int>& dialogueCooldowns,
                                  std::mt19937& gen, std::uniform_real_distribution<>& dis);

void SystemCooldownSweep(entt::registry& reg, int tick, std::unordered_map<std::string, int>& dialogueCooldowns);

void SystemNetworkSync(entt::registry& reg, MundusVivens::AsyncGrpcClient& client);

// 플레이어 및 클라이언트 네트워크 연동 시스템
class TcpServer;
void SystemCleanupDisconnectedPlayerDialogues(entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client);

void SystemPlayerCommands(entt::registry& reg, SpatialHashGrid& grid, TcpServer& tcp,
                          MundusVivens::AsyncGrpcClient& async_client, int tick);

void SystemBroadcastWorldSnapshot(entt::registry& reg, TcpServer& tcp, int tick);


