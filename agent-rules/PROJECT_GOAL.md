# Qt + FreeRDP 원격 데스크톱 애플리케이션 개발 사양서

**문서 버전:** 0.1
**프로젝트명:** QtRdp
**대상 플랫폼:** Windows / macOS
**UI Framework:** Qt Widgets
**RDP Engine:** FreeRDP
**Build System:** QMake

---

# 1. 개요

## 1.1 목적

본 프로젝트는 **FreeRDP 라이브러리**를 기반으로 Windows 및 macOS 환경에서 동작하는 원격 데스크톱 애플리케이션을 개발하는 것을 목적으로 한다.

애플리케이션은 **RDP Client**와 **RDP Server**를 별도의 애플리케이션으로 분리하여 개발한다.

| 구분         | 역할                                                                |
| ---------- | ----------------------------------------------------------------- |
| RDP Client | 원격 RDP Server에 접속하여 Desktop 화면을 표시하고 Keyboard/Mouse 등의 사용자 입력을 전달 |
| RDP Server | 로컬 Desktop 화면을 RDP 프로토콜을 통해 Client에 제공하고 원격 입력을 처리                |

RDP Client는 **Qt Widgets 기반 GUI 애플리케이션**으로 개발한다.

RDP Server 역시 **Qt Widgets 기반 애플리케이션**으로 개발하되, 일반적인 사용 환경에서는 UI 노출을 최소화하고 **백그라운드에서 상시 동작할 수 있는 구조**로 구현한다.

초기 개발 단계에서는 Windows 환경을 우선 지원하여 FreeRDP Client/Server 기능과 Windows 기본 RDP 구현 간의 호환성을 검증한 후, 동일한 애플리케이션 구조를 기반으로 macOS 지원을 추가한다.

---

# 2. 프로젝트 구성

프로젝트는 다음과 같이 구성한다.

```text
QtRdp/
│
├── FreeRDP/          # FreeRDP Git Submodule
│
├── RDPClient/        # Qt 기반 RDP Client
│
├── RDPServer/        # Qt 기반 RDP Server
│
└── Docs/             # 설계서 및 개발 문서
```

## 2.1 FreeRDP

FreeRDP 공식 소스 코드를 Git Submodule 형태로 관리한다.

RDP Client 및 Server는 FreeRDP를 직접 수정하기보다는 FreeRDP에서 제공하는 라이브러리와 API를 이용하여 구현하는 것을 원칙으로 한다.

필요한 경우 FreeRDP 수정 사항은 최소화하고, 프로젝트 자체 코드와 명확히 분리하여 관리한다.

## 2.2 RDPClient

원격 RDP Server에 접속하기 위한 Qt Widgets 기반 GUI 애플리케이션이다.

FreeRDP Client API를 Wrapping하여 Qt UI와 RDP 통신 계층을 분리한다.

## 2.3 RDPServer

로컬 Desktop을 외부 RDP Client에 제공하기 위한 애플리케이션이다.

FreeRDP Server API를 이용하여 Client 연결, Desktop 화면 전송 및 원격 입력 처리를 담당한다.

일반 실행 시에는 백그라운드 동작을 기본으로 하며, 설정 및 상태 확인을 위한 최소한의 GUI를 제공할 수 있도록 설계한다.

## 2.4 Docs

다음과 같은 프로젝트 문서를 관리한다.

```text
Docs/
├── DEVELOPMENT_SPEC.md
├── ARCHITECTURE.md
├── CLIENT_DESIGN.md
├── SERVER_DESIGN.md
└── TEST_SPEC.md
```

---

# 3. 개발 목표

## 3.1 RDP Client

RDP Client는 다음 기능을 제공한다.

### 연결 관리

* RDP Server 주소 및 Port 입력
* RDP Server 연결
* RDP 연결 종료
* 연결 상태 표시
* 연결 실패 및 오류 메시지 표시

### 화면 출력

* 원격 Desktop 화면 출력
* 원격 Desktop 해상도 처리
* Client Window 크기에 따른 화면 크기 조정
* 화면 갱신 영역 처리

### 사용자 입력

* Keyboard 입력 전달
* Mouse 이동 전달
* Mouse Button 입력 전달
* Mouse Wheel 입력 전달

### Clipboard

* Client와 Server 간 Clipboard 연동
* 초기 버전에서는 Text Clipboard를 우선 지원

### 확장성

향후 다음 기능을 추가할 수 있는 구조로 개발한다.

* Full Screen
* Multi Monitor
* Audio
* File Transfer
* 자동 재접속
* 연결 Profile 관리

---

# 4. RDP Server 개발 목표

RDP Server는 다음 기능을 제공한다.

## 4.1 Server 실행

* 시스템 시작 시 자동 실행 가능한 구조
* 백그라운드 실행
* 필요 시 GUI를 통한 상태 및 설정 확인

