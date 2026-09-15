// AcousticSpaceSegmenter.h
// ---------------------------------------------------------
// AcousticCore: 점유 격자의 빈 공간을 방(노드)과 개구부(포탈)로 자동 분할해서 FAcousticRoomGraph를 만든다.
//
// 알고리즘 (침식 → 성장 방식, 로보틱스 지도 방 분할 기법을 3D 음향용으로 변형):
//   1) 수평 거리 변환: 공기 칸마다 같은 층에서 가장 가까운 벽까지의 거리(체비셰프, 칸 수)
//      → 바닥/천장 높이와 무관하게 "벽 사이 폭"만 본다 (문은 수평으로 좁은 곳)
//   2) 코어: 거리 > R 인 칸만 남기고 연결 컴포넌트로 묶는다. 폭 2R칸 이하 개구부(문)는 여기서 끊어진다
//   3) 성장: 코어를 R칸만큼 다시 벽까지 넓힌다 (문 칸은 R+1칸 거리라 닿지 않음)
//   4) 잔여: 코어가 닿지 못한 공간(좁은 복도, 작은 방, 문 칸)은 각자 별도 영역
//   5) 분할: MaxRoomLength보다 긴 영역은 축별로 잘라 여러 방으로 (방 내부 균일 밀도 가정 유지)
//   6) 병합: MinRoomVolume보다 작은 조각은 가장 넓게 맞닿은 이웃에 병합 (문 칸이 여기서 흡수됨)
//   7) 측정: 부피 = 칸 수, 표면적/재질 = 벽과 맞닿은 면, 포탈 면적 = 방 사이 경계 면
//
// 알려진 한계: 폭 2R칸보다 넓은 수직 개구부(계단실 등)는 위아래 층을 한 방으로 합칠 수 있다.
// ---------------------------------------------------------
#pragma once

