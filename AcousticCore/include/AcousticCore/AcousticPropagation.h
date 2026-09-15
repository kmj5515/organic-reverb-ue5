// AcousticPropagation.h
// ---------------------------------------------------------
// AcousticCore: 음원 → 리스너 전파를 음원마다 계산한다 (5단계).
//
// 1) 직접음 경로 — Obstruction / Occlusion
//    같은 방이거나 열린 문 너머로 직선으로 보이면 손실 없음.
//    아니면 문(포탈)을 거치는 최단 경로를 찾는다 (Dijkstra, 노드 = 문 중심).
//    찾은 꺾은선은 격자 가시선으로 "당겨서"(string pulling) 펴 준다 — 긴 공간을 MaxRoomLength로
//    쪼갤 때 생긴 가상 경계는 실제 벽이 아니므로 그 중심을 지날 이유가 없다 (6-2).
//      - 거리 손실: 직선 대신 돌아가는 만큼 멀어짐 → 20·log10(d_direct / d_path)
//      - 회절 손실: 문에서 꺾이는 각도 θ에 비례, 고음일수록 큼 (θ/π · MaxDiffractionLossDb)
//      - 문 투과율 τ: 경로 위 문들의 τ 곱 (반쯤 닫힌 문)
//    여기에 직선 경로가 지나는 벽 수만큼의 벽 투과음을 에너지로 더한다.
//    열린 경로가 없으면(문이 모두 닫힘) 벽 투과음만 남는다 → Occlusion.
// 2) 잔향 결합 — Exclusion
//    음원 방에 연속 출력을 넣었을 때 리스너 방에 쌓이는 정상 상태 에너지 밀도 비 w_L / w_S.
//    같은 방 = 0 dB, 문 너머로 멀어질수록 작아진다 → 음원별 리버브 Send 양.
//    시뮬레이터와 같은 물리(흡음 c·S·α/4, 문 c·S·τ/4)의 정상 상태를 켤레 기울기법으로 푼다.
// ---------------------------------------------------------
#pragma once

#include "AcousticSpaceSegmenter.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <vector>

namespace Acoustic
{
	struct FPropagationSettings
	{
		// 문에서 180° 꺾일 때의 대역별 회절 손실 (각도에 비례)
		FBandValues MaxDiffractionLossDb = FBandValues(6.f, 12.f, 20.f);
		// 벽 하나를 뚫고 오는 소리의 대역별 투과 손실
		FBandValues WallTransmissionLossDb = FBandValues(20.f, 35.f, 50.f);
		float MinGainDb = -60.f;

		// 경로 당기기(string pulling): 격자 가시선으로 직접 이어지는 꼭짓점을 건너뛴다.
		// 긴 공간을 MaxRoomLength로 쪼갤 때 생긴 "가상 경계"는 실제 벽이 아니라서 그 중심을 지날 이유가 없다.
		// 끄면 문 중심을 모두 지나는 예전 경로가 된다 (A/B 비교용). 격자(FSpaceSegmentation)가 있어야 동작한다.
		bool bStringPulling = true;
	};

	struct FPropagationPath
	{
		bool bValid = false;        // 음원과 리스너가 모두 방 안에 있어 계산했는가 (아니면 처리하지 않음)
		bool bSameRoom = false;
		bool bLineOfSight = false;  // 다른 방이지만 열린 문 너머로 직선으로 보임
		bool bOccluded = false;     // 열린 문 경로가 없음 (벽 투과음만)
		int WallsBetween = 0;       // 직선 경로가 지나는 벽 수
		std::vector<PortalId> Portals; // 음원 → 리스너 순서 (투과율 τ는 여기 전부가 곱해진다)
		std::vector<FVec3> PathPoints; // 소리가 실제로 지나는 꺾은선: 음원 → (당기고 남은 꼭짓점) → 리스너
		float DirectDistance = 0.f; // m
		float PathLength = 0.f;     // m
		FBandValues GainDb;         // 직접음 추가 감쇠 (0 = 막힘 없음). 거리 감쇠 자체는 엔진이 직선 거리로 처리
		FVec3 ApparentPosition;     // 소리가 들어오는 것처럼 들릴 위치 (리스너 쪽 마지막 문)
	};

