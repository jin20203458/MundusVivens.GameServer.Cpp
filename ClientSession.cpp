#include "ClientSession.h"
#include "TcpServer.h"
#include "mundus_vivens.pb.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <boost/container/small_vector.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include "TracyIntegration.h"

ClientSession::ClientSession(boost::asio::ip::tcp::socket socket, TcpServer& server, uint32_t index)
    : socket_(std::move(socket)), server_(server), index_(index), backpressure_timer_(socket_.get_executor()), write_channel_(socket_.get_executor(), 1024) {
}

ClientSession::~ClientSession() {
    // 채널 닫기
    write_channel_.close();

    // 큐에 남아서 전송되지 못한 대형 힙 패킷 메모리 해제 (메모리 누수 방지)
    bool has_data = true;
    while (has_data) {
        has_data = write_channel_.try_receive([&](boost::system::error_code ec, PacketBuffer buf) {
            if (buf.is_heap) {
                delete[] buf.heap_data;
            }
        });
    }

    try {
        if (socket_.is_open()) {
            socket_.close();
        }
    } catch (...) {}
    std::cout << "[ClientSession] 세션 객체 소멸 (인덱스: " << index_ << ")" << std::endl;
}

boost::asio::awaitable<void> ClientSession::Run() {
    ZoneScopedN("TCP Client Run Loop");
    auto self = shared_from_this(); // 비동기 작업 중 객체 수명 보장
    try {
        // Nagle 알고리즘 비활성화 
        socket_.set_option(boost::asio::ip::tcp::no_delay(true));

        //  쓰기 루프 코루틴을 단 한 번만 기동
        boost::asio::co_spawn(socket_.get_executor(), WriteLoop(), boost::asio::detached);

        while (true) {
            // 백프레셔 흐름 제어 활성화 시 코루틴 일시 정지
            if (is_reading_suspended_) {
                backpressure_timer_.expires_at(std::chrono::steady_clock::time_point::max());
                try {
                    co_await backpressure_timer_.async_wait(boost::asio::use_awaitable);
                } catch (const boost::system::system_error& ec) {
                    if (ec.code() != boost::asio::error::operation_aborted) {
                        throw;
                    }
                }
            }

            // 1. 헤더 4바이트 수신
            uint8_t header_buf[HEADER_SIZE];
            co_await boost::asio::async_read(
                socket_, 
                boost::asio::buffer(header_buf, HEADER_SIZE), 
                boost::asio::use_awaitable
            );

            uint16_t length = ReadU16BE(header_buf);
            uint16_t packet_id = ReadU16BE(header_buf + 2);

            if (length < HEADER_SIZE || length > MAX_PACKET_SIZE) {
                std::cerr << "[ClientSession] 잘못된 패킷 크기 수신 (크기: " << length << "). 연결을 종료합니다." << std::endl;
                break;
            }

            size_t payload_size = length - HEADER_SIZE;
            
            // boost::container::small_vector를 활용한 SBO 수신 버퍼 자동화 (256바이트 이하 힙 할당 방지)
            boost::container::small_vector<uint8_t, 256> recv_buf;
            recv_buf.resize(payload_size);
            if (payload_size > 0) {
                co_await boost::asio::async_read(
                    socket_, 
                    boost::asio::buffer(recv_buf.data(), payload_size), 
                    boost::asio::use_awaitable
                );
            }
            HandlePacket(packet_id, recv_buf.data(), payload_size);
        }
    } catch (const std::exception& e) {
        std::cout << "[ClientSession] 세션 수신 루프 예외 발생 (인덱스: " << index_ 
                  << ", 원인: " << e.what() << ")" << std::endl;
    }

    // 채널을 닫아 WriteLoop도 함께 종료되도록 함
    write_channel_.close();

    // 소켓 정리 및 서버 언레지스터
    try {
        if (socket_.is_open()) {
            socket_.close();
        }
    } catch (...) {}

    server_.UnregisterSession(index_);
}

