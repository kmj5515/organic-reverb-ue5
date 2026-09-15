# Organic Reverb — Unreal 사용 가이드 (4단계)

> 대상: 이 프로젝트를 에디터에서 열어 테스트 씬을 만들고 소리를 확인하려는 사람
> 선행: [architecture-design.md](architecture-design.md)

---

## 1. 구성 요소

| 클래스 | 역할 |
|---|---|
| `AOrganicReverbVolume` | 레벨에 1개 배치. 스캔 범위(박스)와 모든 튜닝 값, 출력할 Reverb 프리셋을 가진다 |
| `UOrganicReverbSubsystem` | 자동 생성(Game/PIE). 스캔 → 방 분할 → 시뮬레이션 틱 → Submix Reverb 반영 → 디버그 히트맵 |
| `AAcousticTestLayout` | 에셋 없이 방/벽/문을 생성하는 화이트박스 액터. 프리셋 버튼 7종 |
| `UAcousticPhysicalMaterial` | 실제 레벨 메시용 흡음계수 (Physical Material 서브클래스) |
| `UOrganicSoundSourceComponent` | 음원별 전파: 같은 액터의 AudioComponent에 벽 차폐(볼륨·로우패스)와 잔향 Send를 적용 |
| `AOrganicTestSoundSource` | 청취 테스트용 음원 (1 kHz 핑 반복 재생 + 위 컴포넌트) |
| `AOrganicTestGunshotEmitter` | 청취 테스트용 원거리 총기. 일정 간격으로 `PlayGunshotAtLocation` 호출 (근/원거리 총성 음원 지정 가능) |
| `FAcousticGridScanner` | 월드 → 점유 격자 변환 (UObject 아님, 내부용) |

소스: `ORS_Unreal/Source/ORS_Unreal/Acoustics/`, 엔진 독립 Core: `AcousticCore/include/AcousticCore/`

---

## 2. 테스트 맵

에디터를 열면 `ORS_Corridor`가 기본으로 열린다. Play 후 콘솔에 `ors.Shot`을 입력하면 된다.

| 맵 (`/Game/OrganicReverb/Maps/`) | 테스트 씬 | 확인할 것 |
|---|---|---|
| `ORS_Corridor` | 씬 5: BODYCAM 복도 히트맵 | 복도에서 쏘면 옆방으로 에너지가 새어 들어가는 모습 |
| `ORS_ClosetAndHall` | 결합 공간 | 옷장 안에서 쐈을 때 짧은 잔향 뒤에 홀의 긴 꼬리 |
| `ORS_MaterialComparison` | 씬 2: 재질 비교 | 콘크리트 방과 카펫 방의 잔향 차이 |
| `ORS_ModularRoom` | 씬 1: 가변 룸 | `SetRoomSize`로 크기를 바꿀 때 리버브 변화 |
| `ORS_Courtyard` | 씬 3: 개방-밀폐 전이 | 천장 없는 안뜰 ↔ 실내 이동 |
| `ORS_DistantGunshot` | 원거리 총성 | 대기실에서 3초마다 모퉁이 너머 약 50 m 밖 총성이 늦게(≈0.15 s), 복도 입구 쪽에서, 먹먹하고 울리게 들림. 복도로 나가 총기 쪽으로 걸어가면 점점 선명하고 "탕"에 가까워짐 |
| `ORS_WallTest` | 씬 4: 벽 너머 소리 | 옆방 음원(1.5초마다 핑)이 벽에 막혀 작고 먹먹하게 들리다가, 문 쪽으로 걸어가면 선명해짐. 문 근처에서 `ors.Door 0` / `1` |

맵과 오디오 에셋은 [create_test_maps.py](../ORS_Unreal/Scripts/create_test_maps.py)가 만든다. 다시 만들려면 해당 맵/에셋을 지우고 실행한다 (이미 있는 것은 건너뜀).
- 에디터: Output Log 입력창 왼쪽 `Cmd`를 `Python`으로 바꾸고 `py "<저장소 경로>/ORS_Unreal/Scripts/create_test_maps.py"` (예: `C:/UnrealProject/organic-reverb-ue5`)
- 헤드리스: `UnrealEditor-Cmd.exe ORS_Unreal.uproject -ExecutePythonScript="<스크립트 경로> --quit" -unattended -nullrhi -nosound`

