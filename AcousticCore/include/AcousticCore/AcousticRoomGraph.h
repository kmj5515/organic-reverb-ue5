// AcousticRoomGraph.h
// ---------------------------------------------------------
// AcousticCore: 레벨을 "방 = 노드, 개구부(문/복도 연결부) = 엣지" 그래프로 표현한다.
//
// 지오메트리 스캔(레이캐스트 등)은 엔진 어댑터가 수행하고, 결과만 이 그래프에 채워 넣는다.
// ---------------------------------------------------------
#pragma once

#include "AcousticTypes.h"
#include <vector>

namespace Acoustic
{
	// 방 하나 = 그래프의 노드
	struct FRoom
	{
		RoomId Id = InvalidRoomId;
		std::string Name;             // 디버그 표시용
		FVec3 Center;                 // 디버그 표시/위치 조회용

		float Volume = 0.f;           // m^3
		float SurfaceArea = 0.f;      // m^2, 벽/천장/바닥 총면적 (포탈 개구부는 제외 — 포탈은 FPortal이 따로 처리)
		FAcousticMaterial Material;   // 벽 재질이 섞여 있으면 어댑터가 면적 가중평균으로 합성해서 넣는다

		// 현재 보유 음향 에너지 (대역별). 에너지 밀도 = Energy / Volume
		FBandValues Energy;

		// 분할 전 같은 공간(긴 복도, 큰 홀)에 속한 방끼리 같은 값. 공간을 여러 방으로 나눠도
		// 리버브 길이는 공간 전체로 계산하기 위해 쓴다 (ReverbParameterMapper). 비워 두면 AddRoom이 자기 Id로 채운다.
		uint32_t GroupId = InvalidRoomId;
	};

	// 방과 방을 잇는 개구부(문, 복도 단면, 창문 등) = 그래프의 엣지
	struct FPortal
	{
		PortalId Id = InvalidPortalId;
		RoomId RoomA = InvalidRoomId;
		RoomId RoomB = InvalidRoomId;

		float OpeningArea = 0.f;      // m^2, 개구부 면적
		float Transmission = 1.f;     // 0 = 완전히 닫힌 문, 1 = 완전 개방 (게임플레이에서 문 열림 정도로 제어)
		FVec3 Center;                 // 디버그 표시용
	};

	// 박스형 방 생성 헬퍼 (6방향 레이 프로브 결과가 곧 박스 치수이므로 어댑터에서도 사용)
	inline FRoom MakeBoxRoom(const std::string& Name, const FVec3& Center, float SizeX, float SizeY, float SizeZ, const FAcousticMaterial& Material)
	{
		FRoom Room;
		Room.Name = Name;
		Room.Center = Center;
		Room.Volume = SizeX * SizeY * SizeZ;
		Room.SurfaceArea = 2.f * (SizeX * SizeY + SizeY * SizeZ + SizeZ * SizeX);
		Room.Material = Material;
		return Room;
	}

	// 방 그래프 전체를 소유/관리. 엔진 어댑터는 레벨을 스캔한 뒤 이 클래스를 채워 넣기만 하면 된다.
	class FAcousticRoomGraph
	{
	public:
		// 부피가 0 이하인 방은 거부 (InvalidRoomId 반환)
		RoomId AddRoom(const FRoom& InRoom)
		{
			if (!(InRoom.Volume > 0.f)) return InvalidRoomId;

			const RoomId NewId = static_cast<RoomId>(Rooms.size());
			Rooms.push_back(InRoom);
			Rooms.back().Id = NewId;
			if (Rooms.back().GroupId == InvalidRoomId) Rooms.back().GroupId = NewId;
			Adjacency.emplace_back();
			return NewId;
		}

		// 존재하지 않는 방, 자기 자신 연결, 면적 0 이하는 거부 (InvalidPortalId 반환)
		PortalId AddPortal(RoomId A, RoomId B, float OpeningArea, float Transmission = 1.f)
		{
			if (A >= Rooms.size() || B >= Rooms.size() || A == B || !(OpeningArea > 0.f)) return InvalidPortalId;

			FPortal Portal;
			Portal.Id = static_cast<PortalId>(Portals.size());
			Portal.RoomA = A;
			Portal.RoomB = B;
			Portal.OpeningArea = OpeningArea;
			Portal.Transmission = Transmission;
			// 위치를 따로 주지 않으면 두 방의 중간으로 둔다 (전달 지연·디버그 표시가 원점을 가리키지 않도록).
			// 실제 레벨을 스캔할 때는 세그멘터가 개구부의 진짜 위치로 덮어쓴다.
			Portal.Center = (Rooms[A].Center + Rooms[B].Center) * 0.5f;
			Portals.push_back(Portal);

			Adjacency[A].push_back(Portal.Id);
			Adjacency[B].push_back(Portal.Id);
			return Portal.Id;
		}

		FRoom* GetRoom(RoomId Id) { return Id < Rooms.size() ? &Rooms[Id] : nullptr; }
		const FRoom* GetRoom(RoomId Id) const { return Id < Rooms.size() ? &Rooms[Id] : nullptr; }

		FPortal* GetPortal(PortalId Id) { return Id < Portals.size() ? &Portals[Id] : nullptr; }
		const FPortal* GetPortal(PortalId Id) const { return Id < Portals.size() ? &Portals[Id] : nullptr; }

		const std::vector<PortalId>& GetPortalsOfRoom(RoomId Id) const
		{
			static const std::vector<PortalId> Empty;
			return Id < Adjacency.size() ? Adjacency[Id] : Empty;
		}

		std::vector<FRoom>& GetRooms() { return Rooms; }
		const std::vector<FRoom>& GetRooms() const { return Rooms; }
		const std::vector<FPortal>& GetPortals() const { return Portals; }

		size_t NumRooms() const { return Rooms.size(); }
		size_t NumPortals() const { return Portals.size(); }

		void Clear()
		{
			Rooms.clear();
			Portals.clear();
			Adjacency.clear();
		}

	private:
		std::vector<FRoom> Rooms;
		std::vector<FPortal> Portals;
		std::vector<std::vector<PortalId>> Adjacency; // RoomId가 곧 인덱스
	};
}
