// AcousticOccupancyGrid.h
// ---------------------------------------------------------
// AcousticCore: 레벨을 균일한 격자(복셀)로 나눈 점유 정보.
// 엔진 어댑터는 칸마다 "공기 / 고체(재질)"만 채워 넣고, 방 분할은 Core(AcousticSpaceSegmenter)가 한다.
// ---------------------------------------------------------
#pragma once

#include "AcousticTypes.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace Acoustic
{
	// 칸의 6면 방향 (+X, −X, +Y, −Y, +Z, −Z). 반대 면 = Face ^ 1
	inline constexpr int FaceOffsets[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };

	struct FOccupancyGrid
	{
		static constexpr uint8_t Air = 0;

		int SizeX = 0;
		int SizeY = 0;
		int SizeZ = 0;
		float CellSize = 0.5f;               // m
		FVec3 Origin;                        // 칸 (0,0,0)의 최소 코너 월드 좌표 (m)
		std::vector<uint8_t> Cells;          // 0 = 공기, k > 0 = 고체 (재질 = Materials[k - 1])
		std::vector<FAcousticMaterial> Materials;

		// 선택: 고체 칸의 면마다 재질 (NumCells × 6, 0 = 칸 재질 사용). 필요할 때만 할당.
		// 칸 하나에 벽 두 개가 들어가 양쪽 면 마감재가 다를 때 (예: 콘크리트 방과 카펫 방의 공유 벽) 쓴다.
		std::vector<uint8_t> FaceMaterials;

		void Init(int X, int Y, int Z, float InCellSize, const FVec3& InOrigin = FVec3(), uint8_t Fill = Air)
		{
			SizeX = std::max(0, X);
			SizeY = std::max(0, Y);
			SizeZ = std::max(0, Z);
			CellSize = InCellSize;
			Origin = InOrigin;
			Cells.assign(NumCells(), Fill);
			FaceMaterials.clear();
		}

		void SetFaceMaterial(size_t I, int Face, uint8_t Value)
		{
			if (FaceMaterials.empty()) FaceMaterials.assign(NumCells() * 6, Air);
			FaceMaterials[I * 6 + Face] = Value;
		}

		// 고체 칸 I의 Face 방향 면의 재질 값 (면 재질이 없으면 칸 재질)
		uint8_t GetSurfaceValue(size_t I, int Face) const
		{
			if (!FaceMaterials.empty() && FaceMaterials[I * 6 + Face] != Air) return FaceMaterials[I * 6 + Face];
			return Cells[I];
		}

		// 재질 등록 → 이 재질로 칸을 채울 때 쓸 값(1~255) 반환. 가득 차면 Air(0) 반환.
		uint8_t AddMaterial(const FAcousticMaterial& Material)
		{
			if (Materials.size() >= 255) return Air;
			Materials.push_back(Material);
			return static_cast<uint8_t>(Materials.size());
		}

		const FAcousticMaterial& GetMaterial(uint8_t Value) const
		{
			static const FAcousticMaterial Fallback{ "Default", FBandValues::Uniform(0.1f) };
			return (Value >= 1 && Value <= Materials.size()) ? Materials[Value - 1] : Fallback;
		}

		size_t NumCells() const { return static_cast<size_t>(SizeX) * SizeY * SizeZ; }
		bool IsInside(int X, int Y, int Z) const { return X >= 0 && Y >= 0 && Z >= 0 && X < SizeX && Y < SizeY && Z < SizeZ; }
		size_t Index(int X, int Y, int Z) const { return (static_cast<size_t>(Z) * SizeY + Y) * SizeX + X; }
		bool IsAir(size_t I) const { return Cells[I] == Air; }

		// [Min, Max) 범위의 칸을 Value로 채운다 (격자 밖은 잘라냄)
		void FillBox(int X0, int Y0, int Z0, int X1, int Y1, int Z1, uint8_t Value)
		{
			X0 = std::max(X0, 0); Y0 = std::max(Y0, 0); Z0 = std::max(Z0, 0);
			X1 = std::min(X1, SizeX); Y1 = std::min(Y1, SizeY); Z1 = std::min(Z1, SizeZ);
			for (int Z = Z0; Z < Z1; ++Z)
				for (int Y = Y0; Y < Y1; ++Y)
					for (int X = X0; X < X1; ++X)
						Cells[Index(X, Y, Z)] = Value;
		}

		FVec3 CellCenter(int X, int Y, int Z) const
		{
			return FVec3(Origin.X + (X + 0.5f) * CellSize, Origin.Y + (Y + 0.5f) * CellSize, Origin.Z + (Z + 0.5f) * CellSize);
		}
	};
}
