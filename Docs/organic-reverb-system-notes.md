# Organic Reverb System — UE5.8 포폴 프로젝트 노트

> 참고 영상: [BODYCAM Devlog 2/4 — Sound Design](https://www.youtube.com/watch?v=JHu6wkpy_1c) (Reissad Studio)
> 목적: BODYCAM의 "오가닉 리버브(Organic Reverb)" 시스템을 참고하여 언리얼 엔진 5.8 기반 포트폴리오용 오디오 시스템 구현

---

## 1. 오가닉 리버브란 무엇인가

기존 게임 오디오의 리버브 처리는 대부분 **Reverb Volume 방식** — 미리 정해둔 존(zone)에 들어가면 프리셋 리버브를 스위칭하는 방식이라, 공간이 바뀌면 리버브가 부자연스럽게 뚝뚝 전환됨.

**오가닉 리버브**는 이와 달리:

- 공간(방의 크기, 천장 높이, 밀폐 정도, 재질)을 **실시간으로 분석**
- 물리 공식 기반으로 **RT60(잔향 시간)을 동적으로 계산**
- 계산 결과를 리버브 DSP 파라미터(Decay, Density, Diffusion, Wet/Dry)에 실시간 매핑

> 핵심 차별점: "프리셋 스위칭"이 아니라 "공간을 시뮬레이션해서 리버브를 도출"

추가로 BODYCAM 시스템은 **충격파(Shockwave) 전파**까지 다룸: 총성의 압력파가 복도/계단/개구부를 따라 물리적으로 퍼져나가는 것을 시뮬레이션하고, 이를 리버브 파라미터 계산과 연동.

### BODYCAM 스크린샷 분석 (RGB 디버그 히트맵)
영상 속 자막: *"이제 오가닉 리버브 덕분에 충격파가 공간을 타고 어떻게 퍼지는지 표현할 수 있습니다."*

→ 이 화면은 레이 디버그가 아니라 **음향 에너지 밀도 히트맵(Acoustic Energy Density Heatmap)**:
- 빨강/주황 = 에너지(음압) 밀도 높음 (발사 지점 및 직결 복도)
- 초록/청록/파랑 = 에너지 낮음 (개구부를 거쳐 감쇠되며 도달한 옆방)
- 복도를 타고 흐르는 빨간 띠가 각 방의 입구를 통해 "새어 들어가며" 식어가는(파랑으로 변하는) 게 핵심

일반적으로 오디오 디버그에서 RGB가 쓰이는 경우:
1. **주파수 대역별 코딩** (R=저역, G=중역, B=고역 RT60/에너지)
2. **레이 종류별 코딩** (초록=직접음 성공, 노랑=초기반사, 빨강=차폐/후기잔향)
3. **공간 프로브 히트맵** (파랑=짧은 잔향 → 빨강=긴 잔향)

---

## 2. 필수 음향 이론

### 2.1 사운드의 3단계 구조
1. **Direct Sound** (직접음)
2. **Early Reflections** (초기 반사음, ~수십 ms 이내)
3. **Late Reverberation** (후기 잔향, 지수적 감쇠)

→ 이 세 단계를 각각 다르게 처리하는 것이 리얼한 공간감의 핵심.

### 2.2 RT60 / Sabine 방정식
- RT60 = 음압이 60dB 감쇠하는 데 걸리는 시간
- Sabine 공식: `RT60 = 0.161 × V / A`
  - V = 방 부피
  - A = 총 흡음면적 (재질별 흡음계수 × 표면적의 합)
- 재질별 흡음계수 데이터(콘크리트, 카펫, 유리, 나무 등)가 사전에 필요

### 2.3 Geometric Acoustics vs Wave-based Acoustics
- 실시간성 때문에 게임에서는 보통 **레이/패스 트레이싱 기반 기하음향 근사**를 사용 (Wave 시뮬레이션은 연산량 과다)
- 벽/천장에 레이를 쏴서 반사 지점, 거리, 재질 정보를 획득 → 파라미터 유도

### 2.4 Occlusion / Obstruction / Exclusion (OOE)
| 용어 | 의미 |
|---|---|
| Occlusion | 소스-리스너 모두 벽으로 막힘 (직접음+반사음 모두 감쇠) |
| Obstruction | 직접음만 막힘 (반사음은 살아있어 "벽 너머 소리"가 자연스럽게 들림) |
| Exclusion | 리스너는 리버브 공간 안, 소스는 다른 공간 |

### 2.5 기타 기본 개념
- 역제곱 법칙 (거리 감쇠)
- 도플러 효과 (이동체 사운드)
- 주파수별 감쇠 차이 (고주파는 빨리 감쇠, 저주파는 오래 남음)

### 2.6 음향 에너지 확산 모델 (Acoustic Diffusion Equation)
실제 파동방정식을 실시간으로 푸는 게 아니라, **열 확산(Heat Diffusion)과 수학적으로 유사한 근사 모델**을 사용 (Nikunj Raghuvanshi 등의 파라메트릭 웨이브 필드 연구 계열).

핵심 아이디어:
1. 레벨을 **방(Room) = 노드, 개구부(문/복도) = 엣지**로 이루어진 그래프로 단순화
2. 각 방을 "에너지 버킷"으로 취급, 인접 방과 개구부 크기/면적에 비례해 에너지가 유출입
3. 시간에 따라 에너지가 확산·감쇠 (매 프레임 또는 매 N프레임 갱신)
4. 결과값을 리버브 Wet Level/Decay 파라미터로 변환 + 디버그 히트맵으로 시각화

---

## 3. 언리얼 5.8 구현에 필요한 요소

| 구성 요소 | 설명 |
|---|---|
| **MetaSounds** | 절차적 오디오 그래프. 리버브 파라미터를 실시간 변수로 받아 처리 |
| **Steam Audio 플러그인** | 레이트레이싱 기반 반사/오클루전 지원 (Valve, 무료 오픈소스). 사실상 필수급 |
| **Submix + Reverb Effect** | 계산된 파라미터를 적용할 오디오 버스 |
| **Runtime Raycast 시스템** | 룸 치수/재질을 실시간 측정 (C++ 또는 Blueprint) |
| **Physical Material → Acoustic Material 매핑 테이블** | 재질별 흡음계수/반사계수 데이터 |
| **Audio Gameplay Volume / Trigger Volume** | 공간 전환 감지 (occlusion 판정 보조) |
| **방-그래프 에너지 확산 로직** | 커스텀 C++ — 방 사이 연결 그래프 생성 및 에너지 전파 계산 |
| **디버그 시각화 레이어** | DrawDebugLine/Sphere, Post Process Material, 또는 Decal로 히트맵 표시 |

디버그 드로우 예시:
```cpp
if (bHit)
{
    DrawDebugLine(World, Start, HitLocation, FColor::Green, false, 2.0f, 0, 1.5f);
}
else
{
    DrawDebugLine(World, Start, End, FColor::Red, false, 2.0f, 0, 1.5f);
}
```

---

## 4. 테스트 씬 구성안

1. **가변 룸(Modular Room)** — 벽/천장 높이를 런타임 조절 가능한 박스형 공간. 크기 변화에 따른 실시간 리버브 변화 시연
2. **재질 교체 테스트룸** — 동일 크기 방, 벽 재질만 콘크리트↔카펫↔유리로 교체하며 RT60 차이 비교
3. **개방-밀폐 전이 공간** — 실내 → 복도 → 야외로 이어지는 씬 (Occlusion/Exclusion 테스트)
4. **벽 너머 소리 테스트** — 소스-리스너 사이 벽 하나 (Obstruction vs Occlusion 구분 시연)
5. **디버그 시각화 씬** — 레이캐스트 라인, 계산된 RT60 수치, 반사 지점, 에너지 히트맵 오버레이 (포폴 핵심 어필 포인트)

---

## 5. 엔진 이식성(포팅) 전략

**결론: 완전 범용은 불가능하지만, 구조 설계로 포팅 비용을 최소화할 수 있음.** (Steam Audio가 채택한 방식과 동일 개념)

### 3-레이어 구조 및 엔진 의존도

| 레이어 | 하는 일 | 엔진 의존도 |
|---|---|---|
| ① 지오메트리 쿼리 | 방/벽/개구부 감지 (레이캐스트, 콜리전) | 높음 |
| ② 코어 시뮬레이션 | 에너지 확산 계산, RT60 산출, 그래프 순회 | **없음** (순수 수학/로직) |
| ③ 오디오 출력 | 파라미터를 실제 리버브 DSP에 적용 | 높음 |

### Core + Adapter 아키텍처
1. **Core (순수 C++ 라이브러리)** — `UObject`, `FVector` 등 엔진 타입 의존 없이 순수 수학/DSP만 구현
2. **엔진별 어댑터** — 지오메트리를 Core가 이해하는 포맷(삼각형 메시 배열, float 배열 등)으로 변환, 결과를 다시 엔진 오디오 API로 전달
3. 포팅 = 알고리즘 재작성이 아니라 **입출력 변환 레이어(전체 코드의 약 10~20%)만 재작성**

### 데이터 흐름 파이프라인
```
[지오메트리 스캔] (Unreal 어댑터)
        │
        ▼
[방 그래프 생성] (Core)
        │
        ▼
[에너지 확산 계산] (Core) ──→ [디버그 히트맵]
        │
        ▼
[리버브 파라미터 매핑] (Core)
        │
        ▼
[오디오 출력: MetaSounds Submix] (Unreal 어댑터)
```
- 파랑 계열 = 엔진 종속(Adapter) / 청록 계열 = 엔진 독립(Core) / 회색 = 디버그 전용

### 포폴 전략
- Core 로직(방-그래프 확산 시뮬레이션)은 순수 C++로, Unreal 타입 의존 없이 작성
- Unreal 전용 부분(레이캐스트 지오메트리 추출, MetaSounds 파라미터 바인딩)은 얇은 래퍼로 분리
- 발표 자료에 "엔진 독립적 코어 + 엔진별 어댑터 분리 설계로 Unity/FMOD 포팅 용이"라고 명시 → 확장 가능한 아키텍처 설계력 어필

---

## 6. v0 초안 코드 구조 (`Docs/files/`)

> 1단계 당시 기록이다. 프로토타입에서 결함 4건이 나와 Core는 `AcousticCore/`로 다시 썼고, 아래 Unreal 헤더 두 개는 컴파일되지 않아 폐기한 뒤 4단계에서 새 구조로 작성했다 ([architecture-design.md](architecture-design.md) §3.5). `Docs/files/`는 결함 재현 테스트용으로만 남겨 둔다.

```
organic-reverb-architecture/
├── Core/                              (엔진 독립 — 순수 C++, 그대로 포팅 가능)
│   ├── AcousticTypes.h                재질/에너지/ID 등 기본 타입 정의
│   ├── AcousticRoomGraph.h            FRoom(노드) / FPortal(엣지) 그래프 자료구조
│   └── AcousticDiffusionSimulator.h   Sabine RT60 계산 + 에너지 확산/감쇠 로직 (구현 포함)
└── Unreal/                            (엔진 종속 — 어댑터)
    ├── AcousticGeometryScanner.h      레벨 레이캐스트 → RoomGraph 빌드 (선언만, .cpp 미구현)
    └── OrganicReverbSubsystem.h       Core를 소유/틱하는 UWorldSubsystem (선언만, .cpp 미구현)
```

**핵심 클래스 요약**
- `Acoustic::FRoom` — 방 하나의 부피, 표면적, 재질, 현재 에너지, RT60 값 보유
- `Acoustic::FPortal` — 방-방 연결부(문/복도), 개구부 면적과 거리 정보
- `Acoustic::FAcousticRoomGraph` — 방/포탈 컬렉션, 인접 리스트 관리
- `Acoustic::FAcousticDiffusionSimulator` — `Tick(DeltaTime)`에서 RT60 갱신 → 인접 방 간 에너지 확산 → 자체 감쇠 적용. `GetWetLevel(RoomId)`로 결과 조회
- `UAcousticGeometryScanner` — 6방향 레이캐스트로 방 치수 추정, 피직컬 머티리얼 → 흡음계수 매핑, 개구부 감지 (v0: 선언만)
- `UOrganicReverbSubsystem` — 매 틱 시뮬레이터 갱신, `EmitAcousticEvent()`/`GetWetLevelAtLocation()` 게임플레이 API 제공, 디버그 히트맵 표시 (v0: 선언만)

---

## 7. 다음 작업 후보 (TODO)

- [x] **Core 로직 단독 유닛 테스트** — `Prototype/build.bat`로 실행. v0 결함 4건 발견 후 v1 Core(`AcousticCore/`)로 재작성. 결과는 [architecture-design.md](architecture-design.md) §4 참고
- [x] 공간 자동 분할 Core 구현 — 6방향 레이 대신 복셀 기반 `AcousticSpaceSegmenter` (콘솔 검증 완료)
- [x] Unreal 어댑터: 레벨 → 점유 격자 스캔 (`FAcousticGridScanner`, 오버랩 테스트 + 재질 조회)
- [x] `UOrganicReverbSubsystem` 구현 (Tick, EmitAcousticEvent, 문 개폐, 콘솔 명령)
- [x] 재질 흡음계수 연동 — `UAcousticPhysicalMaterial` (Physical Material 서브클래스)
- [x] Submix Reverb 프리셋에 파라미터 실시간 반영 (MetaSounds 연결은 확장 단계)
- [x] 디버그 히트맵 — DrawDebug 기반 바닥 히트맵 (6-3에서 디퍼드 데칼 히트맵으로 교체)
- [x] 테스트 씬 생성 도구 — `AAcousticTestLayout` 프리셋 (씬 1·2·3·5 + 결합 공간). 씬 4(벽 너머 소리)는 OOE 구현 후
- [x] 테스트 맵 5종 구성 + 오디오 에셋(Submix / Reverb 프리셋) 생성·연결 — `ORS_Unreal/Scripts/create_test_maps.py`, 5개 맵 모두 게임 모드에서 스캔 확인
- [x] 에디터에서 PIE 청취 확인 + 튜닝 ([unreal-setup-guide.md](unreal-setup-guide.md))
- [ ] Steam Audio 플러그인 세팅 (UE 5.8 호환성 확인)

### 5단계 (반복 개발 + 디버그 툴링)
- [x] 5-1 공유 벽 재질 결정성 (흡음이 큰 재질 우선), 문 없는 바깥 공기는 히트맵에서 제외
- [x] 5-1b 면 단위 재질 — 벽 양쪽 마감재 구분 (격자 `FaceMaterials` + 스캐너 면 슬랩 조회), 테스트 레이아웃 벽 25 cm로 겹침 제거, 테스트 맵 재생성
- [x] 5-2 음원별 전파 Core — 문 경로 회절/차폐(Occlusion·Obstruction) + 잔향 결합(Exclusion), 콘솔 테스트 8종
- [x] 5-3 Unreal 연결 — `UOrganicSoundSourceComponent`, `AOrganicTestSoundSource`, 씬 4 맵 `ORS_WallTest`, 경로 디버그 선. 게임 모드 확인
- [x] 5-4 디버그 도구 — `ors.Shot` = 총성 + 리스너 방 감쇠 측정(EDT / T20 / 후기 RT, 이중 기울기 자동 판정), 화면 좌하단 감쇠 곡선 그래프 + 로그
- [x] 5-7 원거리 총성 (거리감) — Core `RenderShot`(콘솔 4종) + Unreal `PlayGunshotAtLocation` + `AOrganicTestGunshotEmitter` + 맵 `ORS_DistantGunshot`. 게임 모드: 47.5 m 경로, 139 ms 지연, −33 dB, 로우패스 3.9 kHz, 잔향 Send 보정 0.63
- [x] 총성 사운드 교체 — 돌격소총 단발음 모노 웨이브 6종(발사할 때마다 무작위). 볼륨 `Shot Sounds`(ors.Shot) + 총기 `Near Sounds`, `Scripts/apply_gun_sounds.py`로 모든 테스트 맵에 적용
- [x] 5-6 청취 튜닝 — 회절 계수(문 모서리를 돌아도 −5 dB 정도로 약함), 벽 투과 손실, Wet 양

### 6단계 (최적화 / 폴리싱)
상세 기록은 [architecture-design.md](architecture-design.md) §3.9 ~ §3.13.

- [x] 6-1 공기 흡음 — Sabine 분모에 4mV 항. 잔향·잔향 결합·직접음이 같은 계수(`AirAbsorption::Coefficient()`) 공유. 30 m 홀 Mid 16.1 → 11.0 s, HF 비율 0.75 → 0.40
- [x] 6-2 경로 당기기(string pulling) — 복도 분할 경계의 가상 꼭짓점 제거 (5-8에서 이동). 게임 모드 A/B 직접음 −33.3 → −33.1 dB, 로우패스 3.9 → 4.0 kHz
- [x] 6-3 에너지 히트맵 — 포스트 프로세스 대신 디퍼드 데칼(`M_AcousticHeatmap`) + 방 번호 아틀라스 텍스처. 레벨 표면을 월드 좌표 기준으로 칠함
- [x] 6-4 비동기 스캔 — 격자 스캔은 프레임당 예산(2 ms), 방 분할은 워커 스레드, 끝나면 그래프 교체. 26,082칸 스캔 40 ms를 19프레임에 분산
- [x] 6-5 방 사이 전달 지연 — 포탈 지연 큐, (방 중심 → 문 → 옆방 중심) 거리 / 음속 (5-5에서 이동. 총성 직접음 지연은 5-7에 포함). 34.3 m 옆방 첫 도착 0.100 s, 틱 80 → 145 µs
