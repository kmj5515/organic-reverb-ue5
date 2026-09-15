"""
Organic Reverb — 테스트 맵 + 오디오 에셋 자동 생성

만드는 것
  /Game/OrganicReverb/Audio/SEP_OrganicReverb   Submix Reverb 프리셋 (플레이 중 서브시스템이 값을 덮어씀)
  /Game/OrganicReverb/Audio/SM_OrganicReverb    Reverb Submix (위 프리셋이 이펙트 체인에 들어감)
  /Game/OrganicReverb/Maps/ORS_*                테스트 맵 7종 (레이아웃 + Organic Reverb Volume + Player Start)

실행
  에디터:   Output Log 입력창 왼쪽을 Python으로 바꾸고   py "<이 파일 경로>"
  헤드리스: UnrealEditor-Cmd.exe ORS_Unreal.uproject -ExecutePythonScript="<이 파일 경로> --quit" -unattended -nullrhi -nosound

이미 있는 에셋/맵은 건너뛴다 (다시 만들려면 해당 에셋을 지우고 실행).
Project Settings의 Reverb Submix와 기본 맵은 Config/DefaultEngine.ini에 지정한다.
"""
import sys

import unreal

ROOT = "/Game/OrganicReverb"
AUDIO_DIR = ROOT + "/Audio"
MAP_DIR = ROOT + "/Maps"
TEMPLATE_MAP = "/Engine/Maps/Templates/Template_Default"

# (맵 이름, AAcousticTestLayout 프리셋 함수, Player Start 위치 cm — 첫 방 안쪽, 캡슐 높이만큼 띄움,
#  추가 액터 목록 [(클래스 경로, 위치 cm), ...])
TEST_SOUND_SOURCE = "/Script/ORS_Unreal.OrganicTestSoundSource"
TEST_GUNSHOT_EMITTER = "/Script/ORS_Unreal.OrganicTestGunshotEmitter"
SCENES = [
    ("ORS_Corridor", "PresetCorridorWithSideRooms", (200.0, 125.0, 100.0), []),
    ("ORS_ClosetAndHall", "PresetClosetAndHall", (1000.0, 1000.0, 100.0), []),
    ("ORS_MaterialComparison", "PresetMaterialComparison", (250.0, 200.0, 100.0), []),
    ("ORS_ModularRoom", "PresetModularRoom", (300.0, 250.0, 100.0), []),
    ("ORS_Courtyard", "PresetCourtyard", (500.0, 500.0, 100.0), []),
    # 씬 4: 리스너는 문에서 먼 쪽, 음원은 옆방 먼 쪽 → 벽에 막혀 문을 돌아오는 소리
    ("ORS_WallTest", "PresetWallTest", (200.0, 450.0, 100.0), [(TEST_SOUND_SOURCE, (1450.0, 450.0, 150.0))]),
    # 원거리 총성: 대기실에서 시작, 총기는 모퉁이를 돈 복도 끝 (약 50 m 경로)
    ("ORS_DistantGunshot", "PresetLongCorridor", (300.0, 650.0, 100.0), [(TEST_GUNSHOT_EMITTER, (3200.0, 1850.0, 150.0))]),
]

assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
tools = unreal.AssetToolsHelpers.get_asset_tools()


def log(message):
    unreal.log("[OrganicReverb] " + message)


def get_or_create_asset(name, directory, asset_class, factory):
    path = directory + "/" + name
    if assets.does_asset_exist(path):
        log("exists: " + path)
        return assets.load_asset(path)
    asset = tools.create_asset(name, directory, asset_class, factory)
    if asset is None:
        raise RuntimeError("failed to create " + path)
    log("created: " + path)
    return asset


def create_audio_assets():
    preset_factory = unreal.SoundSubmixEffectFactory()
    preset_factory.set_editor_property("sound_effect_submix_preset_class", unreal.SubmixEffectReverbPreset.static_class())
    preset = get_or_create_asset("SEP_OrganicReverb", AUDIO_DIR, unreal.SubmixEffectReverbPreset, preset_factory)

    submix = get_or_create_asset("SM_OrganicReverb", AUDIO_DIR, unreal.SoundSubmix, unreal.SoundSubmixFactory())
    chain = list(submix.get_editor_property("submix_effect_chain"))
    if preset not in chain:
        chain.append(preset)
        submix.set_editor_property("submix_effect_chain", chain)

    assets.save_loaded_asset(preset, False)
    assets.save_loaded_asset(submix, False)
    return preset


def create_map(map_name, preset_function, player_start, extra_actors, reverb_preset):
    path = MAP_DIR + "/" + map_name
    if assets.does_asset_exist(path):
        log("exists, skipped: " + path)
        return
    if not levels.new_level_from_template(path, TEMPLATE_MAP):
        raise RuntimeError("failed to create level " + path)

    # 템플릿의 바닥 메시는 레이아웃 바닥과 겹쳐 스캔을 방해하고, 기본 Player Start는 방 밖에 있으므로 제거
    for actor in actors.get_all_level_actors():
        if isinstance(actor, (unreal.StaticMeshActor, unreal.PlayerStart)):
            actors.destroy_actor(actor)

    layout_class = unreal.load_class(None, "/Script/ORS_Unreal.AcousticTestLayout")
    layout = actors.spawn_actor_from_class(layout_class, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0))
    layout.call_method(preset_function)  # 벽 생성 + Organic Reverb Volume 생성/범위 맞춤

    volume = next((a for a in actors.get_all_level_actors() if a.get_class().get_name() == "OrganicReverbVolume"), None)
    if volume is None:
        raise RuntimeError("OrganicReverbVolume was not created in " + path)
    volume.set_editor_property("reverb_preset", reverb_preset)

    actors.spawn_actor_from_class(unreal.PlayerStart, unreal.Vector(*player_start), unreal.Rotator(0, 0, 0))

    for class_path, location in extra_actors:
        actor_class = unreal.load_class(None, class_path)
        actors.spawn_actor_from_class(actor_class, unreal.Vector(*location), unreal.Rotator(0, 0, 0))

    if not levels.save_current_level():
        raise RuntimeError("failed to save " + path)
    log("created map: " + path)


def main():
    reverb_preset = create_audio_assets()
    for map_name, preset_function, player_start, extra_actors in SCENES:
        create_map(map_name, preset_function, player_start, extra_actors, reverb_preset)
    log("done")


try:
    main()
finally:
    if "--quit" in sys.argv:
        unreal.SystemLibrary.quit_editor()
