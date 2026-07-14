#include "Systems.h"
#include "Components.h"
#include "TcpServer.h"
#include "ClientSession.h"
#include "mundus_vivens.pb.h"
#include "GrpcResultQueue.h"
#include <iostream>
#include "TracyIntegration.h"

// 7. 연결 끊긴 플레이어의 대화 및 좀비 엔티티 정리 시스템
void SystemCleanupDisconnectedPlayerDialogues(entt::registry& reg, LocationRegistry& grid, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client, GrpcResultQueue& grpc_queue) {
    std::vector<entt::entity> to_cleanup;
    auto view = reg.view<PlayerTag>();
    view.each([&](entt::entity entity, const PlayerTag& tag) {
        if (tcp.GetSession(tag.session_index) == nullptr) {
            to_cleanup.push_back(entity);
        }
    });

    if (to_cleanup.empty()) return;

    auto& index = reg.ctx().get<EntityIndex>();

    for (auto player_ent : to_cleanup) {
        const auto& tag = reg.get<PlayerTag>(player_ent);
        std::cout << "⚠️ [네트워크 끊김 감지] 플레이어 세션(" << tag.session_index 
                  << ") 연결 끊김 확인. 강제 정리를 시작합니다." << std::endl;

        // 대화 중이었던 경우 대화 종료 및 NPC 해방
        if (reg.all_of<PlayerDialogueComp>(player_ent)) {
            const auto& pdc = reg.get<PlayerDialogueComp>(player_ent);
            std::cout << "  - 진행 중이던 대화(Session: " << pdc.session_id << ")를 종료하고 NPC를 해방합니다." << std::endl;

            // C# AI 서버에 종료 신호 전송 (비동기)
            async_client.EndPlayerDialogueAsync(pdc.session_id, [&grpc_queue](bool success, const std::string& summary) {
                grpc_queue.Push([success, summary](entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
                    std::cout << "💬 [비정상 종료 대화 정리 완료] C# AI 서버 대화 종료 응답 수신: " << summary << std::endl;
                });
            });

            // NPC 상태 정상 복귀
            if (reg.valid(pdc.npc_entity)) {
                if (reg.all_of<ActivityComp>(pdc.npc_entity)) {
                    reg.get<ActivityComp>(pdc.npc_entity).current_activity = "대기";
                }
                if (reg.all_of<BusyTag>(pdc.npc_entity)) {
                    reg.erase<BusyTag>(pdc.npc_entity);
                }
            }
        }

        // Spatial Hash Grid에서 플레이어 삭제
        if (reg.all_of<LocationComp>(player_ent)) {
            const auto& loc = reg.get<LocationComp>(player_ent);
            grid.RemoveEntity(player_ent);
        }

        // EntityIndex 맵핑 소거
        index.by_session_index.erase(tag.session_index);
        if (reg.all_of<IdentityComp>(player_ent)) {
            index.by_npc_id.erase(reg.get<IdentityComp>(player_ent).npc_id);
        }

        // 플레이어 엔티티 완전 제거
        reg.destroy(player_ent);
        std::cout << "👤 [플레이어 제거 완료] 좀비 엔티티 및 리소스 삭제 완료." << std::endl;
    }
}

