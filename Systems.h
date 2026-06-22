#pragma once
#include <entt/entt.hpp>
#include <vector>
#include <string>
#include <map>
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
bool IsNpcBusy(entt::entity e, const std::vector<PendingDialogue>& pendings,
               const std::vector<std::string>& busyAgentIdsFromCSharp, entt::registry& reg);

std::string GetCooldownKey(const std::string& a, const std::string& b);

bool IsNPCFocusedOnActivity(const std::string& activity, double roll);

// 6개의 독립적인 시스템 함수 정의 (AsyncGrpcClient 적용)
void SystemEmotionDecay(entt::registry& reg, const std::vector<PendingDialogue>& pendings,
                        const std::vector<std::string>& busyAgentIdsFromCSharp);

void SystemScheduleMovement(entt::registry& reg, SpatialHashGrid& grid, int current_tick,
                            const std::vector<PendingDialogue>& pendings,
                            const std::vector<std::string>& busyAgentIdsFromCSharp);

void SystemPollDialogueResults(entt::registry& reg, SpatialHashGrid& grid,
                               MundusVivens::AsyncGrpcClient& client, int tick,
                               std::vector<PendingDialogue>& pendingDialogues,
                               std::map<std::string, int>& dialogueCooldowns);

void SystemSpatialDialogueTrigger(entt::registry& reg, SpatialHashGrid& grid,
                                  MundusVivens::AsyncGrpcClient& client, int tick,
                                  std::vector<PendingDialogue>& pendingDialogues,
                                  const std::map<std::string, int>& dialogueCooldowns,
                                  const std::vector<std::string>& busyAgentIdsFromCSharp,
                                  std::mt19937& gen, std::uniform_real_distribution<>& dis);

void SystemCooldownSweep(entt::registry& reg, int tick, std::map<std::string, int>& dialogueCooldowns);

void SystemNetworkSync(entt::registry& reg, MundusVivens::AsyncGrpcClient& client);

// 🆕 플레이어 및 클라이언트 네트워크 연동 시스템
class TcpServer;
void SystemPlayerCommands(entt::registry& reg, SpatialHashGrid& grid, TcpServer& tcp,
                          MundusVivens::AsyncGrpcClient& async_client, int tick,
                          std::vector<PendingDialogue>& pendingDialogues);

void SystemBroadcastWorldSnapshot(entt::registry& reg, TcpServer& tcp, int tick);


