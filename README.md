# Mundus Vivens (C++ Game Server)

Project Mundus Vivens의 고성능 C++ 게임 서버 시뮬레이터입니다. 독자적인 물리 틱을 처리하며 gRPC를 통해 C# AI 서버와 실시간으로 통신합니다.

## 기술 문서 (Documentation)

프로젝트 전체 시스템 아키텍처, 에이전트 인지 모델 및 향후 개발 로드맵은 `Obsidian.Agent` 저장소에 중앙 집중화되어 관리됩니다. 아래 문서들은 개발자와 코딩 에이전트 모두가 참조하는 **단일 진실의 원천(Single Source of Truth)**입니다.

- [00_project_overview.md](../Obsidian.Agent/MundusVivens/docs/00_project_overview.md): 전체 시스템 아키텍처 및 통신망
- [01_game_server_architecture.md](../Obsidian.Agent/MundusVivens/docs/01_game_server_architecture.md): C++ 물리 엔진 아키텍처 (ECS, 스레드 모델)
- [02_agent_design.md](../Obsidian.Agent/MundusVivens/docs/02_agent_design.md): C# 인지 파이프라인 (LLM, 기억, 대화)
- [03_future_roadmap.md](../Obsidian.Agent/MundusVivens/docs/03_future_roadmap.md): 향후 구현 로드맵
- [04_cpp_server_profiling.md](../Obsidian.Agent/MundusVivens/docs/04_cpp_server_profiling.md): Tracy 프로파일러 연동 및 4대 벤치마크 검증 보고서
- [05_csharp_ai_profiling.md](../Obsidian.Agent/MundusVivens/docs/05_csharp_ai_profiling.md): C# AI 대뇌 서버 아키텍처 효율성 및 비용 정량 비교 보고서

> **참고**: 전체 시스템 구성도 및 흐름도는 중복을 방지하기 위해 [00_project_overview.md](../Obsidian.Agent/MundusVivens/docs/00_project_overview.md)에서만 제공합니다.

## 주요 기능

- **3-스레드 프로액터 모델**: 데이터 레이스를 완전히 격리하기 위해 I/O, 메인 게임루프(20Hz), gRPC 통신 스레드를 엄격히 분리하고, 더블 버퍼드 Swap 동기화 큐를 활용해 메인 스레드의 락 경합 지연을 최소화합니다.
- **ECS (Entity Component System)**: EnTT 라이브러리를 활용하여 NPC의 상태, 공간 해시 그리드 기반의 최적화된 거리 연산을 순차 조밀 배열 상에서 캐시 친화적으로 수행합니다.
- **공간 기반 대화 트리거**: 물리적 거리와 기존 관계 수치를 바탕으로 대화 주도, 수락 및 제일차 합류 확률을 수학적으로 계산하여 대화 이벤트를 발생시킵니다.

## 빌드 및 실행

1. **요구 사항**: Visual Studio 2022 (C++ 데스크톱 개발 워크로드), CMake, vcpkg가 필요합니다.
2. **자동 빌드 스크립트**: 동봉된 파워쉘 스크립트를 실행하면 vswhere를 통한 경로 탐색과 CMake 프리셋 빌드가 자동으로 진행됩니다. (윈도우 권한 문제를 방지하기 위해 아래 명령어를 터미널에 실행하십시오.)
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\build_local.ps1
   ```
3. **프로파일링 모드 활성화 (선택)**: CMake 컴파일 시 `-DENABLE_PROFILING=ON`(기본값) 토글로 Tracy 프로파일러 컴파일을 켜고 끌 수 있습니다. 상세 연동 및 런북은 [04_cpp_server_profiling.md](../Obsidian.Agent/MundusVivens/docs/04_cpp_server_profiling.md)를 참조하십시오.
4. **실행**: 성공적으로 빌드된 후 `out/build/windows-default/MundusVivensGameServer.exe`를 실행합니다.

---

## 퀵스타트 & 오프라인 벤치마크 (Quickstart & Offline Benchmarks)

**C# AI 서버 구동이나 복잡한 gRPC 네트워크 연결 없이**, C++ 게임 서버의 최적화 메커니즘을 즉시 오프라인 상에서 실측하고 검증할 수 있습니다. 빌드 완료 후 빌드 출력 디렉토리에서 아래 명령어로 4개의 시나리오를 구동해 볼 수 있습니다.

```bash
# 빌드 출력 디렉터리 기준
cd out/build/windows-default

# [시나리오 1] 캐시 지역성 테스트: DOD(ECS) vs OOP
# 50,000개 엔티티 물리 연산 시 밀집 배열 캐시 친화적 성능 차이 실측
./MundusVivensGameServer.exe --benchmark 1

# [시나리오 2] 스레드 동시성 테스트: Double-Buffered Swap 큐 vs Mutex 단일 락
# 메인 스레드 락 점유 시간을 35.6배 단축시킨 스왑 큐의 효율성 검증
./MundusVivensGameServer.exe --benchmark 2

# [시나리오 3] 네트워크/I/O 격리 테스트: Asynchronous Lag Isolation
# C# AI gRPC 응답이 500ms 지연될 때 프레임 레이트 방어 성능 비교
./MundusVivensGameServer.exe --benchmark 3

# [시나리오 4] 공간 최적화 테스트: A* 대량 길찾기 스케일링
# 에이전트 규모(10, 100, 500명)에 따른 실시간 길찾기 연산 지연량 실측
./MundusVivensGameServer.exe --benchmark 4
```

---

## 전체 시스템 통합 실행 순서 (Integration Startup Sequence)

만약 C++ 물리 서버와 C# AI 서버 간의 비동기 연동 시나리오 전체를 구동해 보려면 아래 순서로 실행해야 합니다. **(C++ 서버가 부팅 시 C# 서버로부터 월드 구성 정보를 끌어오는 부트스트랩 의존성이 존재하기 때문입니다.)**

1. **[1단계] C# AI 서버 구동**: 
   `MundusVivens` 프로젝트 디렉토리에서 `dotnet run --project MundusVivens.Prototype` 명령어를 실행하여 C# gRPC 서버(`localhost:5001`)를 기동합니다.
2. **[2단계] C++ 게임 서버 구동**:
   본 리포지토리에서 빌드된 `MundusVivensGameServer.exe`를 구동합니다.
3. **[3단계] 클라이언트/API 호출**:
   기동이 완료되면 C# REST API(`localhost:5000`)를 통해 NPC 상태를 조회하거나 대화를 강제 주입해 볼 수 있습니다.

---

## 트러블슈팅 및 환경설정 (vcpkg Path)

* **vcpkg 자동 감지 실패 시**: 
  vcpkg가 기본 홈 디렉토리(`%USERPROFILE%/vcpkg`)가 아닌 다른 경로에 설치되어 있을 경우, CMake 구성 시 `-DCMAKE_TOOLCHAIN_FILE=[vcpkg설치경로]/scripts/buildsystems/vcpkg.cmake` 옵션을 명시적으로 지정해 주거나 환경 변수 `VCPKG_ROOT`를 설정하십시오.
* **Powershell 스크립트 실행 불가 시**: 
  스크립트 실행 정책 제한 시, 관리자 권한 터미널에서 `Set-ExecutionPolicy RemoteSigned` 명령을 실행하거나 README에 안내된 `Bypass` 매개변수를 사용해 주십시오.