### 총성 사운드 지정
총성 사운드는 저장소에 없으므로 각자 준비한다 (돌격소총 단발음 모노 웨이브 6종 기준, 발사할 때마다 무작위).
- `Organic Reverb Volume` → Acoustics | Gunshot → **Shot Sounds**: `ors.Shot` 테스트 총성 (비우면 엔진 핑)
- `Organic Test Gunshot Emitter` → Gunshot → **Near Sounds** / **Far Sounds**: 원거리 총기 (Far는 원거리 녹음이 있을 때만)
- 모든 테스트 맵에 한 번에 넣기: [apply_gun_sounds.py](../ORS_Unreal/Scripts/apply_gun_sounds.py) (맵 생성 스크립트 다음에 실행)

사운드 큐 대신 원본 웨이브를 쓰는 이유: 큐 안에 자체 거리 감쇠 노드가 있으면 시스템의 거리감 계산과 겹칠 수 있다. 스테레오 대신 모노를 쓰는 이유: 방향(패닝)이 정확하고, 녹음된 공간 울림이 시스템 리버브와 겹치지 않는다.

### 2-1. 새 레이아웃을 직접 만들 때

1. **레벨**: File → New Level → Basic (템플릿의 바닥 메시는 레이아웃 바닥과 겹치므로 지운다)
2. **레이아웃 배치**: Place Actors 패널에서 `Acoustic Test Layout` 검색 → 레벨에 드래그 → 위치 (0, 0, 0)
3. **프리셋 선택**: Details → `Layout | Presets` 버튼 중 하나 클릭
   - `Preset Modular Room` — 가변 룸 (씬 1)
   - `Preset Material Comparison` — 콘크리트 방 vs 카펫 방 (씬 2)
   - `Preset Courtyard` — 천장 없는 안뜰 + 실내 (씬 3)
   - `Preset Corridor With Side Rooms` — BODYCAM 복도 히트맵 (씬 5)
   - `Preset Closet And Hall` — 결합 공간 이중 기울기 감쇠
   - `Preset Wall Test` — 벽 너머 소리 (씬 4)
   - `Preset Long Corridor` — 모퉁이를 도는 긴 복도 (원거리 총성)
   - 버튼을 누르면 `Organic Reverb Volume`이 없을 경우 자동 생성되고 스캔 범위가 레이아웃에 맞춰진다
4. **시작 위치**: 방 안에 `Player Start` 배치, 또는 Play 버튼 옆 ⋮ → Spawn player at → Current Camera Location
5. **Play (PIE)** → Output Log에서 `LogOrganicReverb` 필터 → 방 목록과 RT60 확인

방 좌표는 격자 칸 크기(기본 50 cm)의 배수로 두고, 인접한 방은 50 cm 띄워 벽 두께 25 cm로 두면 스캔이 정확하고 공유 벽의 양쪽 재질도 구분된다 (스캐너가 벽 면마다 재질을 읽는다). 프리셋은 모두 이 규칙을 따른다.

---

## 3. 오디오 연결 (Submix Reverb)

> 테스트 맵은 이미 연결되어 있다: `SEP_OrganicReverb`(프리셋) → `SM_OrganicReverb`(Submix) → Project Settings의 Reverb Submix, 각 맵의 볼륨에 프리셋 지정. 아래는 새 맵이나 다른 프로젝트에서 직접 연결할 때의 절차다.

1. Content Browser → 우클릭 → Audio → Effects → **Submix Effect Preset** → `Submix Effect Reverb Preset` 선택 → `SEP_OrganicReverb`
2. Content Browser → 우클릭 → Audio → **Submix** → `SM_OrganicReverb` → 열어서 Submix Effect Chain에 `SEP_OrganicReverb` 추가
3. Project Settings → Engine → Audio → Mix → **Reverb Submix** = `SM_OrganicReverb`
4. 레벨의 `Organic Reverb Volume` → Details → Acoustics | Output → **Reverb Preset** = `SEP_OrganicReverb`
5. 소리 확인용: 아무 Sound Wave를 레벨에 드래그(Ambient Sound) → Attenuation Settings에서 **Enable Reverb Send** 체크

