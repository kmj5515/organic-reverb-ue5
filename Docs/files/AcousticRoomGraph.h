// AcousticRoomGraph.h
// ---------------------------------------------------------
// Core 레이어: 레벨을 "방 = 노드, 개구부(문/복도 연결부) = 엣지"인
// 그래프로 표현한다. 이 파일도 엔진 타입에 의존하지 않는다.
//
// 실제 지오메트리 스캔(레이캐스트 등)은 Unreal 어댑터 쪽에서 수행하고,
// 그 결과만 이 그래프에 채워 넣는다 (관심사 분리).
// ---------------------------------------------------------
#pragma once

#include "AcousticTypes.h"
#include <unordered_map>
#include <vector>

namespace Acoustic
{
	// 방 하나 = 그래프의 노드
	struct FRoom
	{
		RoomId Id = InvalidRoomId;

		float Volume = 0.f;          // m^3, Sabine 공식용
		float SurfaceArea = 0.f;     // m^2, 총 표면적
		FAcousticMaterial DominantMaterial; // 대표 재질 (벽 재질이 섞여있으면 면적 가중평균으로 미리 합성해서 넣어줌)

		// 현재 보유 중인 음향 에너지 (주파수 대역별)
		FBandValues CurrentEnergy;

		// Sabine 공식으로 산출한 RT60 (대역별)
		FBandValues RT60;
	};

	// 방과 방을 잇는 개구부(문, 복도, 창문 등) = 그래프의 엣지
	struct FPortal
	{
		PortalId Id = InvalidPortalId();
		RoomId RoomA = InvalidRoomId;
		RoomId RoomB = InvalidRoomId;

		float OpeningArea = 0.f;   // 개구부 면적 (클수록 에너지가 잘 흘러감)
		float Distance = 0.f;      // 두 방 중심 사이 거리 (감쇠 계산용)

		static PortalId InvalidPortalId() { return 0xFFFFFFFF; }
	};

	// 방 그래프 전체를 소유하고 관리하는 클래스.
	// 엔진 어댑터는 레벨을 스캔한 뒤 이 클래스를 채워 넣기만 하면 된다.
	class FAcousticRoomGraph
	{
	public:
		RoomId AddRoom(const FRoom& InRoom)
		{
			RoomId NewId = static_cast<RoomId>(Rooms.size());
			FRoom RoomCopy = InRoom;
			RoomCopy.Id = NewId;
			Rooms.push_back(RoomCopy);
			AdjacencyList[NewId] = {};
			return NewId;
		}

		PortalId AddPortal(RoomId A, RoomId B, float OpeningArea, float Distance)
		{
			PortalId NewId = static_cast<PortalId>(Portals.size());
			FPortal Portal;
			Portal.Id = NewId;
			Portal.RoomA = A;
			Portal.RoomB = B;
			Portal.OpeningArea = OpeningArea;
			Portal.Distance = Distance;
			Portals.push_back(Portal);

			AdjacencyList[A].push_back(NewId);
			AdjacencyList[B].push_back(NewId);
			return NewId;
		}

		FRoom* GetRoom(RoomId Id)
		{
			if (Id >= Rooms.size()) return nullptr;
			return &Rooms[Id];
		}

		const std::vector<PortalId>& GetPortalsOfRoom(RoomId Id) const
		{
			static const std::vector<PortalId> Empty;
			auto It = AdjacencyList.find(Id);
			return It != AdjacencyList.end() ? It->second : Empty;
		}

		const FPortal* GetPortal(PortalId Id) const
		{
			if (Id >= Portals.size()) return nullptr;
			return &Portals[Id];
		}

		size_t NumRooms() const { return Rooms.size(); }
		std::vector<FRoom>& GetAllRooms() { return Rooms; }

		void Clear()
		{
			Rooms.clear();
			Portals.clear();
			AdjacencyList.clear();
		}

	private:
		std::vector<FRoom> Rooms;
		std::vector<FPortal> Portals;
		std::unordered_map<RoomId, std::vector<PortalId>> AdjacencyList;
	};
}
