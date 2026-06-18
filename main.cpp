#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include "MundusVivensClient.h"

// 이동 가능한 가상 월드 내 장소 리스트 및 NPC ID 목록
const std::vector<std::string> Locations = { "성당 (Church)", "술집 (Tavern)", "광장 (Square)" };
const std::vector<std::string> NpcIds = { "npc_kyle", "npc_eva", "npc_bart" };

// NPC ID에 매핑되는 한글 이름 딕셔너리
std::map<std::string, std::string> NpcNames = {
    { "npc_kyle", "카일" },
    { "npc_eva", "에바" },
    { "npc_bart", "바르트" }
};

// NPC별 실시간 현재 위치 추적 테이블
std::map<std::string, std::string> CurrentLocations = {
    { "npc_kyle", "성당 (Church)" },
    { "npc_eva", "술집 (Tavern)" },
    { "npc_bart", "술집 (Tavern)" }
};

// NPC별 실시간 현재 행동 상태 추적 테이블
std::map<std::string, std::string> CurrentActivities = {
    { "npc_kyle", "기도 중" },
    { "npc_eva", "맥주 컵 청소" },
    { "npc_bart", "술 마시기" }
};

// 대화 발생 방지를 위한 쿨다운 관리 테이블
// (Key: 알파벳 순으로 정렬된 두 NPC ID를 ":"로 병합한 문자열, Value: 다시 대화가 가능해지는 시점의 틱 넘버)
std::map<std::string, int> DialogueCooldowns;

// 정렬된 복합 키를 생성해 주는 헬퍼 함수 (중복 등록 방지용)
std::string GetCooldownKey(const std::string& a, const std::string& b) {
    std::string first = a;
    std::string second = b;
    if (first > second) {
        std::swap(first, second);
    }
    return first + ":" + second;
}