## 4.2 Client 연결 관리

* RDP Client 연결 대기
* Client 연결 및 종료 처리
* 연결 상태 관리
* 복수 Client Session을 지원할 수 있는 구조

초기 버전에서는 단일 Client 연결을 우선 지원할 수 있으나, 내부 구조는 복수 Session으로 확장 가능하도록 설계한다.

## 4.3 Desktop Capture

* 현재 Desktop 화면 Capture
* Desktop 해상도 정보 제공
* Frame 변경 영역 감지
* 변경된 화면 영역을 RDP Client에 전달

가능한 경우 전체 화면을 반복 전송하지 않고 변경된 영역만 전달할 수 있도록 한다.

## 4.4 원격 입력 처리

다음 Client 입력을 로컬 시스템에 전달한다.

* Keyboard
* Mouse Move
* Mouse Button
* Mouse Wheel

## 4.5 Clipboard

다음 방향의 Clipboard 동기화를 지원한다.

```text
Client → Server
Server → Client
```

초기 버전에서는 Text Clipboard를 우선 지원한다.

## 4.6 보안

다음 기능을 지원하거나 향후 확장할 수 있도록 설계한다.

* TLS 암호화
* Server Certificate
* 사용자 인증
* Client 인증
* 연결 제한 정책

## 4.7 로그

Server 동작 및 문제 분석을 위해 다음 로그를 제공한다.

* Server 시작/종료
* Client 연결/종료
* RDP Session 상태
* 인증 결과
* Desktop Capture 오류
* RDP Protocol 오류
* 네트워크 오류

## 4.8 설정 관리

Server 설정을 별도의 설정 파일로 관리할 수 있도록 한다.

주요 설정 항목은 다음과 같다.

* Listen Address
* RDP Port
* 인증 설정
* Certificate
* Capture 설정
* 최대 Client 수
* Log Level

---

# 5. 개발 기본 원칙

## 5.1 Client / Server 분리

Client와 Server는 독립적인 실행 파일로 개발한다.

```text
RDPClient.exe
RDPServer.exe
```

macOS에서는 각각 별도의 Application Bundle 또는 실행 프로그램 형태로 구성한다.

Client와 Server 사이에 프로젝트 내부 전용 프로토콜을 추가하지 않고 가능한 한 표준 RDP 프로토콜을 사용한다.

이를 통해 Windows 기본 RDP 구현과의 상호 운용성을 확보한다.

## 5.2 FreeRDP 의존성 분리

Qt UI 코드에서 FreeRDP API를 직접 광범위하게 호출하지 않는다.

다음과 같이 Wrapping Layer를 둔다.

```text
Qt UI
  │
  ▼
Application Layer
  │
  ▼
FreeRDP Wrapper
  │
  ▼
FreeRDP
```

이를 통해 향후 FreeRDP 버전 변경에 따른 영향 범위를 최소화한다.

## 5.3 Platform 종속 코드 분리

Windows와 macOS에서 구현 방식이 다른 기능은 Platform Layer로 분리한다.

대표적으로 다음 기능이 이에 해당한다.

```text
Desktop Capture
Keyboard Input
Mouse Input
Clipboard
System Service / Background Execution
```

상위 Application Logic에서는 Platform API를 직접 사용하지 않는 것을 원칙으로 한다.

---

# 6. 우선 검증 항목

본 프로젝트는 처음부터 전체 기능을 구현하지 않고, RDP Client와 Server의 호환성을 단계적으로 검증하며 개발한다.

## 6.1 단계 1 — Windows RDP Client 개발

Windows에서 동작하는 Qt 기반 RDP Client의 최소 기능을 구현한다.

### 구현 범위

* Qt Widgets 기본 UI
* Server 주소 입력
* FreeRDP 초기화
* RDP Server 연결
* 원격 Desktop 화면 출력
* 연결 종료

### 목표

```text
Qt RDP Client
       │
       │ RDP
       ▼
RDP Server
```

FreeRDP와 Qt Widgets 간 기본 연동을 검증한다.

---

## 6.2 단계 2 — Windows 기본 RDP Server 호환성 검증

개발한 RDP Client를 이용하여 Windows에서 제공하는 기본 RDP Server에 접속한다.

```text
RDPClient
Qt + FreeRDP
       │
       │ RDP
       ▼
Windows Remote Desktop
Server
```

### 검증 항목

* RDP 연결
* 인증
* Desktop 화면 수신
* 화면 갱신
* Keyboard 입력
* Mouse 입력
* Clipboard

### 완료 기준

Windows 기본 원격 데스크톱 서버에 정상적으로 접속하고 기본적인 원격 제어가 가능해야 한다.

---

## 6.3 단계 3 — Windows RDP Server 개발

FreeRDP Server API를 이용하여 Windows에서 동작하는 RDP Server를 개발한다.

