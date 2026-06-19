#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <csignal>
#include <atomic>
#include "MundusVivensClient.h"

// 종료 시그널 제어 플래그 및 시그널 핸들러 정의
std::atomic<bool> keep_running(true);
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        keep_running = false;
    }
}

// 이동 가능한 가상 월드 내 장소 리스트 및 NPC ID 목록 (동적 초기화됨)
std::vector<std::string> Locations;
std::vector<std::string> NpcIds;

// NPC ID에 매핑되는 한글 이름 딕셔너리
std::map<std::string, std::string> NpcNames;

// NPC별 실시간 현재 위치 추적 테이블
std::map<std::string, std::string> CurrentLocations;

// NPC별 실시간 현재 행동 상태 추적 테이블
std::map<std::string, std::string> CurrentActivities;

// NPC별 실시간 현재 감정 상태 추적 테이블
std::map<std::string, std::string> CurrentEmotions;

// 대화 발생 방지를 위한 쿨다운 관리 테이블
// (Key: 알파벳 순으로 정렬된 두 NPC ID를 ":"로 병합한 문자열, Value: 다시 대화가 가능해지는 시점의 틱 넘버)
std::map<std::string, int> DialogueCooldowns;

// 🆕 NPC별 일일 스케줄 저장 테이블
std::vector<MundusVivens::DailySchedule> DailySchedules;

// 🆕 NPC별 일일 대화 횟수 추적 테이블
std::map<std::string, int> DailyDialogueCounts;

//  비동기 진행 중인 대화 트래킹을 위한 구조체
struct PendingDialogue {
    std::string task_id;
    std::string npc_a_id;
    std::string npc_b_id;
    int triggered_tick;
    std::string meeting_location; // 대화 시작 장소 스냅샷
};

//  대화 중인 NPC 식별 헬퍼 함수
bool IsNpcBusy(const std::string& npc_id, const std::vector<PendingDialogue>& pendings) {
    for (const auto& pd : pendings) {
        if (pd.npc_a_id == npc_id || pd.npc_b_id == npc_id) {
            return true;
        }
    }
    return false;
}

// 정렬된 복합 키를 생성해 주는 헬퍼 함수 (중복 등록 방지용)
std::string GetCooldownKey(const std::string& a, const std::string& b) {
    std::string first = a;
    std::string second = b;
    if (first > second) {
        std::swap(first, second);
    }
    return first + ":" + second;
}

// 🆕 NPC가 현재 진행 중인 활동(Activity)에 집중하여 대화를 피할 확률을 계산하는 헬퍼 함수
bool IsNPCFocusedOnActivity(const std::string& npc_id, const std::string& activity, double roll) {
    if (activity.find("취침") != std::string::npos || activity.find("휴식") != std::string::npos) {
        // 취침/휴식 중에는 대화 불가
        return true;
    }
    if (activity.find("기도") != std::string::npos || activity.find("명상") != std::string::npos) {
        // 기도/명상 중에는 80% 확률로 대화 불가
        return roll < 0.8;
    }
    // 그 외 활동 시에는 30% 확률로 몰입하여 대화 거부
    return roll < 0.3;
}