	// 직선 A→B가 지나는 벽 수. 양쪽이 공기로 둘러싸인 고체 구간만 센다 (벽에 붙은 음원 대응)
	inline int CountWallsBetween(const FSpaceSegmentation& Seg, const FVec3& A, const FVec3& B)
	{
		if (!(Seg.CellSize > 0.f)) return 1;

		const FVec3 D = B - A;
		const int Steps = std::max(1, static_cast<int>(std::ceil(D.Length() / (0.25f * Seg.CellSize))));
		int Walls = 0;
		bool bSeenAir = false;
		bool bInWall = false;
		for (int I = 0; I <= Steps; ++I)
		{
			const FVec3 P = A + D * (static_cast<float>(I) / static_cast<float>(Steps));
			const int X = static_cast<int>(std::floor((P.X - Seg.Origin.X) / Seg.CellSize));
			const int Y = static_cast<int>(std::floor((P.Y - Seg.Origin.Y) / Seg.CellSize));
			const int Z = static_cast<int>(std::floor((P.Z - Seg.Origin.Z) / Seg.CellSize));
			const bool bInside = X >= 0 && Y >= 0 && Z >= 0 && X < Seg.SizeX && Y < Seg.SizeY && Z < Seg.SizeZ;
			const bool bWall = bInside && Seg.GetCellRoom(X, Y, Z) == InvalidRoomId;
			if (!bWall)
			{
				if (bInWall && bSeenAir) ++Walls;
				bSeenAir = true;
			}
			bInWall = bWall;
		}
		return Walls;
	}

	namespace PropagationDetail
	{
		inline float Distance(const FVec3& A, const FVec3& B) { return (B - A).Length(); }

		// Prev → At → Next 에서 꺾이는 각도 (0 = 직진, π = 되돌아감)
		inline float BendAngle(const FVec3& Prev, const FVec3& At, const FVec3& Next)
		{
			const FVec3 In = At - Prev;
			const FVec3 Out = Next - At;
			const float Denominator = In.Length() * Out.Length();
			if (Denominator <= 1e-6f) return 0.f;
			return std::acos(std::clamp(In.Dot(Out) / Denominator, -1.f, 1.f));
		}

		inline float EnergyToDb(float Energy, float MinDb)
		{
			return Energy > 0.f ? std::clamp(10.f * std::log10(Energy), MinDb, 0.f) : MinDb;
		}

		// 경로 당기기(string pulling): 앞 꼭짓점에서 격자 가시선으로 직접 보이는 가장 먼 꼭짓점까지 건너뛴다.
		// 팽팽한 실을 당기면 불필요한 꺾임이 펴지는 것과 같다. 막히는 순간 멈춰서 벽을 뚫고 건너뛰지 않는다.
		inline std::vector<FVec3> PullTight(const FSpaceSegmentation& Seg, const std::vector<FVec3>& Points)
		{
			std::vector<FVec3> Out;
			Out.reserve(Points.size());
			Out.push_back(Points.front());
			for (size_t I = 0; I + 1 < Points.size(); )
			{
				size_t Next = I + 1;
				for (size_t K = I + 2; K < Points.size(); ++K)
				{
					if (CountWallsBetween(Seg, Points[I], Points[K]) != 0) break;
					Next = K;
				}
				Out.push_back(Points[Next]);
				I = Next;
			}
			return Out;
		}
	}