// 8. 플레이어 커맨드 처리 시스템
void SystemPlayerCommands(entt::registry& reg, LocationRegistry& grid, TcpServer& tcp,
                          MundusVivens::AsyncGrpcClient& async_client, int tick, GrpcResultQueue& grpc_queue) {
    ZoneScoped;
    static std::vector<PlayerCommand> local_commands;
    tcp.DrainPlayerCommands(local_commands);
    
    auto& index = reg.ctx().get<EntityIndex>();

    for (const auto& cmd : local_commands) {
        if (cmd.type == PlayerCommand::Login) {
            mundusvivens::LoginRequest req;
            if (!req.ParseFromArray(cmd.payload.data(), static_cast<int>(cmd.payload.size()))) continue;

            // 플레이어 검색 혹은 신규 생성
            entt::entity player_ent = entt::null;
            auto it = index.by_npc_id.find(GetAgentNumericId(reg, req.player_id()));
            if (it != index.by_npc_id.end()) {
                player_ent = it->second;
            }

            if (player_ent == entt::null) {
                player_ent = reg.create();
                reg.emplace<PlayerTag>(player_ent, cmd.session_index);
                reg.emplace<IdentityComp>(player_ent, GetAgentNumericId(reg, req.player_id()), req.player_name());
                reg.emplace<LocationComp>(player_ent, 0u, "광장");
                reg.emplace<LastSyncedComp>(player_ent);

                index.by_npc_id[GetAgentNumericId(reg, req.player_id())] = player_ent;
                index.by_session_index[cmd.session_index] = player_ent;

                std::cout << "👤 [플레이어 로그인] 신규 플레이어 등록: " << req.player_name() << " (ID: " << req.player_id() << ")" << std::endl;
            } else {
                // 재접속 시 세션 인덱스 최신화
                uint32_t old_session = reg.get<PlayerTag>(player_ent).session_index;
                index.by_session_index.erase(old_session);

                reg.get<PlayerTag>(player_ent).session_index = cmd.session_index;
                index.by_session_index[cmd.session_index] = player_ent;
                std::cout << "👤 [플레이어 재접속] 기존 플레이어 세션 갱신: " << req.player_name() << std::endl;
            }

            // 플레이어 공간 등록
            auto& loc = reg.get<LocationComp>(player_ent);
            grid.UpdateEntityPosition(player_ent, loc.x, loc.z, reg);

            // 로그인 응답 패킷 작성
            mundusvivens::LoginResponse resp;
            resp.set_success(true);
            resp.set_message("로그인에 성공했습니다.");

            if (reg.ctx().contains<MundusVivens::WorldBootstrapData>())
            {
                const auto& bootstrap = reg.ctx().get<MundusVivens::WorldBootstrapData>();
                for (const auto& loc : bootstrap.locations)
                {
                    auto* new_loc = resp.add_locations();
                    new_loc->set_name(loc.name);
                    auto* pos = new_loc->mutable_position();
                    pos->set_x(loc.x);
                    pos->set_y(loc.y);
                    pos->set_z(loc.z);
                }
            }

            // 현재 NPC 전체 상태 추가
            auto npc_view = reg.view<IdentityComp, LocationComp, EmotionComp, ActivityComp>(entt::exclude<PlayerTag>);
            npc_view.each([&](const IdentityComp& npc_id, const LocationComp& npc_loc, const EmotionComp& npc_emo, const ActivityComp& npc_act) {
                auto* snapshot = resp.add_npcs();
                snapshot->set_npc_id(npc_id.npc_id);
                snapshot->set_display_name(npc_id.display_name);
                auto* loc_info = snapshot->mutable_location();
                loc_info->set_name(npc_loc.location_name);
                auto* pos = loc_info->mutable_position();
                pos->set_x(npc_loc.x);
                pos->set_y(npc_loc.y);
                pos->set_z(npc_loc.z);
                snapshot->set_emotion(npc_emo.current_emotion);
                snapshot->set_activity(npc_act.current_activity);
            });

            SendProto(tcp, cmd.session_index, PacketId::SC_LOGIN_ACK, resp);
        }
        else if (cmd.type == PlayerCommand::Move) {
            mundusvivens::PlayerMoveRequest req;
            if (!req.ParseFromArray(cmd.payload.data(), static_cast<int>(cmd.payload.size()))) continue;

            entt::entity player_ent = entt::null;
            auto it = index.by_session_index.find(cmd.session_index);
            if (it != index.by_session_index.end()) {
                player_ent = it->second;
            }

            if (player_ent != entt::null) {
                auto& loc = reg.get<LocationComp>(player_ent);
                const auto& identity = reg.get<IdentityComp>(player_ent);
                std::string old_loc = loc.location_name;
                std::string new_loc = req.target_location().name();
                float target_x = req.target_location().position().x();
                float target_y = req.target_location().position().y();
                float target_z = req.target_location().position().z();

                loc.x = target_x;
                loc.y = target_y;
                loc.z = target_z;
                grid.UpdateEntityPosition(player_ent, loc.x, loc.z, reg);

                if (old_loc != new_loc) {
                    std::cout << "🏃 [TCP 플레이어 구역 이동] 플레이어 " << identity.display_name 
                              << " 이동: [" << old_loc << "] ➔ [" << new_loc << "]" << std::endl;
                }
            }
        }
        else if (cmd.type == PlayerCommand::TalkToNpc) {
            mundusvivens::TalkToNpcRequest req;
            if (!req.ParseFromArray(cmd.payload.data(), static_cast<int>(cmd.payload.size()))) continue;

            entt::entity player_ent = entt::null;
            auto it = index.by_session_index.find(cmd.session_index);
            if (it != index.by_session_index.end()) {
                player_ent = it->second;
            }

            if (player_ent == entt::null) continue;
            const auto& player_loc = reg.get<LocationComp>(player_ent);
            const auto& player_identity = reg.get<IdentityComp>(player_ent);

            // 대화 대상 NPC 엔티티 검색
            entt::entity npc_ent = entt::null;
            auto npc_it = index.by_npc_id.find(req.npc_id());
            if (npc_it != index.by_npc_id.end()) {
                npc_ent = npc_it->second;
            }

            if (npc_ent == entt::null) {
                std::cerr << "❌ [플레이어 대화 에러] NPC를 찾을 수 없음: " << req.npc_id() << std::endl;
                continue;
            }

            if (player_ent == npc_ent) {
                std::cerr << "❌ [플레이어 대화 에러] 자기 자신과는 대화할 수 없습니다." << std::endl;
                continue;
            }

            if (reg.all_of<PlayerTag>(npc_ent)) {
                std::cerr << "❌ [플레이어 대화 에러] 다른 플레이어와는 AI 대화를 진행할 수 없습니다." << std::endl;
                continue;
            }

            const auto& npc_loc = reg.get<LocationComp>(npc_ent);
            const auto& npc_id = reg.get<IdentityComp>(npc_ent);

            // 동일 구역인지 검증
            float dx = player_loc.x - npc_loc.x;
            float dz = player_loc.z - npc_loc.z;
            float dist = std::sqrt(dx*dx + dz*dz);
            if (player_loc.location_name != npc_loc.location_name || dist > 8.0f) {
                std::cerr << "❌ [플레이어 대화 에러] 플레이어와 NPC가 다른 구역에 있거나 너무 멉니다. 거리: " 
                          << dist << "m, 플레이어: " << player_loc.location_name 
                          << ", NPC: " << npc_loc.location_name << std::endl;
                continue;
            }

            // NPC 대화 불가 여부 검증
            if (reg.all_of<BusyTag>(npc_ent)) {
                std::cout << "⏳ [플레이어 대화 불가] NPC " << npc_id.display_name << "은(는) 이미 대화 중입니다." << std::endl;
                continue;
            }

            std::cout << "💬 [플레이어 대화 시작] 플레이어와 NPC " << npc_id.display_name << " 대화 시도..." << std::endl;

            uint32_t session_idx = cmd.session_index;
            std::string npc_name = npc_id.display_name;
            
            // NPC 임시 대화 대기 상태
            reg.get<ActivityComp>(npc_ent).current_activity = "플레이어와 대화 대기";
            reg.emplace_or_replace<BusyTag>(npc_ent);
            reg.emplace_or_replace<BusyTag>(player_ent);

            auto session = tcp.GetSession(session_idx);
            if (session) session->IncrementPendingGrpc();

            async_client.StartPlayerDialogueAsync(GetAgentStringId(reg, player_identity.npc_id), req.npc_id(), 
                [&grpc_queue, tcp_addr = &tcp, session_idx, npc_name, npc_ent, player_ent, session](bool success, uint64_t session_id, const std::string& greeting, const std::string& message) {
                    if (session) session->DecrementPendingGrpc();
                    grpc_queue.Push([success, session_id, greeting, message, tcp_addr, session_idx, npc_name, npc_ent, player_ent](entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
                        if (success) {
                            std::cout << "🚀 플레이어 대화 세션 오픈 성공: " << session_id << std::endl;
                            if (reg.valid(npc_ent)) {
                                reg.get<ActivityComp>(npc_ent).current_activity = "플레이어와 대화 중";
                            }
                            if (reg.valid(player_ent)) {
                                reg.emplace_or_replace<PlayerDialogueComp>(player_ent, session_id, npc_ent);
                            }
                            mundusvivens::NpcReplyPayload reply;
                            reply.set_session_id(session_id);
                            reply.set_npc_name(npc_name);
                            reply.set_reply_text(greeting);

                            SendProto(*tcp_addr, session_idx, PacketId::SC_NPC_REPLY, reply);
                        } else {
                            std::cerr << "❌ [플레이어 대화 실패] 대화 세션을 생성하지 못했습니다. 사유: " << message << std::endl;
                            if (reg.valid(npc_ent)) {
                                reg.get<ActivityComp>(npc_ent).current_activity = "대기";
                                if (reg.all_of<BusyTag>(npc_ent)) reg.erase<BusyTag>(npc_ent);
                            }
                            if (reg.valid(player_ent) && reg.all_of<BusyTag>(player_ent)) {
                                reg.erase<BusyTag>(player_ent);
                            }
                        }
                    });
                }
            );
        }
        else if (cmd.type == PlayerCommand::PlayerMessage) {
            mundusvivens::PlayerMessageRequest req;
            if (!req.ParseFromArray(cmd.payload.data(), static_cast<int>(cmd.payload.size()))) continue;

            uint32_t session_idx = cmd.session_index;
            entt::entity player_ent = entt::null;
            auto it = index.by_session_index.find(cmd.session_index);
            if (it != index.by_session_index.end()) {
                player_ent = it->second;
            }

            std::string npc_name = "NPC";
            if (player_ent != entt::null && reg.all_of<PlayerDialogueComp>(player_ent)) {
                entt::entity npc_ent = reg.get<PlayerDialogueComp>(player_ent).npc_entity;
                if (reg.valid(npc_ent) && reg.all_of<IdentityComp>(npc_ent)) {
                    npc_name = reg.get<IdentityComp>(npc_ent).display_name;
                }
            }

            auto session = tcp.GetSession(session_idx);
            if (session) session->IncrementPendingGrpc();

            async_client.SendPlayerMessageAsync(req.session_id(), req.message(),
                [&grpc_queue, tcp_addr = &tcp, session_idx, npc_name, req, session](bool success, const std::string& reply_text) {
                    if (session) session->DecrementPendingGrpc();
                    grpc_queue.Push([success, reply_text, tcp_addr, session_idx, npc_name, req](entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
                        if (success) {
                            mundusvivens::NpcReplyPayload reply;
                            reply.set_session_id(req.session_id());
                            reply.set_npc_name(npc_name);
                            reply.set_reply_text(reply_text);

                            SendProto(*tcp_addr, session_idx, PacketId::SC_NPC_REPLY, reply);
                        } else {
                            std::cerr << "❌ [플레이어 메시지 전송 실패] 대화 응답 전송 실패" << std::endl;
                        }
                    });
                }
            );
        }
        else if (cmd.type == PlayerCommand::EndDialogue) {
            mundusvivens::EndDialogueRequest req;
            if (!req.ParseFromArray(cmd.payload.data(), static_cast<int>(cmd.payload.size()))) continue;

            uint32_t session_idx = cmd.session_index;
            entt::entity player_ent = entt::null;
            auto it = index.by_session_index.find(cmd.session_index);
            if (it != index.by_session_index.end()) {
                player_ent = it->second;
            }

            entt::entity npc_ent = entt::null;
            if (player_ent != entt::null && reg.all_of<PlayerDialogueComp>(player_ent)) {
                npc_ent = reg.get<PlayerDialogueComp>(player_ent).npc_entity;
            }

            auto session = tcp.GetSession(session_idx);
            if (session) session->IncrementPendingGrpc();

            async_client.EndPlayerDialogueAsync(req.session_id(),
                [&grpc_queue, npc_ent, player_ent, session](bool success, const std::string& summary) {
                    if (session) session->DecrementPendingGrpc();
                    grpc_queue.Push([success, summary, npc_ent, player_ent](entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
                        std::cout << "💬 [플레이어 대화 종료 수신] 요약: " << summary << std::endl;
                        if (reg.valid(npc_ent)) {
                            reg.get<ActivityComp>(npc_ent).current_activity = "대기";
                            if (reg.all_of<BusyTag>(npc_ent)) reg.erase<BusyTag>(npc_ent);
                        }
                        if (reg.valid(player_ent)) {
                            if (reg.all_of<BusyTag>(player_ent)) reg.erase<BusyTag>(player_ent);
                            if (reg.all_of<PlayerDialogueComp>(player_ent)) reg.erase<PlayerDialogueComp>(player_ent);
                        }
                    });
                }
            );
        }
        else if (cmd.type == PlayerCommand::GetAgentStatus) {
            mundusvivens::GetAgentStatusRequest req;
            if (!req.ParseFromArray(cmd.payload.data(), static_cast<int>(cmd.payload.size()))) continue;

            uint32_t agent_id = req.agent_id();
            uint32_t session_idx = cmd.session_index;

            auto session = tcp.GetSession(session_idx);
            if (session) session->IncrementPendingGrpc();

            async_client.GetAgentStatusAsync(agent_id,
                [&grpc_queue, session_idx, agent_id, session](bool success, const MundusVivens::AgentStatus& status) {
                    if (session) session->DecrementPendingGrpc();
                    grpc_queue.Push([success, status, session_idx, agent_id](entt::registry& reg, TcpServer& tcp, MundusVivens::AsyncGrpcClient& async_client) {
                        mundusvivens::GetAgentStatusResponse resp;
                        if (success) {
                            resp.set_name(status.name);
                            auto* loc = resp.mutable_location();
                            loc->set_name(status.location);
                            auto* pos = loc->mutable_position();
                            pos->set_x(status.x);
                            pos->set_y(status.y);
                            pos->set_z(status.z);
                            resp.set_emotion(status.emotion);
                            resp.set_activity(status.activity);

                            // C#에서 온 기억 목록 복사
                            for (const auto& mem : status.memories) {
                                resp.add_memories(mem);
                            }

                            // C++ 내의 관계 정보 가져와서 기억 목록 끝에 덧붙여 전송
                            if (reg.ctx().contains<EntityIndex>()) {
                                auto& index = reg.ctx().get<EntityIndex>();
                                auto it = index.by_npc_id.find(agent_id);
                                if (it != index.by_npc_id.end()) {
                                    entt::entity npc_ent = it->second;
                                    if (reg.valid(npc_ent) && reg.all_of<RelationshipCacheComp>(npc_ent)) {
                                        const auto& rel_cache = reg.get<RelationshipCacheComp>(npc_ent);
                                        for (const auto& pair : rel_cache.relationships) {
                                            uint32_t target_id = pair.first;
                                            const auto& rel = pair.second;

                                            std::string target_name = "알 수 없음";
                                            auto target_it = index.by_npc_id.find(target_id);
                                            if (target_it != index.by_npc_id.end()) {
                                                entt::entity target_ent = target_it->second;
                                                if (reg.valid(target_ent) && reg.all_of<IdentityComp>(target_ent)) {
                                                    target_name = reg.get<IdentityComp>(target_ent).display_name;
                                                }
                                            }
                                            resp.add_memories("[관계] " + target_name + ": 호감도 " + std::to_string(rel.liking) + " / 신뢰도 " + std::to_string(rel.trust));
                                        }
                                    }
                                }
                            }
                        } else {
                            resp.set_name("조회 실패");
                        }

                        std::string serialized;
                        if (resp.SerializeToString(&serialized)) {
                            tcp.SendTo(session_idx, PacketId::SC_GET_AGENT_STATUS_ACK,
                                reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size());
                        }
                    });
                }
            );
        }
    }
}
