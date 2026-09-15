"""
Organic Reverb — 음향 에너지 히트맵 머티리얼 생성 (6-3)

만드는 것
  /Game/OrganicReverb/Debug/M_AcousticHeatmap   에너지 볼륨을 레이마칭해서 그리는 오버레이 머티리얼

방 단위로 솔리드 박스를 그리던 예전 디버그 표시를 대체한다. 서브시스템이 방별 에너지를
아틀라스 텍스처(Z 슬라이스를 타일로 펼친 2D 텍스처)에 채워 넣으면, 스캔 범위를 덮는 디퍼드 데칼이
화면에 보이는 모든 표면을 "그 지점의 에너지" 색으로 물들인다.
→ 바닥·벽이 에너지 색으로 칠해지고, 방 경계에서는 텍스처 보간 덕분에 색이 부드럽게 이어진다.

데칼을 쓰는 이유: 화면에 실제로 보이는 표면의 월드 좌표를 그대로 받을 수 있어서(AbsoluteWorldPosition)
포스트 프로세스에서 씬 깊이로 월드 좌표를 되짚는 과정이 필요 없다. 엔진 버전에 따라 달라지는
내부 심볼에 의존하지 않고, 1인칭 시점에서도 복도를 따라 번지는 색이 그대로 보인다.

실행
  에디터:   py "<이 파일 경로>"
  헤드리스: UnrealEditor-Cmd.exe ORS_Unreal.uproject -ExecutePythonScript="<이 파일 경로> --quit" -unattended -nullrhi -nosound
"""
import sys

import unreal

PACKAGE_PATH = "/Game/OrganicReverb/Debug"
ASSET_NAME = "M_AcousticHeatmap"
ASSET_FULL = PACKAGE_PATH + "/" + ASSET_NAME

# 그래프에서 넘겨받는 입력만 쓰므로 엔진 버전에 따라 달라지는 내부 심볼에 의존하지 않는다.
#   WorldPos    데칼이 칠하는 표면의 월드 좌표 (cm)
#   BoundsMin   스캔 범위 최소 모서리 (cm), BoundsSize 크기 (cm)
#   Lattice     격자 해상도 (x, y, z), Tiles 아틀라스 타일 수 (x, y)
# Z 방향은 두 슬라이스를 섞어서, XY는 텍스처 이중선형 보간으로 부드럽게 만든다.
HLSL = """
float3 L = (WorldPos - BoundsMin) / BoundsSize;
if (any(L < 0.0f) || any(L > 1.0f)) return float4(0, 0, 0, 0);

float2 Inset = 0.5f / Lattice.xy;
float2 XY = lerp(Inset, 1.0f - Inset, saturate(L.xy));

// Z 슬라이스 두 장을 섞는다 (타일 아틀라스라 3D 보간이 자동으로 되지 않는다)
float Zf = clamp(L.z * Lattice.z - 0.5f, 0.0f, Lattice.z - 1.0f);
float Z0 = floor(Zf);
float Z1 = min(Z0 + 1.0f, Lattice.z - 1.0f);
float ZFrac = Zf - Z0;

float2 Tile0 = float2(fmod(Z0, Tiles.x), floor(Z0 / Tiles.x));
float2 Tile1 = float2(fmod(Z1, Tiles.x), floor(Z1 / Tiles.x));
float3 C0 = Texture2DSample(LatticeTex, LatticeTexSampler, (Tile0 + XY) / Tiles).rgb;
float3 C1 = Texture2DSample(LatticeTex, LatticeTexSampler, (Tile1 + XY) / Tiles).rgb;

float3 Color = lerp(C0, C1, ZFrac);
// 알파 = 얼마나 칠할지. 에너지가 0인 방은 아예 칠하지 않아 원래 레벨 색이 그대로 보이고,
// 최대로 칠해도 레벨 형태가 비치도록 남겨 둔다
float Alpha = saturate(max(Color.r, max(Color.g, Color.b))) * 0.65f;
return float4(Color * Intensity, Alpha);
"""

INPUTS = ["WorldPos", "BoundsMin", "BoundsSize", "Lattice", "Tiles", "LatticeTex", "Intensity"]


def log(message):
    unreal.log("[heatmap] " + message)