플레이 중에는 서브시스템이 프리셋의 `DecayTime`, `DecayHFRatio`, `ReflectionsDelay`, `WetLevel`만 덮어쓰고, 나머지 값(Density, Diffusion 등)은 에셋에 설정한 값을 유지한다. 플레이가 끝나면 원래 값으로 되돌린다.

---

## 4. 플레이 중 조작 (콘솔 `~`)

| 명령 | 동작 |
|---|---|
| `ors.Debug 1` / `0` | 에너지 히트맵 + 방 이름/RT60/dB + 포탈 표시 (`-1` = 볼륨 설정 따름) |
| `ors.Heatmap.Intensity 1` | 히트맵 밝기. `0`이면 히트맵만 끄고 글자·포탈 표시는 유지. 레벨이 밝으면 낮춰 쓴다 |
| `ors.Shot [세기]` | **청취 테스트용 총성 + 감쇠 측정**: 리스너 위치에서 1 kHz 핑(리버브 Send 켜짐) 재생 + 에너지 주입. 화면 좌하단에 그 방의 감쇠 곡선과 EDT / T20 / 후기 RT를 그리고, 후기가 EDT보다 훨씬 길면 `DOUBLE SLOPE` 표시. 끝나면 로그 |
| `ors.Emit [세기]` | 리스너 위치 방에 총성 에너지만 주입 (소리 없음, 기본 100). 히트맵에서 퍼지는 모습 확인. 레벨 시작 인자(`-ExecCmds`)로 줘도 스캔이 끝난 뒤 발동한다 |
| `ors.Door 0` / `1` | 리스너에서 가장 가까운 문(4 m 이내) 닫기 / 열기 |
| `ors.Rebuild` | 레벨 재스캔 + 방 그래프 재생성 |

화면 좌상단에 현재 리스너 방, Decay, HF 비율, 반사 지연, 이벤트 Wet이 표시된다.

음원(`UOrganicSoundSourceComponent`)이 있으면 음원에서 리스너까지 경로가 선으로 표시된다.

| 색 | 의미 |
|---|---|
| 초록 | 같은 방이거나 열린 문 너머로 직선으로 보임 — 손실 없음 |
| 노랑 | 문을 돌아서 옴 — 약간 작고 고음이 깎임 |
| 빨강 | 열린 문 경로 없음 — 벽 투과음만 (크게 작고 먹먹함) |

---

## 5. Blueprint / C++ API

`Get World Subsystem (OrganicReverbSubsystem)`에서 호출:

| 함수 | 용도 |
|---|---|
| `PlayGunshotAtLocation(NearSound, FarSound, Location, Intensity)` | **총기 발사 시 호출.** 리스너 기준 거리감(도착 지연, 문 쪽 방향, 거리 감쇠·공기 흡수·회절/차폐, 로우패스, 근/원거리 녹음 크로스페이드, 직접음:잔향 비율)으로 재생하고 에너지도 주입 |
| `EmitAcousticEvent(Location, Intensity)` | 소리 없이 에너지만 주입 (폭발 잔향 등 직접 재생하는 소리와 함께 쓸 때) |
| `GetReverbParamsAtLocation(Location)` | 위치의 방 번호, Decay, HF 비율, 반사 지연, 이벤트 Wet |
| `GetWetLevelAtLocation(Location)` | 이벤트 Wet만 (MetaSounds 등 다른 출력에 연결할 때) |
| `SetPortalTransmissionAtLocation(Location, 0~1, Radius)` | 문 개폐 연동 |
| `RequestRebuild()` | 지오메트리 변경 후 재스캔 (몇 프레임 뒤 실행) |

**음원별 전파 (벽 너머 소리):** 소리를 내는 액터에 `Organic Sound Source` 컴포넌트를 추가하면 된다.
- 같은 액터의 첫 번째 AudioComponent에 매 틱 볼륨·로우패스·Reverb Submix Send를 적용한다
- 해당 AudioComponent의 Attenuation에서 **Reverb Send는 끈다** (컴포넌트가 잔향 결합만큼 Send를 직접 넣으므로 중복 방지)
- `GetPropagationInfo()`로 차폐 여부, 거친 문 수, 직접음 감쇠, 로우패스, 잔향 Send를 읽을 수 있다

