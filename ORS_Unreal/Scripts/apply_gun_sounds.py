"""
Organic Reverb — 테스트 맵에 총성 사운드 지정

  모든 테스트 맵(/Game/OrganicReverb/Maps)의
    - Organic Reverb Volume      → Shot Sounds   (ors.Shot 테스트 총성)
    - Organic Test Gunshot Emitter → Near Sounds (원거리 총기)
  에 GUN_SOUNDS를 넣고 저장한다. 여러 번 실행해도 같은 결과.

총성 웨이브는 저장소에 없다. 아래 GUN_SOUNDS를 프로젝트에 넣은 사운드 경로로 바꿔서 쓴다
(기준: 돌격소총 단발음 모노 원본 6개, 발사할 때마다 무작위).
  - 큐(RifleA_Fire_Cue) 대신 원본을 쓰는 이유: 큐 안의 거리 감쇠 노드가 우리 거리감 계산과 겹칠 수 있음
  - 스테레오(_ST) 대신 모노를 쓰는 이유: 방향(패닝)이 정확하고, 녹음된 공간 울림이 우리 리버브와 겹치지 않음

실행
  에디터:   Output Log 입력창 왼쪽을 Python으로 바꾸고   py "<이 파일 경로>"
  헤드리스: UnrealEditor-Cmd.exe ORS_Unreal.uproject -ExecutePythonScript="<이 파일 경로> --quit" -unattended -nullrhi -nosound
"""
import sys

import unreal

GUN_SOUNDS = ["/Game/MilitaryWeapSilver/Sound/Rifle/Wavs/RifleA_Fire0%d" % i for i in range(1, 7)]
MAP_DIR = "/Game/OrganicReverb/Maps"

assets = unreal.get_editor_subsystem(unreal.EditorAssetSubsystem)
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def log(message):
    unreal.log("[OrganicReverb] " + message)


def main():
    sounds = [assets.load_asset(path) for path in GUN_SOUNDS if assets.does_asset_exist(path)]
    if not sounds:
        raise RuntimeError("gun sounds not found - check GUN_SOUNDS: " + GUN_SOUNDS[0])
    log("gun sounds: %d" % len(sounds))

    map_paths = sorted({path.split(".")[0] for path in assets.list_assets(MAP_DIR, recursive=False, include_folder=False)})
    for map_path in map_paths:
        if not levels.load_level(map_path):
            raise RuntimeError("failed to load " + map_path)

        volumes = emitters = 0
        for actor in actors.get_all_level_actors():
            class_name = actor.get_class().get_name()
            if class_name == "OrganicReverbVolume":
                actor.set_editor_property("shot_sounds", sounds)
                volumes += 1
            elif class_name == "OrganicTestGunshotEmitter":
                actor.set_editor_property("near_sounds", sounds)
                emitters += 1

        if not levels.save_current_level():
            raise RuntimeError("failed to save " + map_path)
        log("%s: volume %d, gunshot emitter %d" % (map_path, volumes, emitters))
    log("done")


try:
    main()
finally:
    if "--quit" in sys.argv:
        unreal.SystemLibrary.quit_editor()