void ClientSession::Send(uint16_t packet_id, const uint8_t* payload, size_t size) {
    ZoneScopedN("TCP Queue Send");
    uint16_t total_length = static_cast<uint16_t>(HEADER_SIZE + size);
    PacketBuffer buf;
    buf.size = total_length;

    uint8_t* target_ptr = nullptr;
    if (total_length <= 256) {
        buf.is_heap = false;
        target_ptr = buf.inline_data;
    } else {
        buf.is_heap = true;
        buf.heap_data = new uint8_t[total_length];
        target_ptr = buf.heap_data;
    }

    WriteU16BE(target_ptr, total_length);
    WriteU16BE(target_ptr + 2, packet_id);
    if (payload && size > 0) {
        std::memcpy(target_ptr + HEADER_SIZE, payload, size);
    }

    // 스레드 안전성 보장: IO 스레드(socket_ executor)로 dispatch하여 채널 동시 호출 방지
    boost::asio::post(socket_.get_executor(), [self = shared_from_this(), buf]() {
        if (!self->write_channel_.try_send(boost::system::error_code{}, buf)) {
            std::cerr << "[ClientSession] 송신 채널 포화 - 패킷 폐기 (세션: " << self->index_ << ")" << std::endl;
            if (buf.is_heap) {
                delete[] buf.heap_data; // 힙 할당된 대형 패킷 메모리 직접 수동 해제
            }
        }
    });
}

boost::asio::awaitable<void> ClientSession::WriteLoop() {
    ZoneScopedN("TCP Client Write Loop");
    auto self = shared_from_this();
    try {
        while (true) {
            //  채널로부터 비동기 수신 대기 (채널이 닫히면 operation_aborted 예외를 던지며 종료됨)
            PacketBuffer buf = co_await write_channel_.async_receive(boost::asio::use_awaitable);

            const uint8_t* data_ptr = buf.is_heap ? buf.heap_data : buf.inline_data;

            try {
                co_await boost::asio::async_write(
                    socket_,
                    boost::asio::buffer(data_ptr, buf.size),
                    boost::asio::use_awaitable
                );
            } catch (...) {
                if (buf.is_heap) {
                    delete[] buf.heap_data;
                }
                throw;
            }

            if (buf.is_heap) {
                delete[] buf.heap_data;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[ClientSession] 쓰기 루프 종료 (세션: " << index_ 
                  << ", 원인: " << e.what() << ")" << std::endl;
        
        // 남은 힙 데이터 누수 방지
        bool has_data = true;
        while (has_data) {
            has_data = write_channel_.try_receive([&](boost::system::error_code ec, PacketBuffer leak_buf) {
                if (leak_buf.is_heap) {
                    delete[] leak_buf.heap_data;
                }
            });
        }

        try {
            if (socket_.is_open()) {
                socket_.close();
            }
        } catch (...) {}
    }
}

void ClientSession::HandlePacket(uint16_t packet_id, const uint8_t* payload, size_t size) {
    ZoneScopedN("TCP Handle Packet");
    PlayerCommand cmd;
    cmd.session_index = index_;
    cmd.payload.assign(payload, payload + size); // 힙 할당 방지 (small_vector)

    switch (packet_id) {
        case PacketId::CS_LOGIN: {
            cmd.type = PlayerCommand::Login;
            server_.QueueCommand(std::move(cmd));
            break;
        }
        case PacketId::CS_PLAYER_MOVE: {
            cmd.type = PlayerCommand::Move;
            server_.QueueCommand(std::move(cmd));
            break;
        }
        case PacketId::CS_TALK_TO_NPC: {
            cmd.type = PlayerCommand::TalkToNpc;
            server_.QueueCommand(std::move(cmd));
            break;
        }
        case PacketId::CS_PLAYER_MESSAGE: {
            cmd.type = PlayerCommand::PlayerMessage;
            server_.QueueCommand(std::move(cmd));
            break;
        }
        case PacketId::CS_END_DIALOGUE: {
            cmd.type = PlayerCommand::EndDialogue;
            server_.QueueCommand(std::move(cmd));
            break;
        }
        case PacketId::CS_GET_AGENT_STATUS: {
            cmd.type = PlayerCommand::GetAgentStatus;
            server_.QueueCommand(std::move(cmd));
            break;
        }
        case PacketId::CS_HEARTBEAT: {
            Send(PacketId::SC_HEARTBEAT_ACK, nullptr, 0);
            break;
        }
        default:
            std::cout << "[ClientSession] 정의되지 않은 패킷 아이디 수신: " << packet_id << std::endl;
            break;
    }
}
