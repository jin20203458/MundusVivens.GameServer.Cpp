#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <stdexcept>

// 패킷 헤더 구조 (4바이트)
// +------------------------------------------+
// | [2 bytes] Packet Length (big-endian)      |  ← 헤더 포함 전체 패킷 크기
// | [2 bytes] Packet ID   (big-endian)       |
// | [N bytes] Payload (Protobuf serialized)  |
// +------------------------------------------+

namespace PacketId {
    // 클라이언트 → 서버 (CS)
    constexpr uint16_t CS_LOGIN            = 0x0001;  // 플레이어 로그인 요청
    constexpr uint16_t CS_PLAYER_MOVE      = 0x0002;  // 플레이어 이동 요청
    constexpr uint16_t CS_TALK_TO_NPC      = 0x0003;  // NPC 대화 요청
    constexpr uint16_t CS_PLAYER_MESSAGE   = 0x0004;  // 플레이어 대화 메시지 전송
    constexpr uint16_t CS_END_DIALOGUE     = 0x0005;  // 대화 종료 요청
    constexpr uint16_t CS_HEARTBEAT        = 0x00FF;  // 클라이언트 하트비트

    // 서버 → 클라이언트 (SC)
    constexpr uint16_t SC_LOGIN_ACK        = 0x1001;  // 로그인 응답 + 초기 맵 정보
    constexpr uint16_t SC_WORLD_SNAPSHOT   = 0x1002;  // 월드 NPC 상태 브로드캐스트
    constexpr uint16_t SC_NPC_UPDATE       = 0x1003;  // 특정 NPC 상태 변경 알림
    constexpr uint16_t SC_DIALOGUE_EVENT   = 0x1004;  // NPC간 대화 이벤트 알림
    constexpr uint16_t SC_NPC_REPLY        = 0x1005;  // 플레이어에게 NPC 대사 전달
    constexpr uint16_t SC_HEARTBEAT_ACK    = 0x10FF;  // 하트비트 응답
}

struct PacketHeader {
    uint16_t length;     // 헤더를 포함한 전체 패킷의 바이트 수
    uint16_t packet_id;  // 패킷 아이디
};

constexpr size_t HEADER_SIZE = 4;
constexpr size_t MAX_PACKET_SIZE = 65535;

// 빅엔디안 16비트 정수 읽기 (네트워크 바이트 오더 -> 호스트 바이트 오더)
inline uint16_t ReadU16BE(const uint8_t* data) {
    return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

// 빅엔디안 16비트 정수 쓰기 (호스트 바이트 오더 -> 네트워크 바이트 오더)
inline void WriteU16BE(uint8_t* dest, uint16_t val) {
    dest[0] = static_cast<uint8_t>(val >> 8);
    dest[1] = static_cast<uint8_t>(val & 0xFF);
}

// 락프리 큐 적재용 Trivially Copyable POD 패킷 버퍼 구조체 (하이브리드 SBO)
struct PacketBuffer {
    uint32_t size = 0;
    bool is_heap = false;
    union {
        uint8_t inline_data[256]; // 소형 패킷용 인라인 배열 (95% 패킷)
        uint8_t* heap_data;       // 대형 패킷용 힙 주소 포인터
    };
};