	inline FPropagationPath FindPropagationPath(const FAcousticRoomGraph& Graph,
		RoomId SourceRoom, const FVec3& SourcePos, RoomId ListenerRoom, const FVec3& ListenerPos,
		const FPropagationSettings& S = FPropagationSettings(), const FSpaceSegmentation* Seg = nullptr)
	{
		using namespace PropagationDetail;

		FPropagationPath Out;
		Out.DirectDistance = std::max(0.1f, Distance(SourcePos, ListenerPos));
		Out.PathLength = Out.DirectDistance;
		Out.ApparentPosition = SourcePos;
		if (!Graph.GetRoom(SourceRoom) || !Graph.GetRoom(ListenerRoom)) return Out;

		Out.bValid = true;
		Out.PathPoints = { SourcePos, ListenerPos }; // 같은 방 / 직선 가시 / 벽 투과는 직선. 문을 도는 경우만 아래에서 다시 만든다
		if (SourceRoom == ListenerRoom)
		{
			Out.bSameRoom = true;
			return Out;
		}

		Out.WallsBetween = Seg ? CountWallsBetween(*Seg, SourcePos, ListenerPos) : 1;
		if (Seg && Out.WallsBetween == 0)
		{
			Out.bLineOfSight = true; // 열린 문 너머로 직접 보임
			return Out;
		}

		// 문 중심을 노드로 한 Dijkstra. 닫힌 문(τ = 0)은 지나갈 수 없다
		constexpr float Infinity = std::numeric_limits<float>::infinity();
		const size_t NumPortals = Graph.NumPortals();
		std::vector<float> Dist(NumPortals, Infinity);
		std::vector<PortalId> Prev(NumPortals, InvalidPortalId);
		using FItem = std::pair<float, PortalId>;
		std::priority_queue<FItem, std::vector<FItem>, std::greater<FItem>> Queue;

		auto IsOpen = [&Graph](PortalId Id) { return Graph.GetPortal(Id)->Transmission > 0.f; };
		for (PortalId P : Graph.GetPortalsOfRoom(SourceRoom))
		{
			if (!IsOpen(P)) continue;
			Dist[P] = Distance(SourcePos, Graph.GetPortal(P)->Center);
			Queue.push({ Dist[P], P });
		}

		float Best = Infinity;
		PortalId BestLast = InvalidPortalId;
		while (!Queue.empty())
		{
			const auto [D, P] = Queue.top();
			Queue.pop();
			if (D > Dist[P] || D >= Best) continue;

			const FPortal& Portal = *Graph.GetPortal(P);
			if (Portal.RoomA == ListenerRoom || Portal.RoomB == ListenerRoom)
			{
				const float Total = D + Distance(Portal.Center, ListenerPos);
				if (Total < Best) { Best = Total; BestLast = P; }
			}

			for (RoomId Room : { Portal.RoomA, Portal.RoomB })
			{
				for (PortalId Q : Graph.GetPortalsOfRoom(Room))
				{
					if (Q == P || !IsOpen(Q)) continue;
					const float ND = D + Distance(Portal.Center, Graph.GetPortal(Q)->Center);
					if (ND < Dist[Q])
					{
						Dist[Q] = ND;
						Prev[Q] = P;
						Queue.push({ ND, Q });
					}
				}
			}
		}

		// 벽 투과음 (문 경로 유무와 상관없이 에너지로 더함)
		const int Walls = std::max(1, Out.WallsBetween);
		FBandValues Energy;
		for (int B = 0; B < NumBands; ++B) Energy[B] = std::pow(10.f, -S.WallTransmissionLossDb[B] * Walls / 10.f);

		if (BestLast == InvalidPortalId)
		{
			Out.bOccluded = true;
			for (int B = 0; B < NumBands; ++B) Out.GainDb[B] = EnergyToDb(Energy[B], S.MinGainDb);
			return Out;
		}

		for (PortalId P = BestLast; P != InvalidPortalId; P = Prev[P]) Out.Portals.push_back(P);
		std::reverse(Out.Portals.begin(), Out.Portals.end());
		Out.ApparentPosition = Graph.GetPortal(BestLast)->Center;

		Out.PathPoints.clear();
		Out.PathPoints.reserve(Out.Portals.size() + 2);
		Out.PathPoints.push_back(SourcePos);
		for (PortalId P : Out.Portals) Out.PathPoints.push_back(Graph.GetPortal(P)->Center);
		Out.PathPoints.push_back(ListenerPos);

		// 문 중심을 모두 지나는 꺾은선을 팽팽하게 당긴다. 분할된 복도의 가상 경계처럼
		// 실제로는 막힌 곳이 아닌 꼭짓점이 펴지면서 가짜 경로 연장과 가짜 회절 손실이 사라진다.
		if (S.bStringPulling && Seg && Out.PathPoints.size() > 2)
		{
			Out.PathPoints = PullTight(*Seg, Out.PathPoints);
			if (Out.PathPoints.size() > 2) Out.ApparentPosition = Out.PathPoints[Out.PathPoints.size() - 2];
		}

		float PathLength = 0.f;
		float BendFraction = 0.f; // Σ θ/π
		for (size_t I = 1; I < Out.PathPoints.size(); ++I)
		{
			PathLength += Distance(Out.PathPoints[I - 1], Out.PathPoints[I]);
			if (I + 1 < Out.PathPoints.size())
			{
				BendFraction += BendAngle(Out.PathPoints[I - 1], Out.PathPoints[I], Out.PathPoints[I + 1]) / 3.14159265f;
			}
		}
		Out.PathLength = std::max(PathLength, Out.DirectDistance);

		// 투과율은 지나온 문 전부의 곱. 당기기로 꼭짓점이 펴져도 그 문을 통과한 사실은 그대로다
		float Openness = 1.f; // Π τ
		for (PortalId P : Out.Portals) Openness *= std::clamp(Graph.GetPortal(P)->Transmission, 0.f, 1.f);

		const float DistanceDb = 20.f * std::log10(Out.DirectDistance / Out.PathLength);
		const float OpennessDb = 10.f * std::log10(Openness);
		for (int B = 0; B < NumBands; ++B)
		{
			const float PathDb = DistanceDb - BendFraction * S.MaxDiffractionLossDb[B] + OpennessDb;
			Energy[B] += std::pow(10.f, PathDb / 10.f);
			Out.GainDb[B] = EnergyToDb(Energy[B], S.MinGainDb);
		}
		return Out;
	}