int main() {
#ifdef _WIN32
    // Windows 환경의 콘솔창에서 한글 깨짐을 방지하기 위한 UTF-8(코드페이지 65001) 설정
    system("chcp 65001 > nul");
#endif

    std::cout << "=======================================================" << std::endl;
    std::cout << "🎮 Mundus Vivens — C++ 게임 서버 시뮬레이터 콘솔" << std::endl;
    std::cout << "=======================================================\n" << std::endl;

    // C# AI gRPC 서버 엔드포인트 주소 설정
    const std::string server_address = "localhost:5001";
    std::cout << "[C++ 서버] " << server_address << " 포트의 C# AI gRPC 서버로 연결 시도 중..." << std::endl;

    // gRPC 클라이언트 채널 및 스텁 초기화
    MundusVivens::MundusVivensClient client(server_address);
    std::cout << "[C++ 서버] gRPC 통신 채널이 성공적으로 초기화되었습니다." << std::endl;
    std::cout << "[C++ 서버] 메인 시뮬레이션 루프 가동 (5초 주기). 종료하려면 Ctrl+C를 누르세요.\n" << std::endl;

    // 난수 생성기 및 확률 분포 세팅 (Mersenne Twister Engine)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dis(0.0, 1.0);       // 0.0 ~ 1.0 사이의 확률 주사위
    std::uniform_int_distribution<> loc_dis(0, Locations.size() - 1); // 장소 배열 인덱스 선택용 분포

    int tick = 0;

    // =================================================================
    //  가상 월드 메인 시뮬레이션 무한 루프 시작
    // =================================================================
    while (true) {
        tick++;
        std::cout << "\n================== [ C++ 월드 루프 틱 " << tick << " ] ==================" << std::endl;

        // -------------------------------------------------------------
        // 1. 글로벌 시간(Tick) 동기화 통지
        // -------------------------------------------------------------
        std::string out_msg;
        bool success = client.ProcessWorldTick(tick, out_msg);
        if (success) {
            std::cout << "⏱️ [틱 동기화] 틱 번호 " << tick << "가 C# 서버에 동기화되었습니다. 메시지: " << out_msg << std::endl;
        }
        else {
            std::cerr << "❌ [틱 동기화 에러] C# AI 서버와의 연결 또는 틱 동기화 실패: " << out_msg << std::endl;
            std::cout << "[C++ 서버] 5초 후 재시도합니다..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue; // 통신 장애 시 이번 틱의 물리 이동/대화 연산은 건너뛰고 재시도
        }

        // -------------------------------------------------------------
        // 2. 각 NPC들의 무작위 물리 이동 시뮬레이션 및 역동기화
        // -------------------------------------------------------------
        for (const auto& npc_id : NpcIds) {
            // 30%의 확률로 다른 장소로 이동 결정
            if (dis(gen) < 0.3) {
                std::string old_loc = CurrentLocations[npc_id];
                std::string new_loc = Locations[loc_dis(gen)]; // 무작위 새 장소 추출
                CurrentLocations[npc_id] = new_loc;

                // 새로 도착한 장소의 특성에 맞춰 기본 행동(Activity) 문자열 가공
                if (new_loc.find("Church") != std::string::npos) {
                    CurrentActivities[npc_id] = "예배 참여 및 침묵 명상";
                }
                else if (new_loc.find("Tavern") != std::string::npos) {
                    CurrentActivities[npc_id] = "술집 테이블 정리 및 잡담";
                }
                else {
                    CurrentActivities[npc_id] = "광장 게시판 구경";
                }

                std::cout << "🏃 [공간 이동] " << NpcNames[npc_id] << " 이동함: [" << old_loc << "] ➔ [" << new_loc << "] (현재 행동: " << CurrentActivities[npc_id] << ")" << std::endl;
            }
            else {
                // 70% 확률로 기존 위치에 잔류
                std::cout << "🧍 [위치 잔류] " << NpcNames[npc_id] << " 위치 유지: [" << CurrentLocations[npc_id] << "] (현재 행동: " << CurrentActivities[npc_id] << ")" << std::endl;
            }

            // C++ 월드에서 가공된 NPC의 최신 물리 상태를 C# AI 인메모리 객체로 역동기화 (패킷 송신)
            std::string status_msg;
            client.UpdateAgentStatus(npc_id, CurrentLocations[npc_id], "평온함", CurrentActivities[npc_id], status_msg);
        }

        // -------------------------------------------------------------
        // 3. 동일 공간 인접 검사(Overlap Detection) 및 대화 트리거 연산
        // -------------------------------------------------------------
        // 2중 루프 조합 연산을 통해 3명의 NPC 중 겹치지 않는 2인 조 조합 생성
        for (size_t i = 0; i < NpcIds.size(); ++i) {
            for (size_t j = i + 1; j < NpcIds.size(); ++j) {
                std::string npcA = NpcIds[i];
                std::string npcB = NpcIds[j];

                // 관문 1: 두 NPC가 물리적으로 완전히 동일한 장소에 서 있는가?
                if (CurrentLocations[npcA] == CurrentLocations[npcB]) {
                    std::string cooldown_key = GetCooldownKey(npcA, npcB);

                    // 관문 2: 대화 재발동 쿨다운 제한 시간이 풀렸는가?
                    if (DialogueCooldowns.find(cooldown_key) != DialogueCooldowns.end() && tick < DialogueCooldowns[cooldown_key]) {
                        continue; // 아직 쿨다운 페널티가 남아있으므로 무시
                    }

                    // 관문 3: 조건을 충족했을 때 최종 50%의 대화 발동 주사위 확률이 터졌는가?
                    if (dis(gen) < 0.5) {
                        std::cout << "\n💬 [C++ 공간 충돌 감지] " << NpcNames[npcA] << "와(과) " << NpcNames[npcB] << "이(가) [" << CurrentLocations[npcA] << "] 공간에서 마주쳤습니다!" << std::endl;
                        std::cout << "💬 gRPC 통신으로 C# AI 서버의 대화 시뮬레이션 엔진을 강제 구동합니다..." << std::endl;

                        //  [동기식 블락킹 호출] C# 서버가 LLM(Gemini) 연산을 다 마칠 때까지 스레드 일시 대기
                        auto result = client.TriggerDialogue(npcA, npcB, true);

                        if (result.dialogue_lines.empty()) {
                            std::cerr << "❌ [대화 데이터 에러] 대화 트리거는 성공했으나 반환된 대사 텍스트가 비어있습니다." << std::endl;
                        }
                        else {
                            // 통신 성공 및 AI 대사 수신 정산 결과 출력
                            std::cout << "\n================== [ C++ AI 대화 요약 결과 리포트 ] ==================" << std::endl;
                            std::cout << result.dialogue_summary << std::endl;
                            std::cout << "==============================================================" << std::endl;

                            std::cout << "\n[실시간 소문 유통 연극 대본 로그]" << std::endl;
                            for (const auto& line : result.dialogue_lines) {
                                std::cout << line << std::endl;
                            }
                            std::cout << "==============================================================\n" << std::endl;

                            // 대화를 마쳤으므로 물리 행동 상태 문자열 갱신
                            CurrentActivities[npcA] = "대화 마침";
                            CurrentActivities[npcB] = "대화 마침";

                            // 무한 수다 방지를 위해 해당 조에게 5틱(25초) 동안 대화 금지 쿨다운 가중치 등록
                            DialogueCooldowns[cooldown_key] = tick + 5;
                        }
                    }
                }
            }
        }

        // 5초 동안 메인 스레드를 수면(Sleep)시켜 루프 주기 확보
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    return 0;
}