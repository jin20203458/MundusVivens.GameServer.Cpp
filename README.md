# 🎮 Mundus Vivens — C++ Game Server Simulator

Project Mundus Vivens의 C++ 게임 서버 시뮬레이터입니다. gRPC 프로토콜을 통하여 C# AI 에이전트 서버와 실시간으로 연동되어 작동합니다.

---

## 🛠️ 주요 기능

- **월드 틱 동기화 (ProcessWorldTick)**: C++ 서버가 자체 시뮬레이션 루프(기본 5초 주기)를 돌며 틱을 진행하고, 이를 C# AI 서버에 실시간으로 동기화합니다.
- **에이전트 상태 업데이트 (UpdateAgentStatus)**: 매 틱마다 NPC들의 위치(Location), 현재 행동(Activity), 감정 상태(Emotion)를 동적으로 변경하여 C# AI 서버에 전송합니다.
- **공간 인접 감지 및 대화 트리거 (TriggerDialogue)**: 동일한 지역(예: 광장, 술집 등)에 조우한 NPC들을 감지하여 자동으로 C# 서버에 gRPC 요청을 보내고, LLM(Gemini)에 의해 동적으로 생성된 대화 결과를 콘솔에 출력합니다.
- **한글 깨짐 방지**: Windows 콘솔 환경(CMD/PowerShell)에서 한국어가 정상 출력되도록 UTF-8 코드페이지(`chcp 65001`) 세팅이 내장되어 있습니다.

---

## 📋 사전 요구사항 (Prerequisites)

이 프로젝트를 빌드하고 실행하기 위해서는 아래의 도구들이 PC에 설치되어 있어야 합니다.

1. **Visual Studio (2022 이상)**
   - C++를 사용한 데스크톱 개발 워크로드 필수 설치
2. **CMake (3.20 이상)**
   - Visual Studio에 내장된 CMake를 사용하거나 독립 실행형 CMake를 설치
3. **vcpkg (C++ 패키지 관리자)**
   - gRPC 및 Protobuf C++ 라이브러리 설치를 위해 필수적입니다.
   - [vcpkg 설치 공식 가이드](https://github.com/microsoft/vcpkg)

---

## ⚙️ 빌드 및 실행 방법

### 1. 의존성 라이브러리 설치 (vcpkg)
프로젝트 구동을 위해 gRPC와 Protobuf를 설치해야 합니다. CMD 또는 PowerShell에서 아래 명령어를 실행합니다.

```bash
# vcpkg를 통해 gRPC를 x64-windows 대상으로 컴파일 및 설치
vcpkg install grpc:x64-windows protobuf:x64-windows
```

### 2. 프로젝트 빌드 (CMake)

#### A. Visual Studio로 여는 경우 (권장)
1. Visual Studio를 실행한 후 **[폴더 열기]**를 선택합니다.
2. `MundusVivens.GameServer.Cpp` 폴더를 통째로 선택해 엽니다.
3. VS가 CMake 구성을 자동으로 시작합니다. 만약 vcpkg 경로를 찾지 못할 경우 상단 메뉴의 **[프로젝트] ➔ [MundusVivens.GameServer.Cpp의 CMake 설정]**에서 `CMAKE_TOOLCHAIN_FILE` 변수에 로컬 vcpkg 툴체인 파일 경로(예: `C:/Users/<사용자>/vcpkg/scripts/buildsystems/vcpkg.cmake`)를 지정해 줍니다.
4. 빌드 대상 타겟(`MundusVivensGameServer.exe`)을 선택하고 실행(F5)합니다.

#### B. CLI(명령줄)로 빌드하는 경우
```bash
# 빌드 디렉토리 생성 및 CMake 구성 (vcpkg 툴체인 경로 적용)
cmake -B out/build -S . -DCMAKE_TOOLCHAIN_FILE="C:/Users/<사용자>/vcpkg/scripts/buildsystems/vcpkg.cmake"

# 빌드 실행
cmake --build out/build --config Debug
```

---

## 📂 프로젝트 구조

- `main.cpp`: 프로그램 진입점 및 틱 루프 시뮬레이션 본체.
- `MundusVivensClient.h / .cpp`: gRPC 채널 개설 및 Stub을 활용한 RPC 요청 통신 모듈.
- `Protos/mundus_vivens.proto`: 통신 규격 스키마 정의 파일.
- `CMakeLists.txt`: 크로스플랫폼 컴파일 및 Protobuf 빌드 체인 정의 스크립트.