	// 음원 방 → 리스너 방 잔향 결합 (Mid 대역). 음원 방마다 한 번 풀어 캐시한다.
	// 문 투과율이 바뀌거나 그래프를 다시 만들면 Invalidate() 해야 한다.
	class FReverbCoupling
	{
	public:
		// 공기 흡음계수 m (1/m, Mid 대역). 시뮬레이터(FDiffusionSettings)와 같은 값이어야
		// 음원별 리버브 Send가 실제로 들리는 잔향과 어긋나지 않는다. 바꾼 뒤에는 Invalidate() 필요.
		float AirAbsorptionCoefficient = AirAbsorption::Coefficient().Mid;

		float GetCouplingDb(const FAcousticRoomGraph& Graph, RoomId Source, RoomId Listener, float MinDb = -60.f)
		{
			if (Source == Listener) return 0.f;
			if (!Graph.GetRoom(Source) || !Graph.GetRoom(Listener)) return 0.f;

			auto It = Cache.find(Source);
			if (It == Cache.end()) It = Cache.emplace(Source, SolveSteadyState(Graph, Source, SpeedOfSound, AirAbsorptionCoefficient)).first;
			const std::vector<float>& W = It->second;
			if (Source >= W.size() || Listener >= W.size() || !(W[Source] > 0.f)) return MinDb;

			const float Ratio = W[Listener] / W[Source];
			return Ratio > 0.f ? std::clamp(10.f * std::log10(Ratio), MinDb, 0.f) : MinDb;
		}

		void Invalidate() { Cache.clear(); }