int main() {
#ifdef _WIN32
    // Windows 환경의 콘솔창에서 한글 깨짐을 방지하기 위한 UTF-8(코드페이지 65001) 설정
    system("chcp 65001 > nul");
#endif

    // 종료 시그널 등록
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "=======================================================" << std::endl;
    std::cout << "🎮 Mundus Vivens — C++ 게임 서버 시뮬레이터 콘솔" << std::endl;
    std::cout << "=======================================================\n" << std::endl;

    // C# AI gRPC 서버 엔드포인트 주소 설정
    const std::string server_address = "localhost:5001";
    std::cout << "[C++ 서버] " << server_address << " 포트의 C# AI gRPC 서버로 연결 시도 중..." << std::endl;

    // gRPC 클라이언트 채널 및 스텁 초기화
    MundusVivens::MundusVivensClient client(server_address);
    std::cout << "[C++ 서버] gRPC 통신 채널이 성공적으로 초기화되었습니다." << std::endl;

    // C# 서버로부터 부트스트랩 데이터 동적 로드
    std::cout << "[C++ 서버] C# 서버로부터 월드 부트스트랩 데이터를 요청하는 중..." << std::endl;
    auto bootstrap = client.GetWorldBootstrap();

    if (bootstrap.locations.empty() || bootstrap.agents.empty()) {
        std::cerr << "❌ [부트스트랩 실패] 월드 초기화 데이터를 가져오지 못했습니다. 서버 상태를 확인해 주세요." << std::endl;
        return 1;
    }

    Locations = bootstrap.locations;
    for (const auto& agent : bootstrap.agents) {
        // "player" 에이전트는 C++ 시뮬레이션 및 자율 이동 대상에서 제외
        if (agent.agent_id == "player") {
            continue;
        }
        NpcIds.push_back(agent.agent_id);
        NpcNames[agent.agent_id] = agent.name;
        CurrentLocations[agent.agent_id] = agent.location;
        CurrentActivities[agent.agent_id] = agent.activity;
        CurrentEmotions[agent.agent_id] = agent.emotion;
    }

    std::cout << "[C++ 서버] 월드 부트스트랩 완료: 위치 " << Locations.size()
              << "곳, NPC " << NpcIds.size() << "명 연동 완료." << std::endl;
    std::cout << "[C++ 서버] 메인 시뮬레이션 루프 가동 (5초 주기). 종료하려면 Ctrl+C를 누르세요.\n" << std::endl;

    // 난수 생성기 및 확률 분포 세팅 (Mersenne Twister Engine)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.0, 1.0);       // 0.0 ~ 1.0 사이의 확률 주사위
    std::uniform_int_distribution<> loc_dis(0, Locations.size() - 1); // 장소 배열 인덱스 선택용 분포

    int tick = 0; // 현재까지 성공적으로 동기화 완료된 마지막 틱 번호
    std::vector<PendingDialogue> pendingDialogues;

    // =================================================================
    //  가상 월드 메인 시뮬레이션 무한 루프 시작
    // =================================================================
    while (keep_running) {
        int target_tick = tick + 1; // 이번에 시도할 틱 번호
        std::cout << "\n================== [ C++ 월드 루프 틱 " << target_tick << " ] ==================" << std::endl;

        // -------------------------------------------------------------
        // 1. 글로벌 시간(Tick) 동기화 통지
        // -------------------------------------------------------------
        std::string out_msg;
        bool success = client.ProcessWorldTick(target_tick, out_msg);
        if (success) {
            tick = target_tick; // 성공 시에만 틱 번호 확정
            std::cout << "⏱️ [틱 동기화] 틱 번호 " << tick << "가 C# 서버에 동기화되었습니다. 메시지: " << out_msg << std::endl;
        }
        else {
            std::cerr << "❌ [틱 동기화 에러] 틱 " << target_tick << " 동기화 실패. 동일 번호로 재시도합니다: " << out_msg << std::endl;
            std::cout << "[C++ 서버] 5초 후 재시도합니다..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue; // 통신 장애 시 이번 틱의 물리 이동/대화 연산은 건너뛰고 재시도
        }

        // 🆕 매일 아침(0시/0틱) 감지: 대화 제한 횟수 초기화 및 일일 스케줄 데이터 조회
        if (DailySchedules.empty() || target_tick % 24 == 0) {
            DailyDialogueCounts.clear();
            std::cout << "☀️ [새로운 하루 시작] NPC들의 일일 대화 제한 횟수 초기화 및 일일 스케줄 데이터 조회 중..." << std::endl;
            auto schedules = client.GetDailySchedules(target_tick);
            if (!schedules.empty()) {
                DailySchedules = schedules;
                std::cout << "[C++ 서버] 일일 스케줄 갱신 완료 (대상 NPC 수: " << DailySchedules.size() << ")" << std::endl;
            } else {
                std::cerr << "⚠️ [스케줄 에러] 일일 스케줄을 가져오지 못했습니다. 기존 스케줄 혹은 기본값을 유지합니다." << std::endl;
            }
        }

        // -------------------------------------------------------------
        // 2. 각 NPC들의 스케줄 기반 이동 시뮬레이션 및 역동기화
        // -------------------------------------------------------------
        for (const auto& npc_id : NpcIds) {
            // [이슈 3] 대화 중인 NPC는 이동 대상에서 원천 배제
            if (IsNpcBusy(npc_id, pendingDialogues)) {
                std::cout << "💬 [이동 제한] " << NpcNames[npc_id] << "은(는) 대화 중이므로 이동할 수 없습니다. 위치 유지: [" << CurrentLocations[npc_id] << "]" << std::endl;
                std::string status_msg;
                client.UpdateAgentStatus(npc_id, CurrentLocations[npc_id], CurrentEmotions[npc_id], CurrentActivities[npc_id], status_msg);
                continue;
            }

            // 스케줄에서 현재 시간의 목적지와 행동 조회
            std::string target_location = CurrentLocations[npc_id];
            std::string scheduled_activity = CurrentActivities[npc_id];
            bool found_schedule = false;

            int current_hour = target_tick % 24;
            for (const auto& ds : DailySchedules) {
                if (ds.agent_id == npc_id) {
                    for (const auto& item : ds.items) {
                        if (item.start_hour <= current_hour && current_hour <= item.end_hour) {
                            target_location = item.target_location;
                            scheduled_activity = item.activity;
                            found_schedule = true;
                            break;
                        }
                    }
                    break;
                }
            }

            std::string old_loc = CurrentLocations[npc_id];
            if (found_schedule) {
                CurrentLocations[npc_id] = target_location;
                CurrentActivities[npc_id] = scheduled_activity;

                if (target_location != old_loc) {
                    std::cout << "🏃 [스케줄 이동] " << NpcNames[npc_id] << " 이동함: [" << old_loc << "] ➔ [" << target_location << "] (행동: " << scheduled_activity << ")" << std::endl;
                } else {
                    std::cout << "🧍 [스케줄 유지] " << NpcNames[npc_id] << " 위치 유지: [" << target_location << "] (행동: " << scheduled_activity << ")" << std::endl;
                }
            } else {
                // 스케줄이 없을 경우 기존의 기본 잔류 처리
                std::cout << "🧍 [위치 잔류] " << NpcNames[npc_id] << " 위치 유지: [" << CurrentLocations[npc_id] << "] (현재 행동: " << CurrentActivities[npc_id] << ")" << std::endl;
            }

            // C++ 월드에서 가공된 NPC의 최신 물리 상태를 C# AI 인메모리 객체로 역동기화 (패킷 송신)
            std::string status_msg;
            client.UpdateAgentStatus(npc_id, CurrentLocations[npc_id], CurrentEmotions[npc_id], CurrentActivities[npc_id], status_msg);
        }

        // -------------------------------------------------------------
        // 3. [Phase 2] 비동기 대화 결과 수거 (Polling)
        // -------------------------------------------------------------
        for (auto it = pendingDialogues.begin(); it != pendingDialogues.end(); ) {
            // [기아 방지] 10틱(50초) 이상 완료 응답이 없으면 타임아웃 강제 해제
            if (tick - it->triggered_tick > 10) {
                std::cerr << "⚠️ [대화 타임아웃] Job " << it->task_id
                          << " (" << NpcNames[it->npc_a_id] << " <-> " << NpcNames[it->npc_b_id]
                          << ", 장소: " << it->meeting_location
                          << ") 응답 없음 — 강제 해제합니다." << std::endl;

                CurrentActivities[it->npc_a_id] = "대기";
                CurrentActivities[it->npc_b_id] = "대기";

                std::string status_msg;
                client.UpdateAgentStatus(it->npc_a_id, CurrentLocations[it->npc_a_id],
                    CurrentEmotions[it->npc_a_id], "대기", status_msg);
                client.UpdateAgentStatus(it->npc_b_id, CurrentLocations[it->npc_b_id],
                    CurrentEmotions[it->npc_b_id], "대기", status_msg);

                std::string cooldown_key = GetCooldownKey(it->npc_a_id, it->npc_b_id);
                DialogueCooldowns[cooldown_key] = tick + 3; // 쿨다운 3틱 적용

                it = pendingDialogues.erase(it);
                continue;
            }

            auto result = client.PollDialogueResult(it->task_id);
            if (result.has_error) {
                std::cerr << "⏳ [대화 결과 수거 에러] Job " << it->task_id
                          << " 통신 에러 발생. 타임아웃 대기 (" << (tick - it->triggered_tick) << "/10 틱)..." << std::endl;
                ++it;
                continue;
            }
            if (result.is_completed) {
                std::cout << "\n🔔 [비동기 대화 완료 수신] [" << it->meeting_location << "]에서 진행된 "
                          << NpcNames[it->npc_a_id] << "와(과) " << NpcNames[it->npc_b_id] << "의 대화 완료!" << std::endl;
                
                if (result.dialogue_lines.empty()) {
                    std::cerr << "❌ [대화 데이터 에러] 대화 수거에 성공했으나 대사 텍스트가 비어있습니다. 에러 요약: " << result.dialogue_summary << std::endl;
                } else {
                    std::cout << "\n================== [ C++ AI 대화 요약 결과 리포트 ] ==================" << std::endl;
                    std::cout << result.dialogue_summary << std::endl;
                    std::cout << "==============================================================" << std::endl;

                    std::cout << "\n[실시간 소문 유통 연극 대본 로그]" << std::endl;
                    for (const auto& line : result.dialogue_lines) {
                        std::cout << line << std::endl;
                    }
                    std::cout << "==============================================================\n" << std::endl;
                }

                // 🆕 C++ 내부 감정 상태 갱신 (대화 분석 결과 반영)
                for (const auto& em_update : result.emotion_updates) {
                    if (CurrentEmotions.find(em_update.agent_id) != CurrentEmotions.end()) {
                        CurrentEmotions[em_update.agent_id] = em_update.new_emotion;
                        std::cout << "🎭 [감정 동기화] " << NpcNames[em_update.agent_id] << "의 감정이 [" << em_update.new_emotion << "](으)로 업데이트되었습니다." << std::endl;
                    }
                }

                // C++ 내부 행동 상태 갱신
                CurrentActivities[it->npc_a_id] = "대화 마침";
                CurrentActivities[it->npc_b_id] = "대화 마침";

                // 대화 마침 상태를 C# AI 서버에도 즉시 역동기화하여 대화 종료 상태 반영
                std::string status_msg;
                // [이슈 2] "평온함" 하드코딩 제거 → CurrentEmotions 맵 참조
                client.UpdateAgentStatus(it->npc_a_id, CurrentLocations[it->npc_a_id], CurrentEmotions[it->npc_a_id], CurrentActivities[it->npc_a_id], status_msg);
                client.UpdateAgentStatus(it->npc_b_id, CurrentLocations[it->npc_b_id], CurrentEmotions[it->npc_b_id], CurrentActivities[it->npc_b_id], status_msg);

                // 무한 수다 방지를 위해 해당 조에게 5틱(25초) 동안 대화 금지 쿨다운 가중치 등록
                std::string cooldown_key = GetCooldownKey(it->npc_a_id, it->npc_b_id);
                DialogueCooldowns[cooldown_key] = tick + 5;

                // pending 목록에서 제거
                it = pendingDialogues.erase(it);
            } else {
                std::cout << "⏳ [대화 진행 중] Job " << it->task_id << " (" << NpcNames[it->npc_a_id] << " <-> " << NpcNames[it->npc_b_id] << ") 연산 대기 중..." << std::endl;
                ++it;
            }
        }

        // -------------------------------------------------------------
        // 4. [Phase 3] 동일 공간 인접 검사 및 새 대화 비동기 트리거 (Fire-and-Forget)
        // -------------------------------------------------------------
        for (size_t i = 0; i < NpcIds.size(); ++i) {
            for (size_t j = i + 1; j < NpcIds.size(); ++j) {
                std::string npcA = NpcIds[i];
                std::string npcB = NpcIds[j];

                // 이미 둘 중 하나라도 대화 진행 중(Pending)이면 트리거 대상에서 제외
                if (IsNpcBusy(npcA, pendingDialogues) || IsNpcBusy(npcB, pendingDialogues)) {
                    continue;
                }

                // 관문 1: 두 NPC가 물리적으로 완전히 동일한 장소에 서 있는가?
                if (CurrentLocations[npcA] == CurrentLocations[npcB]) {
                    std::string cooldown_key = GetCooldownKey(npcA, npcB);

                    // 관문 2: 대화 재발동 쿨다운 제한 시간이 풀렸는가?
                    if (DialogueCooldowns.find(cooldown_key) != DialogueCooldowns.end() && tick < DialogueCooldowns[cooldown_key]) {
                        continue;
                    }

                    // 🆕 관문 2.5: 일일 대화 제한 횟수가 초과되었는가? (NPC당 하루 최대 3회)
                    if (DailyDialogueCounts[npcA] >= 3) {
                        std::cout << "⏳ [대화 억제] " << NpcNames[npcA] << "은(는) 오늘 이미 " << DailyDialogueCounts[npcA] << "번 대화하여 더 이상의 대화를 거절합니다." << std::endl;
                        continue;
                    }
                    if (DailyDialogueCounts[npcB] >= 3) {
                        std::cout << "⏳ [대화 억제] " << NpcNames[npcB] << "은(는) 오늘 이미 " << DailyDialogueCounts[npcB] << "번 대화하여 더 이상의 대화를 거절합니다." << std::endl;
                        continue;
                    }

                    // 🆕 관문 2.6: 현재 활동(Activity)에 집중/몰입하고 있는가?
                    double rollA = dis(gen);
                    double rollB = dis(gen);
                    if (IsNPCFocusedOnActivity(npcA, CurrentActivities[npcA], rollA)) {
                        std::cout << "⏳ [대화 억제] " << NpcNames[npcA] << "은(는) 현재 활동(\"" << CurrentActivities[npcA] << "\")에 몰입해 대화를 나누지 못합니다." << std::endl;
                        continue;
                    }
                    if (IsNPCFocusedOnActivity(npcB, CurrentActivities[npcB], rollB)) {
                        std::cout << "⏳ [대화 억제] " << NpcNames[npcB] << "은(는) 현재 활동(\"" << CurrentActivities[npcB] << "\")에 몰입해 대화를 나누지 못합니다." << std::endl;
                        continue;
                    }

                    // 관문 3: 조건을 충족했을 때 최종 50%의 대화 발동 주사위 확률이 터졌는가?
                    if (dis(gen) < 0.5) {
                        std::cout << "\n💬 [C++ 공간 충돌 감지] " << NpcNames[npcA] << "와(과) " << NpcNames[npcB] << "이(가) [" << CurrentLocations[npcA] << "] 공간에서 마주쳤습니다!" << std::endl;
                        std::cout << "💬 비동기 gRPC 통신으로 대화 트리거를 요청합니다 (Fire-and-Forget)..." << std::endl;

                        // 비동기 트리거 호출
                        auto result = client.TriggerDialogueAsync(npcA, npcB);
                        if (result.is_queued) {
                            std::cout << "🚀 대화가 대기열에 성공적으로 등록되었습니다. Job ID: " << result.task_id << std::endl;
                            // [이슈 4] 대화 시작 위치 저장
                            pendingDialogues.push_back({ result.task_id, npcA, npcB, tick, CurrentLocations[npcA] });

                            // 🆕 일일 대화 횟수 증가
                            DailyDialogueCounts[npcA]++;
                            DailyDialogueCounts[npcB]++;

                            // C++ 내부 행동 상태 갱신
                            CurrentActivities[npcA] = NpcNames[npcB] + "와(과) 대화 중";
                            CurrentActivities[npcB] = NpcNames[npcA] + "와(과) 대화 중";

                            // 대화 중 상태를 C# AI 서버에도 역동기화
                            std::string status_msg;
                            // [이슈 2] "평온함" 하드코딩 제거 → CurrentEmotions 맵 참조
                            client.UpdateAgentStatus(npcA, CurrentLocations[npcA], CurrentEmotions[npcA], CurrentActivities[npcA], status_msg);
                            client.UpdateAgentStatus(npcB, CurrentLocations[npcB], CurrentEmotions[npcB], CurrentActivities[npcB], status_msg);
                        } else {
                            std::cerr << "❌ [대화 요청 실패] 대화가 큐에 등록되지 못했습니다." << std::endl;
                        }
                    }
                }
            }
        }

        // -------------------------------------------------------------
        // 5. [이슈 A] DialogueCooldowns 맵 클리닝 (Sweep)
        // -------------------------------------------------------------
        for (auto it = DialogueCooldowns.begin(); it != DialogueCooldowns.end(); ) {
            if (tick >= it->second) {
                it = DialogueCooldowns.erase(it);
            } else {
                ++it;
            }
        }

        // 5초 동안 메인 스레드를 수면(Sleep)시켜 루프 주기 확보
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    // -------------------------------------------------------------
    // [이슈 I] Graceful Shutdown: 남아있는 pending 대화 NPC 상태 원복
    // -------------------------------------------------------------
    if (!pendingDialogues.empty()) {
        std::cout << "\n🧹 [종료 정리] 진행 중인 대화가 남아있어 NPC 상태를 원복합니다..." << std::endl;
        for (const auto& pd : pendingDialogues) {
            std::string status_msg;
            client.UpdateAgentStatus(pd.npc_a_id, CurrentLocations[pd.npc_a_id], CurrentEmotions[pd.npc_a_id], "대기", status_msg);
            client.UpdateAgentStatus(pd.npc_b_id, CurrentLocations[pd.npc_b_id], CurrentEmotions[pd.npc_b_id], "대기", status_msg);
            std::cout << "  - " << NpcNames[pd.npc_a_id] << "와(과) " << NpcNames[pd.npc_b_id] << "을(를) '대기' 상태로 복원했습니다." << std::endl;
        }
    }
    std::cout << "[C++ 서버] 시뮬레이션 서버가 안전하게 종료되었습니다." << std::endl;

    return 0;
}