초기 버전에서는 기능 검증을 위해 실제 Desktop Capture보다 **Test Pattern 또는 임의 Frame 전송을 우선 구현할 수 있다.**

```text
RDP Client
       │
       │ RDP
       ▼
FreeRDP Server
       │
       ▼
Test Frame
```

Test Frame 전송이 검증되면 Windows Desktop Capture를 추가한다.

```text
Windows Desktop
       │
       ▼
Desktop Capture
       │
       ▼
FreeRDP Server
```

---

## 6.4 단계 4 — Windows 기본 RDP Client 호환성 검증

개발한 RDP Server에 Windows 기본 RDP Client를 이용하여 접속한다.

```text
Windows Remote Desktop Client
            │
            │ RDP
            ▼
       RDPServer
       Qt + FreeRDP
```

### 검증 항목

* Server Discovery / Connection
* RDP Negotiation
* TLS
* Desktop 화면 표시
* Keyboard
* Mouse
* Clipboard
* 연결 종료 및 재연결

### 완료 기준

Windows 기본 Remote Desktop Client를 이용하여 개발한 RDP Server에 정상적으로 연결하고 원격 Desktop을 제어할 수 있어야 한다.

---

## 6.5 단계 5 — 자체 Client / Server 통합 검증

개발한 Client와 Server를 연결하여 전체 시스템을 검증한다.

```text
RDPClient
Qt + FreeRDP
       │
       │ RDP
       ▼
RDPServer
Qt + FreeRDP
```

다음 기능을 통합 검증한다.

* 연결
* 인증
* Desktop Capture
* 화면 갱신
* Keyboard
* Mouse
* Clipboard
* 연결 종료
* 재연결

---

## 6.6 단계 6 — macOS 지원

Windows에서 Client와 Server의 기본 기능 및 상호 호환성을 검증한 이후 macOS 지원을 진행한다.

Client의 FreeRDP 및 Qt 기반 공통 로직은 최대한 재사용한다.

OS 종속 기능만 macOS용 구현으로 추가한다.

주요 대상은 다음과 같다.

* Desktop Capture
* Keyboard 처리
* Mouse 처리
* Clipboard
* Background 실행
* 권한 처리

최종적으로 다음 조합을 검증한다.

| Client  | Server  | 검증 |
| ------- | ------- | -- |
| Windows | Windows | 필수 |
| Windows | macOS   | 필수 |
| macOS   | Windows | 필수 |
| macOS   | macOS   | 필수 |

---

# 7. 개발 우선순위

전체 개발 순서는 다음과 같다.

```text
1. Windows RDP Client 최소 기능 구현
                │
                ▼
2. Windows 기본 RDP Server 연결 검증
                │
                ▼
3. Keyboard / Mouse / Clipboard 검증
                │
                ▼
4. Windows RDP Server 최소 기능 구현
                │
                ▼
5. Test Frame 전송 검증
                │
                ▼
6. Windows 기본 RDP Client 연결 검증
                │
                ▼
7. Windows Desktop Capture 적용
                │
                ▼
8. 자체 Client ↔ Server 통합 검증
                │
                ▼
9. Server 백그라운드 실행 및 자동 시작
                │
                ▼
10. macOS Client 지원
                │
                ▼
11. macOS Server 지원
                │
                ▼
12. Windows / macOS Cross-Platform 검증
```

---

# 8. 1차 개발 완료 기준

Windows 1차 개발의 완료 기준은 다음과 같다.

### Client

* Qt Widgets 기반 Client 실행 가능
* Windows 기본 RDP Server 연결 가능
* Desktop 화면 표시 가능
* Keyboard 입력 가능
* Mouse 입력 가능
* Clipboard Text 전송 가능

### Server

* Qt 기반 Server 실행 가능
* RDP Client 연결 대기 가능
* Windows 기본 RDP Client 연결 가능
* Desktop 화면 전송 가능
* Keyboard/Mouse 원격 입력 가능
* Client 연결/종료 처리 가능
* 백그라운드 실행 가능

### 상호 운용성

다음 두 조합이 정상적으로 동작해야 한다.

```text
QtRdp Client
        ↓
Windows RDP Server
```

```text
Windows RDP Client
        ↓
QtRdp Server
```

이를 만족한 후 macOS 지원 개발을 진행한다.

---

# 9. 최종 목표

최종적으로 다음 구성을 지원한다.

```text
        QtRdp Client
     Windows / macOS
             │
             │
            RDP
             │
             ▼
        QtRdp Server
     Windows / macOS
             │
             ▼
       Local Desktop
```

Client와 Server는 각각 독립적인 애플리케이션으로 제공하며, FreeRDP를 RDP Protocol Engine으로 사용한다.

Qt를 이용하여 Windows와 macOS 간 Application 구조와 UI를 최대한 공통화하고, 운영체제에 종속적인 기능만 별도의 Platform Layer로 구현한다.