#include "AcousticOccupancyGrid.h"
#include "AcousticRoomGraph.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace Acoustic
{
	struct FSegmentationSettings
	{
		float MaxPortalWidth = 2.f;   // m. 이 폭 이하의 좁은 개구부는 방 경계(문)로 본다
		float MaxRoomLength = 12.f;   // m. 이보다 긴 공간(긴 복도/홀)은 여러 방으로 나눈다. 0 이하면 분할 안 함
		float MinRoomVolume = 2.f;    // m^3. 이보다 작은 조각은 이웃 방에 병합
		FAcousticMaterial BoundaryMaterial = MaterialPresets::OpenAir(); // 격자 바깥 = 열린 공간
	};

	// 분할 결과: 칸 → RoomId. 월드 위치 → 방 조회에 사용 (총성 위치, 리스너 위치)
	class FSpaceSegmentation
	{
	public:
		RoomId GetCellRoom(int X, int Y, int Z) const
		{
			if (X < 0 || Y < 0 || Z < 0 || X >= SizeX || Y >= SizeY || Z >= SizeZ) return InvalidRoomId;
			return CellRooms[(static_cast<size_t>(Z) * SizeY + Y) * SizeX + X];
		}

		// 위치가 벽 속이면 인접 26칸 중 첫 번째 방을 돌려준다 (벽에 붙은 음원 대응)
		RoomId FindRoomAt(const FVec3& WorldPos) const
		{
			if (!(CellSize > 0.f)) return InvalidRoomId;
			const int X = static_cast<int>(std::floor((WorldPos.X - Origin.X) / CellSize));
			const int Y = static_cast<int>(std::floor((WorldPos.Y - Origin.Y) / CellSize));
			const int Z = static_cast<int>(std::floor((WorldPos.Z - Origin.Z) / CellSize));

			const RoomId Direct = GetCellRoom(X, Y, Z);
			if (Direct != InvalidRoomId) return Direct;

			for (int DZ = -1; DZ <= 1; ++DZ)
				for (int DY = -1; DY <= 1; ++DY)
					for (int DX = -1; DX <= 1; ++DX)
					{
						const RoomId Near = GetCellRoom(X + DX, Y + DY, Z + DZ);
						if (Near != InvalidRoomId) return Near;
					}
			return InvalidRoomId;
		}

		int SizeX = 0;
		int SizeY = 0;
		int SizeZ = 0;
		float CellSize = 0.f;
		FVec3 Origin;
		std::vector<RoomId> CellRooms;
	};

	namespace SegmenterDetail
	{
		constexpr int32_t NoLabel = -1;
		inline constexpr const auto& Face6 = FaceOffsets; // 격자와 같은 면 순서 (+X, −X, +Y, −Y, +Z, −Z)

		struct FCoord { int X, Y, Z; };

		inline FCoord ToCoord(const FOccupancyGrid& G, size_t I)
		{
			const size_t Layer = static_cast<size_t>(G.SizeX) * G.SizeY;
			return { static_cast<int>(I % G.SizeX), static_cast<int>((I % Layer) / G.SizeX), static_cast<int>(I / Layer) };
		}

		// 같은 층(XY)에서 가장 가까운 고체 또는 격자 옆면 경계까지의 체비셰프 거리 (칸 수, 고체 = 0)
		inline std::vector<uint16_t> ComputeHorizontalDistance(const FOccupancyGrid& G)
		{
			constexpr uint16_t Unset = std::numeric_limits<uint16_t>::max();
			const size_t N = G.NumCells();
			std::vector<uint16_t> Dist(N, Unset);
			std::vector<size_t> Queue;
			Queue.reserve(N);

			// BFS 큐가 거리 오름차순이 되도록 고체(0) 먼저, 격자 옆면 공기(1) 나중
			for (size_t I = 0; I < N; ++I)
			{
				if (!G.IsAir(I)) { Dist[I] = 0; Queue.push_back(I); }
			}
			for (size_t I = 0; I < N; ++I)
			{
				if (Dist[I] != Unset) continue;
				const FCoord C = ToCoord(G, I);
				if (C.X == 0 || C.Y == 0 || C.X == G.SizeX - 1 || C.Y == G.SizeY - 1) { Dist[I] = 1; Queue.push_back(I); }
			}

			for (size_t Head = 0; Head < Queue.size(); ++Head)
			{
				const size_t I = Queue[Head];
				const FCoord C = ToCoord(G, I);
				for (int DY = -1; DY <= 1; ++DY)
					for (int DX = -1; DX <= 1; ++DX)
					{
						if (!G.IsInside(C.X + DX, C.Y + DY, C.Z) || (DX == 0 && DY == 0)) continue;
						const size_t J = G.Index(C.X + DX, C.Y + DY, C.Z);
						if (Dist[J] != Unset) continue;
						Dist[J] = static_cast<uint16_t>(Dist[I] + 1);
						Queue.push_back(J);
					}
			}
			return Dist;
		}

		// Keys[I] >= 0 인 칸을 6-이웃 + 같은 키 기준으로 연결 컴포넌트 라벨링. 다음 라벨 번호 반환.
		inline int32_t LabelByKey(const FOccupancyGrid& G, const std::vector<int64_t>& Keys, std::vector<int32_t>& Labels, int32_t NextLabel)
		{
			const size_t N = G.NumCells();
			std::vector<uint8_t> Visited(N, 0);
			std::vector<size_t> Stack;

			for (size_t Seed = 0; Seed < N; ++Seed)
			{
				if (Keys[Seed] < 0 || Visited[Seed]) continue;

				const int32_t Label = NextLabel++;
				Visited[Seed] = 1;
				Stack.push_back(Seed);
				while (!Stack.empty())
				{
					const size_t I = Stack.back();
					Stack.pop_back();
					Labels[I] = Label;

					const FCoord C = ToCoord(G, I);
					for (const auto& D : Face6)
					{
						if (!G.IsInside(C.X + D[0], C.Y + D[1], C.Z + D[2])) continue;
						const size_t J = G.Index(C.X + D[0], C.Y + D[1], C.Z + D[2]);
						if (Visited[J] || Keys[J] != Keys[I]) continue;
						Visited[J] = 1;
						Stack.push_back(J);
					}
				}
			}
			return NextLabel;
		}

		// 라벨된 칸에서 26-이웃으로 MaxDepth칸까지 빈 공기 칸에 라벨을 넓힌다
		inline void GrowLabels(const FOccupancyGrid& G, std::vector<int32_t>& Labels, int MaxDepth)
		{
			const size_t N = G.NumCells();
			std::vector<uint16_t> Depth(N, 0);
			std::vector<size_t> Queue;
			for (size_t I = 0; I < N; ++I)
			{
				if (Labels[I] != NoLabel) Queue.push_back(I);
			}

			for (size_t Head = 0; Head < Queue.size(); ++Head)
			{
				const size_t I = Queue[Head];
				if (Depth[I] >= MaxDepth) continue;

				const FCoord C = ToCoord(G, I);
				for (int DZ = -1; DZ <= 1; ++DZ)
					for (int DY = -1; DY <= 1; ++DY)
						for (int DX = -1; DX <= 1; ++DX)
						{
							if (!G.IsInside(C.X + DX, C.Y + DY, C.Z + DZ)) continue;
							const size_t J = G.Index(C.X + DX, C.Y + DY, C.Z + DZ);
							if (!G.IsAir(J) || Labels[J] != NoLabel) continue;
							Labels[J] = Labels[I];
							Depth[J] = static_cast<uint16_t>(Depth[I] + 1);
							Queue.push_back(J);
						}
			}
		}

		// MaxRoomLength보다 긴 영역을 축별 균등 슬랩으로 자른 뒤 다시 연결 컴포넌트로 라벨링
		inline int32_t SplitOversized(const FOccupancyGrid& G, std::vector<int32_t>& Labels, int32_t NumLabels, float MaxRoomLength)
		{
			if (!(MaxRoomLength > 0.f) || NumLabels == 0) return NumLabels;

			struct FBounds { int Min[3] = { INT32_MAX, INT32_MAX, INT32_MAX }; int Max[3] = { INT32_MIN, INT32_MIN, INT32_MIN }; };
			std::vector<FBounds> Bounds(NumLabels);
			const size_t N = G.NumCells();
			for (size_t I = 0; I < N; ++I)
			{
				if (Labels[I] == NoLabel) continue;
				const FCoord C = ToCoord(G, I);
				const int P[3] = { C.X, C.Y, C.Z };
				FBounds& B = Bounds[Labels[I]];
				for (int A = 0; A < 3; ++A) { B.Min[A] = std::min(B.Min[A], P[A]); B.Max[A] = std::max(B.Max[A], P[A]); }
			}

			const int MaxCells = std::max(1, static_cast<int>(std::floor(MaxRoomLength / G.CellSize)));
			std::vector<int64_t> Keys(N, -1);
			for (size_t I = 0; I < N; ++I)
			{
				const int32_t L = Labels[I];
				if (L == NoLabel) continue;
				const FCoord C = ToCoord(G, I);
				const int P[3] = { C.X, C.Y, C.Z };
				int64_t Key = static_cast<int64_t>(L) << 30;
				for (int A = 0; A < 3; ++A)
				{
					const int Extent = Bounds[L].Max[A] - Bounds[L].Min[A] + 1;
					const int Slabs = std::clamp((Extent + MaxCells - 1) / MaxCells, 1, 1023);
					const int Slab = (P[A] - Bounds[L].Min[A]) * Slabs / Extent;
					Key |= static_cast<int64_t>(Slab) << (20 - 10 * A);
				}
				Keys[I] = Key;
			}
			return LabelByKey(G, Keys, Labels, 0);
		}

		// 작은 영역을 "자기보다 크거나 같은" 이웃 중 접촉면이 가장 넓은 곳에 병합 (순환 병합 방지).
		// 이웃이 없는 작은 영역은 버린다. 마지막에 라벨을 0부터 연속으로 압축.
		inline int32_t MergeSmallRegions(const FOccupancyGrid& G, std::vector<int32_t>& Labels, int32_t NumLabels, size_t MinCells)
		{
			const size_t N = G.NumCells();
			for (int Pass = 0; Pass < 4; ++Pass)
			{
				std::vector<size_t> Count(NumLabels, 0);
				for (size_t I = 0; I < N; ++I)
				{
					if (Labels[I] != NoLabel) ++Count[Labels[I]];
				}
				auto IsSmall = [&](int32_t L) { return Count[L] > 0 && Count[L] < MinCells; };

				std::vector<std::unordered_map<int32_t, size_t>> Contacts(NumLabels);
				for (size_t I = 0; I < N; ++I)
				{
					const int32_t L = Labels[I];
					if (L == NoLabel || !IsSmall(L)) continue;
					const FCoord C = ToCoord(G, I);
					for (const auto& D : Face6)
					{
						if (!G.IsInside(C.X + D[0], C.Y + D[1], C.Z + D[2])) continue;
						const int32_t M = Labels[G.Index(C.X + D[0], C.Y + D[1], C.Z + D[2])];
						if (M != NoLabel && M != L) ++Contacts[L][M];
					}
				}

				std::vector<int32_t> Target(NumLabels);
				bool bChanged = false;
				for (int32_t L = 0; L < NumLabels; ++L)
				{
					Target[L] = L;
					if (!IsSmall(L)) continue;
					if (Contacts[L].empty()) { Target[L] = NoLabel; bChanged = true; continue; }

					int32_t Best = NoLabel;
					size_t BestFaces = 0;
					for (const auto& [M, Faces] : Contacts[L])
					{
						const bool bBiggerOrEqual = Count[M] > Count[L] || (Count[M] == Count[L] && M < L);
						if (bBiggerOrEqual && (Faces > BestFaces || (Faces == BestFaces && M < Best))) { Best = M; BestFaces = Faces; }
					}
					if (Best != NoLabel) { Target[L] = Best; bChanged = true; }
				}
				if (!bChanged) break;

				auto Resolve = [&](int32_t L)
				{
					while (L != NoLabel && Target[L] != L) L = Target[L];
					return L;
				};
				for (size_t I = 0; I < N; ++I)
				{
					if (Labels[I] != NoLabel) Labels[I] = Resolve(Labels[I]);
				}
			}

			std::vector<int32_t> Remap(NumLabels, NoLabel);
			int32_t Next = 0;
			for (size_t I = 0; I < N; ++I)
			{
				const int32_t L = Labels[I];
				if (L == NoLabel) continue;
				if (Remap[L] == NoLabel) Remap[L] = Next++;
				Labels[I] = Remap[L];
			}
			return Next;
		}

		// 최종 방마다 칸이 가장 많이 속한 "분할 전 영역"을 그룹으로 정하고, 그룹 번호를 0부터 압축.
		// 분할된 복도/홀 조각은 같은 그룹, 병합된 문 칸은 다수결에 묻혀 흡수한 방의 그룹을 따른다.
		inline std::vector<uint32_t> AssignGroups(const std::vector<int32_t>& Labels, const std::vector<int32_t>& PreSplitLabels, int32_t NumLabels)
		{
			std::vector<std::unordered_map<int32_t, size_t>> Votes(NumLabels);
			for (size_t I = 0; I < Labels.size(); ++I)
			{
				if (Labels[I] != NoLabel && PreSplitLabels[I] != NoLabel) ++Votes[Labels[I]][PreSplitLabels[I]];
			}

			std::unordered_map<int32_t, uint32_t> Compact;
			std::vector<uint32_t> Groups(NumLabels, 0);
			for (int32_t L = 0; L < NumLabels; ++L)
			{
				int32_t Best = NoLabel;
				size_t BestCount = 0;
				for (const auto& [Region, Count] : Votes[L])
				{
					if (Count > BestCount || (Count == BestCount && Region < Best)) { Best = Region; BestCount = Count; }
				}
				auto It = Compact.find(Best);
				if (It == Compact.end()) It = Compact.emplace(Best, static_cast<uint32_t>(Compact.size())).first;
				Groups[L] = It->second;
			}
			return Groups;
		}

		inline void BuildGraph(const FOccupancyGrid& G, const std::vector<int32_t>& Labels, int32_t NumLabels,
			const std::vector<uint32_t>& Groups, const FSegmentationSettings& Settings, FAcousticRoomGraph& OutGraph)
		{
			struct FRoomAccum { size_t Cells = 0; double Sum[3] = { 0, 0, 0 }; size_t SurfaceFaces = 0; FBandValues AbsorptionSum; };
			struct FPortalAccum { size_t Faces = 0; double Sum[3] = { 0, 0, 0 }; };

			std::vector<FRoomAccum> Rooms(NumLabels);
			std::unordered_map<uint64_t, FPortalAccum> Portals;
			const float Half = 0.5f * G.CellSize;

			const size_t N = G.NumCells();
			for (size_t I = 0; I < N; ++I)
			{
				const int32_t L = Labels[I];
				if (L == NoLabel) continue;

				const FCoord C = ToCoord(G, I);
				const FVec3 Center = G.CellCenter(C.X, C.Y, C.Z);
				FRoomAccum& R = Rooms[L];
				++R.Cells;
				R.Sum[0] += Center.X; R.Sum[1] += Center.Y; R.Sum[2] += Center.Z;

				for (int Face = 0; Face < 6; ++Face)
				{
					const int* D = Face6[Face];
					const FAcousticMaterial* Surface = nullptr;
					if (!G.IsInside(C.X + D[0], C.Y + D[1], C.Z + D[2]))
					{
						Surface = &Settings.BoundaryMaterial;
					}
					else
					{
						const size_t J = G.Index(C.X + D[0], C.Y + D[1], C.Z + D[2]);
						if (!G.IsAir(J))
						{
							Surface = &G.GetMaterial(G.GetSurfaceValue(J, Face ^ 1)); // 벽 칸에서 이 방을 바라보는 면
						}
						else
						{
							const int32_t M = Labels[J];
							if (M > L) // 방 쌍마다 한 번만 센다
							{
								FPortalAccum& P = Portals[(static_cast<uint64_t>(L) << 32) | static_cast<uint32_t>(M)];
								++P.Faces;
								P.Sum[0] += Center.X + D[0] * Half; P.Sum[1] += Center.Y + D[1] * Half; P.Sum[2] += Center.Z + D[2] * Half;
							}
							continue;
						}
					}

					++R.SurfaceFaces;
					for (int B = 0; B < NumBands; ++B) R.AbsorptionSum[B] += Surface->Absorption[B];
				}
			}

			const float FaceArea = G.CellSize * G.CellSize;
			const float CellVolume = FaceArea * G.CellSize;
			for (int32_t L = 0; L < NumLabels; ++L)
			{
				const FRoomAccum& R = Rooms[L];
				FRoom Room;
				Room.Name = "Room_" + std::to_string(L);
				Room.Center = FVec3(static_cast<float>(R.Sum[0] / R.Cells), static_cast<float>(R.Sum[1] / R.Cells), static_cast<float>(R.Sum[2] / R.Cells));
				Room.Volume = R.Cells * CellVolume;
				Room.SurfaceArea = R.SurfaceFaces * FaceArea;
				Room.Material.Name = "Mixed"; // 면적 가중 평균
				Room.GroupId = Groups[L];
				for (int B = 0; B < NumBands; ++B)
				{
					Room.Material.Absorption[B] = R.SurfaceFaces > 0 ? R.AbsorptionSum[B] / R.SurfaceFaces : 0.f;
				}
				OutGraph.AddRoom(Room);
			}

			std::vector<std::pair<uint64_t, FPortalAccum>> Sorted(Portals.begin(), Portals.end());
			std::sort(Sorted.begin(), Sorted.end(), [](const auto& A, const auto& B) { return A.first < B.first; });
			for (const auto& [Key, P] : Sorted)
			{
				const PortalId Id = OutGraph.AddPortal(static_cast<RoomId>(Key >> 32), static_cast<RoomId>(Key & 0xFFFFFFFFu), P.Faces * FaceArea);
				if (FPortal* Portal = OutGraph.GetPortal(Id))
				{
					Portal->Center = FVec3(static_cast<float>(P.Sum[0] / P.Faces), static_cast<float>(P.Sum[1] / P.Faces), static_cast<float>(P.Sum[2] / P.Faces));
				}
			}
		}
	}

	// 점유 격자 → 방 그래프. OutGraph는 비우고 새로 채운다.
	inline FSpaceSegmentation SegmentSpace(const FOccupancyGrid& Grid, FAcousticRoomGraph& OutGraph,
		const FSegmentationSettings& Settings = FSegmentationSettings())
	{
		using namespace SegmenterDetail;

		FSpaceSegmentation Result;
		Result.SizeX = Grid.SizeX;
		Result.SizeY = Grid.SizeY;
		Result.SizeZ = Grid.SizeZ;
		Result.CellSize = Grid.CellSize;
		Result.Origin = Grid.Origin;

		OutGraph.Clear();
		const size_t N = Grid.NumCells();
		Result.CellRooms.assign(N, InvalidRoomId);
		if (N == 0 || !(Grid.CellSize > 0.f) || Grid.Cells.size() != N) return Result;

		// 1~2) 폭 2R칸 이하 개구부가 끊어지도록 거리 > R 인 칸만 코어로
		const int R = std::max(1, static_cast<int>(std::lround(Settings.MaxPortalWidth / (2.f * Grid.CellSize))));
		const std::vector<uint16_t> Dist = ComputeHorizontalDistance(Grid);

		std::vector<int64_t> Keys(N, -1);
		for (size_t I = 0; I < N; ++I)
		{
			if (Grid.IsAir(I) && Dist[I] > R) Keys[I] = 0;
		}
		std::vector<int32_t> Labels(N, NoLabel);
		int32_t NumLabels = LabelByKey(Grid, Keys, Labels, 0);

		// 3) 코어를 벽까지 되돌려 성장
		GrowLabels(Grid, Labels, R);

		// 4) 코어가 닿지 못한 공기 칸 → 별도 영역
		for (size_t I = 0; I < N; ++I)
		{
			Keys[I] = (Grid.IsAir(I) && Labels[I] == NoLabel) ? 0 : -1;
		}
		NumLabels = LabelByKey(Grid, Keys, Labels, NumLabels);

		// 5) 긴 공간 분할. 분할 전 영역 번호를 칸마다 기억해 두었다가 그룹(GroupId)으로 쓴다
		const std::vector<int32_t> PreSplitLabels = Labels;
		NumLabels = SplitOversized(Grid, Labels, NumLabels, Settings.MaxRoomLength);

		// 6) 작은 조각 병합
		const float CellVolume = Grid.CellSize * Grid.CellSize * Grid.CellSize;
		const size_t MinCells = static_cast<size_t>(std::ceil(std::max(0.f, Settings.MinRoomVolume) / CellVolume));
		NumLabels = MergeSmallRegions(Grid, Labels, NumLabels, MinCells);

		// 7) 측정 → 그래프
		const std::vector<uint32_t> Groups = AssignGroups(Labels, PreSplitLabels, NumLabels);
		BuildGraph(Grid, Labels, NumLabels, Groups, Settings, OutGraph);
		for (size_t I = 0; I < N; ++I)
		{
			if (Labels[I] != NoLabel) Result.CellRooms[I] = static_cast<RoomId>(Labels[I]);
		}
		return Result;
	}
}
