#include "Systems.h"
#include "Components.h"
#include "GrpcResultQueue.h"
#include <iostream>

// 6. 변경 상태 배치 gRPC 동기화 시스템
void SystemNetworkSync(entt::registry& reg, MundusVivens::AsyncGrpcClient& client, GrpcResultQueue& grpc_queue) {
    std::vector<MundusVivens::AgentStatusUpdate> updates;
    std::vector<entt::entity> target_entities;
    auto view = reg.view<IdentityComp, LocationComp, EmotionComp, ActivityComp, LastSyncedComp>();

    view.each([&](entt::entity entity, IdentityComp& identity, LocationComp& loc, EmotionComp& emo, ActivityComp& act, LastSyncedComp& sync) {
        if (sync.location != loc.location_name || sync.emotion != emo.current_emotion || sync.activity != act.current_activity) {
            MundusVivens::AgentStatusUpdate update;
            update.agent_id = identity.npc_id;
            update.location = loc.location_name;
            update.x = loc.x;
            update.y = loc.y;
            update.z = loc.z;
            update.emotion = emo.current_emotion;
            update.activity = act.current_activity;
            updates.push_back(update);
            target_entities.push_back(entity);
        }
    });

    if (!updates.empty()) {
        std::vector<MundusVivens::AgentStatusUpdate> updates_for_callback = updates; // 콜백용 명시적 복사
        client.BatchUpdateStatusAsync(std::move(updates), [&grpc_queue, target_entities = std::move(target_entities), updates = std::move(updates_for_callback)](bool success, int32_t updated_count, const std::string& message) mutable {
            grpc_queue.Push([success, updated_count, message, target_entities = std::move(target_entities), updates = std::move(updates)](entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
                if (success) {
                    for (size_t i = 0; i < updates.size(); ++i) {
                        entt::entity target_ent = target_entities[i];
                        if (reg.valid(target_ent) && reg.all_of<LastSyncedComp>(target_ent)) {
                            auto& sync = reg.get<LastSyncedComp>(target_ent);
                            sync.location = updates[i].location;
                            sync.emotion = updates[i].emotion;
                            sync.activity = updates[i].activity;
                        }
                    }
                    std::cout << "🔄 [gRPC-Batch 비동기 완료] " << message << " (업데이트 개수: " << updated_count << ")" << std::endl;
                } else {
                    std::cerr << "❌ [gRPC-Batch 비동기 에러] 배치 업데이트 전송 실패: " << message << std::endl;
                }
            });
        });
    }
}

// 스냅샷 브로드캐스트 시스템
void SystemBroadcastWorldSnapshot(entt::registry& reg, TcpServer& tcp, int tick) {
    mundusvivens::WorldSnapshotPayload payload;
    payload.set_tick(tick);

    auto view = reg.view<IdentityComp, LocationComp, EmotionComp, ActivityComp>(entt::exclude<PlayerTag>);
    view.each([&](entt::entity entity, const IdentityComp& identity, const LocationComp& loc, const EmotionComp& emo, const ActivityComp& act) {
        auto* snapshot = payload.add_npcs();
        snapshot->set_npc_id(identity.npc_id);
        snapshot->set_display_name(identity.display_name);
        auto* loc_info = snapshot->mutable_location();
        loc_info->set_name(loc.location_name);
        auto* pos = loc_info->mutable_position();
        pos->set_x(loc.x);
        pos->set_y(loc.y);
        pos->set_z(loc.z);
        snapshot->set_emotion(emo.current_emotion);
        snapshot->set_activity(act.current_activity);

        if (reg.all_of<HealthComp>(entity)) {
            const auto& health = reg.get<HealthComp>(entity);
            snapshot->set_hp(health.hp);
            snapshot->set_max_hp(health.max_hp);
        } else {
            snapshot->set_hp(100.0f);
            snapshot->set_max_hp(100.0f);
        }

        if (reg.all_of<VelocityComp>(entity)) {
            const auto& vel = reg.get<VelocityComp>(entity);
            auto* vel_vec = snapshot->mutable_velocity();
            vel_vec->set_x(vel.dir_x * vel.speed);
            vel_vec->set_y(0.0f);
            vel_vec->set_z(vel.dir_z * vel.speed);
        }
    });

    BroadcastProto(tcp, PacketId::SC_WORLD_SNAPSHOT, payload);
}
