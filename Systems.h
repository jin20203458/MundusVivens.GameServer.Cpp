#pragma once
#include <entt/entt.hpp>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include "AsyncGrpcClient.h"
#include "SpatialHashGrid.h"

#include <boost/container/small_vector.hpp>

class TcpServer;
class GrpcResultQueue;

// 헬퍼 함수들
bool IsNPCFocusedOnActivity(const std::string& activity, double roll);

// 시스템 함수 정의
void SystemEmotionDecay(entt::registry& reg);

void SystemJobDriver(entt::registry& reg, SpatialHashGrid& grid, int current_tick, MundusVivens::AsyncGrpcClient& client, GrpcResultQueue& grpc_queue);


void SystemSpatialDialogueTrigger(entt::registry& reg, SpatialHashGrid& grid,
                                  MundusVivens::AsyncGrpcClient& client, int tick,
                                  std::mt19937& gen, std::uniform_real_distribution<>& dis,
                                  GrpcResultQueue& grpc_queue);

void SystemNetworkSync(entt::registry& reg, MundusVivens::AsyncGrpcClient& client, GrpcResultQueue& grpc_queue);

// 플레이어 및 클라이언트 네트워크 연동 시스템
void SystemCleanupDisconnectedPlayerDialogues(entt::registry& reg, SpatialHashGrid& grid, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client, GrpcResultQueue& grpc_queue);

void SystemPlayerCommands(entt::registry& reg, SpatialHashGrid& grid, TcpServer& tcp,
                          MundusVivens::AsyncGrpcClient& async_client, int tick, GrpcResultQueue& grpc_queue);

void SystemBroadcastWorldSnapshot(entt::registry& reg, TcpServer& tcp, int tick);