`AAcousticTestLayout`의 `SetRoomSize(RoomIndex, Size)` / `SetRoomMaterial(RoomIndex, Preset)`은 런타임에 방을 바꾸고 자동으로 재스캔한다 (가변 룸 시연용).

---

## 6. 실제 레벨 메시에 재질 지정

1. Content Browser → 우클릭 → Physics → **Physical Material** → 클래스 선택에서 `Acoustic Physical Material`
2. `Absorption` (Low / Mid / High) 입력
3. 메시가 쓰는 머티리얼의 `Phys Material`로 지정 (또는 컴포넌트의 Phys Material Override)

재질 정보가 없는 표면은 볼륨의 `Default Absorption`(기본 콘크리트)을 쓴다.

---

## 7. 튜닝 값 (`Organic Reverb Volume`)

| 값 | 기본 | 설명 |
|---|---|---|
| Cell Size | 50 cm | 격자 해상도. 절반으로 줄이면 스캔 비용 8배 |
| Scan Time Budget | 2 ms | 백그라운드 스캔이 한 프레임에 쓸 시간. 크게 잡으면 빨리 끝나지만 프레임이 튄다 |
| Scan Object Type | WorldStatic | 벽으로 인식할 오브젝트. 움직이는 문(WorldDynamic)은 개구부로 취급됨 |
| Max Portal Width | 200 cm | 이 폭 이하 개구부 = 방 경계 |
| Max Room Length | 800 cm | 긴 공간 분할 길이. 작을수록 복도 전파가 세밀 |
| Min Room Volume | 2 m³ | 작은 조각 병합 기준 |
| Portal Delay | 켬 | 방 사이 전달 지연. 켜면 옆방에 소리가 거리/음속 만큼 늦게 도착한다 (복도를 따라 번져 가는 게 보임) |
| Air Absorption Scale | 1.0 | 공기 흡음 배율. 1 = 실측값(ISO 9613-1), 0 = 끔(벽 흡음만). 큰 공간·고음일수록 크게 작용해 30 m 홀에서 Mid 잔향 −32 %, High −63 %. 잔향·잔향 Send·총성 직접음에 같이 걸린다 |
| Base Wet Level | 0.3 | 이벤트가 없을 때 Wet |
| Event Wet Boost | 0.7 | 총성 에너지 도달 시 추가 Wet |
| Parameter Smoothing Time | 0.25 s | 방 이동 시 전환 시간 |
| Event Reference Density | 1.0 | 이 밀도에서 이벤트 Wet = 1 |
| Default Event Intensity | 100 | `ors.Emit` 기본 세기 |
| Gunshot Inner Radius | 500 cm | 이 안에서는 총성 거리 감쇠 없음. 밖에서 거리 2배당 −6 dB |
| Gunshot Near / Far Layer Distance | 1000 / 5000 cm | 근거리 → 원거리 녹음 크로스페이드 구간 |
| Gunshot Base Reverb Send | 0.15 | 가까이서 쐈을 때 잔향 양. 멀어져도 잔향 크기는 유지되도록 보정 |

---

## 8. 알려진 제약

- 스캔은 레벨 시작 시 1회. 프레임마다 나눠 진행하고 방 분할은 워커 스레드에서 돌므로 프레임이 멈추지 않는다. 끝날 때까지는 이전 방 그래프로 소리가 난다 (레벨 로드 직후에는 그래프가 없으므로 잠깐 리버브가 적용되지 않는다). 칸 800만 개 초과 시 거부 (큰 레벨은 Cell Size를 키우거나 볼륨을 나눌 것)
- 레이아웃 사이 빈 공간(방 밖 바깥 공기)도 방으로 잡힌다. 포탈이 없으면 시뮬레이션에 영향 없음
- 리버브 파라미터는 전역 Submix 하나에 적용되므로 모든 소리가 리스너 방 기준 리버브를 받는다 (음원별 Send는 5단계 과제)