		// Source 방에 단위 출력을 계속 넣었을 때 방마다의 정상 상태 에너지 밀도.
		//   (흡음 컨덕턴스 a_i + Σ G_ij) w_i − Σ G_ij w_j = δ_iS,   a_i = c·S_i·α_i/4 + c·m·V_i,  G_ij = c·S·τ/4
		//   (뒤 항이 공기 흡음. 손실 c·m·E = c·m·V·w 이므로 컨덕턴스로는 c·m·V)
		// 대칭 양의 정부호 행렬이라 켤레 기울기법이 최대 N번 반복 안에 수렴한다.
		static std::vector<float> SolveSteadyState(const FAcousticRoomGraph& Graph, RoomId Source, float C = SpeedOfSound,
			float AirM = AirAbsorption::Coefficient().Mid)
		{
			const size_t N = Graph.NumRooms();
			const double Air = std::max(0.f, AirM);
			std::vector<double> Diag(N, 1e-6); // 흡음이 전혀 없는 공간의 특이성 방지용 아주 작은 값
			for (const FRoom& Room : Graph.GetRooms())
			{
				Diag[Room.Id] += 0.25 * C * Room.SurfaceArea * Room.Material.Absorption.Mid + C * Air * Room.Volume;
			}

			struct FEdge { RoomId A; RoomId B; double G; };
			std::vector<FEdge> Edges;
			for (const FPortal& Portal : Graph.GetPortals())
			{
				const double G = 0.25 * C * Portal.OpeningArea * std::clamp(Portal.Transmission, 0.f, 1.f);
				if (G <= 0.0) continue;
				Edges.push_back({ Portal.RoomA, Portal.RoomB, G });
				Diag[Portal.RoomA] += G;
				Diag[Portal.RoomB] += G;
			}

			auto MatVec = [&](const std::vector<double>& X, std::vector<double>& Y)
			{
				for (size_t I = 0; I < N; ++I) Y[I] = Diag[I] * X[I];
				for (const FEdge& E : Edges)
				{
					Y[E.A] -= E.G * X[E.B];
					Y[E.B] -= E.G * X[E.A];
				}
			};
			auto Dot = [N](const std::vector<double>& A, const std::vector<double>& B)
			{
				double Sum = 0.0;
				for (size_t I = 0; I < N; ++I) Sum += A[I] * B[I];
				return Sum;
			};

			std::vector<double> W(N, 0.0), R(N, 0.0), P(N, 0.0), AP(N, 0.0);
			if (Source >= N) return std::vector<float>(N, 0.f);
			R[Source] = 1.0;
			P = R;
			double RS = 1.0;
			for (size_t Iter = 0; Iter < 4 * N + 50 && RS > 1e-24; ++Iter)
			{
				MatVec(P, AP);
				const double PAP = Dot(P, AP);
				if (!(PAP > 0.0)) break;
				const double Alpha = RS / PAP;
				for (size_t I = 0; I < N; ++I)
				{
					W[I] += Alpha * P[I];
					R[I] -= Alpha * AP[I];
				}
				const double RSNew = Dot(R, R);
				for (size_t I = 0; I < N; ++I) P[I] = R[I] + (RSNew / RS) * P[I];
				RS = RSNew;
			}

			std::vector<float> Result(N);
			for (size_t I = 0; I < N; ++I) Result[I] = static_cast<float>(W[I]);
			return Result;
		}

	private:
		std::unordered_map<RoomId, std::vector<float>> Cache;
	};

	// 엔진 오디오 컴포넌트에 넣을 값
	struct FSourceOutput
	{
		float VolumeMultiplier = 1.f;      // 직접음 진폭 배율
		float LowpassHz = 20000.f;         // 직접음 로우패스 차단 주파수
		float ReverbSendMultiplier = 1.f;  // 리버브 Send 진폭 배율
	};

	struct FSourceOutputSettings
	{
		float MaxLowpassHz = 20000.f;
		float MinLowpassHz = 400.f;
		float FullMuffleDb = 24.f; // 고음이 중음보다 이만큼 더 깎이면 최소 차단 주파수
	};

	inline FSourceOutput MapToSourceOutput(const FPropagationPath& Path, float ReverbCouplingDb,
		const FSourceOutputSettings& S = FSourceOutputSettings())
	{
		FSourceOutput Out;
		Out.VolumeMultiplier = std::pow(10.f, Path.GainDb.Mid / 20.f);

		// 고음이 중음보다 더 깎인 만큼 로우패스를 내린다 (로그 스케일)
		const float HighExtraLossDb = std::max(0.f, Path.GainDb.Mid - Path.GainDb.High);
		const float T = std::clamp(HighExtraLossDb / S.FullMuffleDb, 0.f, 1.f);
		Out.LowpassHz = S.MaxLowpassHz * std::pow(S.MinLowpassHz / S.MaxLowpassHz, T);

		Out.ReverbSendMultiplier = std::pow(10.f, ReverbCouplingDb / 20.f);
		return Out;
	}
}
