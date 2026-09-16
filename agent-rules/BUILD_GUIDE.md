# Qt Widgets 개발 및 빌드 가이드

## 1. 개발 환경

본 프로젝트는 일반적인 Qt Widgets 기반 데스크톱 애플리케이션으로 개발한다.

| 항목               | 기준               |
| ---------------- | ---------------- |
| UI Framework     | Qt Widgets       |
| Qt Version       | Qt 6.x           |
| Language         | C++17 이상         |
| Build System     | qmake            |
| IDE              | Qt Creator 권장    |
| Windows Compiler | MSVC 2022 64-bit |
| Version Control  | Git              |

Qt 버전과 Compiler의 ABI 및 Architecture는 일치시켜야 한다.

예:

```text
Qt 6.x MSVC 2022 64-bit
→ MSVC 2022 x64 Compiler 사용
```

---

## 2. 프로젝트 구성

일반적으로 다음 파일을 사용한다.

```text
project/
├── project.pro
├── main.cpp
├── mainwindow.h
├── mainwindow.cpp
├── mainwindow.ui
├── resources.qrc
└── src/
```

Qt Designer에서 생성한 `.ui` 파일은 직접 수정할 수 있지만,
빌드 과정에서 생성되는 다음 파일은 직접 수정하지 않는다.

```text
ui_*.h
moc_*.cpp
Makefile
```

---

## 3. Qt Creator 빌드

1. Qt Creator에서 `.pro` 파일을 연다.
2. Qt 6.x Desktop Kit를 선택한다.
3. Debug 또는 Release 구성을 선택한다.
4. `Build`를 실행한다.
5. 빌드 성공 후 애플리케이션을 실행하여 동작을 확인한다.

가능하면 Source 디렉터리와 Build 디렉터리를 분리하는
Shadow Build를 사용한다.

예:

```text
build/
├── debug/
└── release/
```

---

## 4. 명령줄 빌드

Windows MSVC 환경에서는 Developer PowerShell 또는
Developer Command Prompt 사용을 권장한다.

```powershell
mkdir build
cd build

qmake ../project.pro
nmake
```

Release 빌드는 다음과 같이 수행할 수 있다.

```powershell
qmake ../project.pro CONFIG+=release
nmake
```

`.pro` 파일을 변경한 경우에는 `qmake`를 다시 실행한다.

---

## 5. 소스 추가

새로운 소스 파일을 추가한 경우 `.pro` 파일에 등록한다.

```qmake
SOURCES += \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    mainwindow.h

FORMS += \
    mainwindow.ui
```

Qt 모듈이 필요한 경우 다음과 같이 추가한다.

```qmake
QT += core gui widgets
```

---

## 6. 기본 검증

개발 완료 후 최소한 다음 항목을 확인한다.

* Debug 빌드 성공
* Release 빌드 성공
* 애플리케이션 실행 확인
* 주요 UI 기능 동작 확인
* 빌드 Warning 및 Error 확인
* `git status`를 통해 불필요한 생성 파일이 포함되지 않았는지 확인

실제로 실행하지 않은 경우 GUI 동작까지 검증했다고 기록하지 않는다.

---

## 7. Windows 배포

Release 실행 파일 배포 시 Qt의 `windeployqt`를 사용할 수 있다.

```powershell
windeployqt path\to\application.exe
```

이를 통해 Qt DLL 및 필요한 Qt Plugin을 배포 폴더에 복사할 수 있다.

---

## 8. 개발 규칙

* 생성 파일(`ui_*.h`, `moc_*.cpp`, Makefile 등)은 직접 수정하지 않는다.
* 사용자별 Qt Creator 설정 파일은 Git에 포함하지 않는다.
* 새 파일이나 Qt Module 추가 시 `.pro` 파일을 함께 수정한다.
* Qt Kit와 Compiler Architecture를 일치시킨다.
* 작업 전후 `git status`를 확인한다.