def make_material():
    if unreal.EditorAssetLibrary.does_asset_exist(ASSET_FULL):
        if "--force" not in sys.argv:
            log("이미 있음: " + ASSET_FULL + " (다시 만들려면 --force)")
            return unreal.EditorAssetLibrary.load_asset(ASSET_FULL)
        unreal.EditorAssetLibrary.delete_asset(ASSET_FULL)
        log("기존 에셋 지우고 다시 만듦")

    tools = unreal.AssetToolsHelpers.get_asset_tools()
    material = tools.create_asset(ASSET_NAME, PACKAGE_PATH, unreal.Material, unreal.MaterialFactoryNew())
    # 데칼 + Unlit → Emissive를 Opacity만큼 표면 위에 얹는다 (조명과 무관하게 같은 색으로 보인다).
    # 데칼 도메인은 블렌드 모드가 제한된다. Opaque(기본값)로 두면 컴파일이 실패하고
    # 엔진 기본 머티리얼로 대체되므로(로그: "Failed to compile Material ... Default Material will be used")
    # 반드시 허용된 값으로 바꿔야 한다. DecalBlendMode 쪽은 Python에서 잠겨 있어 기본값을 쓴다.
    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_DEFERRED_DECAL)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)

    lib = unreal.MaterialEditingLibrary

    custom = lib.create_material_expression(material, unreal.MaterialExpressionCustom, -500, 0)
    custom.set_editor_property("code", HLSL)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    custom.set_editor_property("description", "AcousticHeatmapRaymarch")

    custom_inputs = []
    for name in INPUTS:
        entry = unreal.CustomInput()
        entry.set_editor_property("input_name", name)
        custom_inputs.append(entry)
    custom.set_editor_property("inputs", custom_inputs)

    def connect(expression, input_name, output_name=""):
        lib.connect_material_expressions(expression, output_name, custom, input_name)

    y = -400
    world_pos = lib.create_material_expression(material, unreal.MaterialExpressionWorldPosition, -900, y)
    connect(world_pos, "WorldPos")

    # 서브시스템이 매 틱 채워 주는 파라미터들
    for param_name, default in (
        ("BoundsMin", unreal.LinearColor(0.0, 0.0, 0.0, 0.0)),
        ("BoundsSize", unreal.LinearColor(1000.0, 1000.0, 1000.0, 0.0)),
        ("Lattice", unreal.LinearColor(32.0, 32.0, 16.0, 0.0)),
        ("Tiles", unreal.LinearColor(4.0, 4.0, 0.0, 0.0)),
    ):
        y += 120
        param = lib.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -900, y)
        param.set_editor_property("parameter_name", param_name)
        param.set_editor_property("default_value", default)
        connect(param, param_name)

    y += 120
    texture = lib.create_material_expression(material, unreal.MaterialExpressionTextureObjectParameter, -900, y)
    texture.set_editor_property("parameter_name", "LatticeTex")
    texture.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    connect(texture, "LatticeTex")

    for param_name, default in (("Intensity", 1.0),):
        y += 120
        scalar = lib.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -900, y)
        scalar.set_editor_property("parameter_name", param_name)
        scalar.set_editor_property("default_value", default)
        connect(scalar, param_name)

    # float4 → RGB는 Emissive, A는 Opacity
    rgb_mask = lib.create_material_expression(material, unreal.MaterialExpressionComponentMask, -250, -60)
    rgb_mask.set_editor_property("r", True)
    rgb_mask.set_editor_property("g", True)
    rgb_mask.set_editor_property("b", True)
    rgb_mask.set_editor_property("a", False)
    lib.connect_material_expressions(custom, "", rgb_mask, "")

    alpha_mask = lib.create_material_expression(material, unreal.MaterialExpressionComponentMask, -250, 60)
    alpha_mask.set_editor_property("r", False)
    alpha_mask.set_editor_property("g", False)
    alpha_mask.set_editor_property("b", False)
    alpha_mask.set_editor_property("a", True)
    lib.connect_material_expressions(custom, "", alpha_mask, "")

    lib.connect_material_property(rgb_mask, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    lib.connect_material_property(alpha_mask, "", unreal.MaterialProperty.MP_OPACITY)

    # 데칼은 알베도도 같이 덮어쓴다. 연결하지 않으면 흰색이 깔려 밝은 레벨에서 화면이 날아가므로
    # 검게 깔고 Emissive만 색으로 보이게 한다 (= 에너지 색 그대로, 조명과 무관)
    black = lib.create_material_expression(material, unreal.MaterialExpressionConstant3Vector, -250, 180)
    black.set_editor_property("constant", unreal.LinearColor(0.0, 0.0, 0.0, 1.0))
    lib.connect_material_property(black, "", unreal.MaterialProperty.MP_BASE_COLOR)

    zero = lib.create_material_expression(material, unreal.MaterialExpressionConstant, -250, 260)
    zero.set_editor_property("r", 0.0)
    lib.connect_material_property(zero, "", unreal.MaterialProperty.MP_SPECULAR)

    one = lib.create_material_expression(material, unreal.MaterialExpressionConstant, -250, 320)
    one.set_editor_property("r", 1.0)
    lib.connect_material_property(one, "", unreal.MaterialProperty.MP_ROUGHNESS)
    lib.recompile_material(material)
    unreal.EditorAssetLibrary.save_asset(ASSET_FULL)
    log("만듦: " + ASSET_FULL)
    return material


def main():
    if not unreal.EditorAssetLibrary.does_directory_exist(PACKAGE_PATH):
        unreal.EditorAssetLibrary.make_directory(PACKAGE_PATH)
    make_material()
    if "--quit" in sys.argv:
        unreal.SystemLibrary.quit_editor()


main()
