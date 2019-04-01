#include <Windows.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include "Hooking.Patterns.h"
#include "Hooking.h"
#include "CScenarioInfo.h"
#include "CScenarioPoint.h"
#include "CScenarioPointRegion.h"
#include "CScenarioInfoManager.h"
#include "CCargen.h"
#include <unordered_map>
#include <MinHook.h>
#include <jitasm.h>
#include <filesystem>

static constexpr bool EnableLogging = true;

using IsScenarioVehicleInfo_fn = bool(*)(uint32_t index);
using CAmbientModelSetsManager_FindIndexByHash_fn = uint32_t(*)(void* mgr, int type, uint32_t hash);
using CScenarioInfoManager_GetScenarioTypeByHash_fn = uint32_t(*)(CScenarioInfoManager* mgr, uint32_t* name, bool a3, bool searchInScenarioTypeGroups);
using CScenarioPoint_CanScenarioSpawn_fn = bool(*)(CScenarioPoint* point, uint32_t scenarioIndex, bool, bool);
static IsScenarioVehicleInfo_fn IsScenarioVehicleInfo;
static CAmbientModelSetsManager_FindIndexByHash_fn CAmbientModelSetsManager_FindIndexByHash;
static CScenarioInfoManager_GetScenarioTypeByHash_fn CScenarioInfoManager_GetScenarioTypeByHash;
static CScenarioPoint_CanScenarioSpawn_fn CScenarioPoint_CanScenarioSpawn;

static void FindGameFunctions()
{
	IsScenarioVehicleInfo = (IsScenarioVehicleInfo_fn)hook::pattern("48 83 EC 28 48 8B 15 ? ? ? ? 0F B7 42 10 3B C8 7D 2A").get(1).get<void>();
	CAmbientModelSetsManager_FindIndexByHash = (CAmbientModelSetsManager_FindIndexByHash_fn)hook::get_pattern("44 89 44 24 ? 48 83 EC 28 48 63 C2 48 8D 14 80");
	CScenarioInfoManager_GetScenarioTypeByHash = (CScenarioInfoManager_GetScenarioTypeByHash_fn)hook::get_pattern("48 8B F9 66 39 59 48 76 1C 8B 02", -0x1C);
	CScenarioPoint_CanScenarioSpawn = (CScenarioPoint_CanScenarioSpawn_fn)hook::get_pattern("48 85 C9 74 06 0F B6 51 16 EB 05", -0x2D);
}

static void** g_AmbientModelSetsMgr;
static CScenarioInfoManager** g_ScenarioInfoMgr;

static void FindGameVariables()
{
	g_AmbientModelSetsMgr = hook::get_address<void**>(hook::get_pattern("48 8B 0D ? ? ? ? E8 ? ? ? ? 83 F8 FF 75 07", 3));
	g_ScenarioInfoMgr = hook::get_address<CScenarioInfoManager**>(hook::get_pattern("8B 42 30 48 8B 0D ? ? ? ? 48 8D 54 24 ? 89 44 24 30", 6));
}

template<int FramesToSkip = 1>
static void LogStackTrace()
{
	void* stack[32];
	USHORT frames = CaptureStackBackTrace(FramesToSkip, 32, stack, NULL);

	spdlog::warn("\tStack Trace:");
	for (int i = 0; i < frames; i++)
	{
		void* address = stack[i];
		HMODULE module = NULL;
		GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module);
		char moduleName[256];
		GetModuleFileName(module, moduleName, 256);

		spdlog::warn("\t\t{:16X} - {}+{:08X}", (uintptr_t)address, std::filesystem::path(moduleName).filename().string().c_str(), ((uintptr_t)address - (uintptr_t)module));
	}
}

struct ExtendedScenarioPoint
{
	uint32_t iType;
	uint32_t ModelSetId;
};
static std::unordered_map<CScenarioPoint*, ExtendedScenarioPoint> g_Points;

static void SavePoint(CScenarioPoint* point, uint32_t scenarioType, uint32_t modelSetId)
{
	g_Points[point] = { scenarioType, modelSetId };
}

static void RemovePoint(CScenarioPoint* point)
{
	if (g_Points.erase(point) == 0)
	{
		spdlog::warn("RemovePoint:: POINT {} NOT FOUND IN MAP ({}, {}, {})",
			(void*)point, point->vPositionAndDirection[0], point->vPositionAndDirection[1], point->vPositionAndDirection[2]);
		LogStackTrace<2>();
	}
}

static uint32_t GetSavedModelSetId(CScenarioPoint* point)
{
	auto p = g_Points.find(point);
	if (p != g_Points.end())
	{
		return p->second.ModelSetId;
	}
	else
	{
		spdlog::warn("GetSavedModelSetId:: POINT {} NOT FOUND IN MAP ({}, {}, {})",
			(void*)point, point->vPositionAndDirection[0], point->vPositionAndDirection[1], point->vPositionAndDirection[2]);
		LogStackTrace<2>();
		return point->ModelSetId;
	}
}

static uint32_t GetSavedScenarioType(CScenarioPoint* point)
{
	auto p = g_Points.find(point);
	if (p != g_Points.end())
	{
		return p->second.iType;
	}
	else
	{
		spdlog::warn("GetSavedScenarioType:: POINT {} NOT FOUND IN MAP ({}, {}, {})",
			(void*)point, point->vPositionAndDirection[0], point->vPositionAndDirection[1],
			point->vPositionAndDirection[2]);
		LogStackTrace<2>();
		return point->iType;
	}
}

static void Patch1()
{
	spdlog::info(__func__);

	// CScenarioPointRegion::LookUps::ConvertHashesToIndices
	hook::put(hook::get_pattern("41 BD ? ? ? ? 85 ED 7E 51 4C 8B F3", 2), 0xFFFFFFFF);
}

static void(*CScenarioPoint_TransformIdsToIndices_orig)(CScenarioPointRegion::sLookUps*, CScenarioPoint*);
static void CScenarioPoint_TransformIdsToIndices_detour(CScenarioPointRegion::sLookUps* indicesLookups, CScenarioPoint* point)
{
	uint32_t scenarioIndex = indicesLookups->TypeNames.Items[point->iType];

	atArray<uint32_t>* modelSetNames = IsScenarioVehicleInfo(scenarioIndex) ?
										&indicesLookups->VehicleModelSetNames :
										&indicesLookups->PedModelSetNames;

	//spdlog::info(" TransformIdsToIndices:: detour -> point:{}, scenarioType:{:08X}, modelSet:{:08X}, modelSetId:{:X}, modelSetNamesCount:{:X}, modelSetNamesSize:{:X}",
	//	(void*)point, scenarioIndex, modelSetNames->Items[point->ModelSetId], point->ModelSetId, modelSetNames->Count, modelSetNames->Size);

	SavePoint(point, scenarioIndex, modelSetNames->Items[point->ModelSetId]);

	CScenarioPoint_TransformIdsToIndices_orig(indicesLookups, point);
}

static void Patch2()
{
	spdlog::info(__func__);

	// CScenarioPoint::TransformIdsToIndices
	MH_CreateHook(hook::get_pattern("48 8B 01 44 0F B6 42 ? 0F B6 72 16", -0xF), CScenarioPoint_TransformIdsToIndices_detour, (void**)&CScenarioPoint_TransformIdsToIndices_orig);
}

static void Patch3()
{
	spdlog::info(__func__);

	// CScenarioInfoManager::IsValidModelSet
	hook::put(hook::get_pattern("81 FF ? ? ? ? 74 6F 48 8B 05", 2), 0xFFFFFFFF);
}

static void Patch4()
{
	spdlog::info(__func__);

	// CScenarioPoint::CanScenarioSpawn
	static struct : jitasm::Frontend
	{
		static int GetModelSetIndex(CScenarioPoint* point)
		{
			if (!point)
			{
				return 0xFFFFFFFF;
			}

			return GetSavedModelSetId(point);
		}

		void InternalMain() override
		{
			push(rcx);
			sub(rsp, 0x10);

			mov(rcx, rdi);
			mov(rax, (uintptr_t)GetModelSetIndex);
			call(rax);

			add(rsp, 0x10);
			pop(rcx);

			mov(edx, eax);
			ret();
		}
	} getModelSetIndexStub;

	auto location = hook::get_pattern("48 85 C9 74 06 0F B6 51 16 EB 05");
	hook::nop(location, 0x10);
	hook::call(location, getModelSetIndexStub.GetCode());
}

static void Patch5()
{
	spdlog::info(__func__);

	// bool GetAndLoadScenarioPointModel(__int64 rcx0, signed int scenarioIndex, CScenarioPoint *point, __int64 a4, ...)
	static struct : jitasm::Frontend
	{
		static int GetModelSetIndex(CScenarioInfo* scenario, CScenarioPoint* point)
		{
			constexpr uint32_t CScenarioVehicleInfo_ClassId = 0xFB9AD9D7;

			if (scenario->GetIsClassId(CScenarioVehicleInfo_ClassId))
			{
				return 0xFFFFFFFF;
			}

			return GetSavedModelSetId(point);
		}

		void InternalMain() override
		{
			push(rcx);
			push(rdx);
			sub(rsp, 0x18);

			mov(rcx, rdi); // first param: CScenarioInfo*
			mov(rdx, r14); // second param: CScenarioPoint*
			mov(rax, (uintptr_t)GetModelSetIndex);
			call(rax);

			add(rsp, 0x18);
			pop(rdx);
			pop(rcx);

			mov(r15d, eax);
			ret();
		}
	} getModelSetIndexStub;

	auto location = hook::get_pattern("48 8B CF 41 BF ? ? ? ? FF 10 84 C0");
	hook::nop(location, 0x14);
	hook::call(location, getModelSetIndexStub.GetCode());

	// cmp against 0xFFFFFFF
	hook::put(hook::get_pattern("41 81 FF ? ? ? ? 0F 85 ? ? ? ? B9", 3), 0xFFFFFFFF);
}

static bool(*CScenarioPoint_SetModelSet_orig)(CScenarioPoint*, uint32_t*, bool);
static bool CScenarioPoint_SetModelSet_detour(CScenarioPoint* _this, uint32_t* modelSetHash, bool isVehicle)
{
	constexpr uint32_t usepopulation_hash = 0xA7548A2;

	bool success = true;
	uint32_t hash = *modelSetHash;
	uint32_t index = 0xFFFFFFFF;
	if (hash != usepopulation_hash)
	{
		index = CAmbientModelSetsManager_FindIndexByHash(*g_AmbientModelSetsMgr, isVehicle ? 2 : 0, hash);
		if (index == 0xFFFFFFFF)
		{
			success = false;
		}
	}

	SavePoint(_this, 0xDEADBEEF, index);
	_this->ModelSetId = index;

	return success;
}

static void(*CScenarioPoint_Delete_orig)(CScenarioPoint*);
static void CScenarioPoint_Delete_detour(CScenarioPoint* _this)
{
	RemovePoint(_this);

	CScenarioPoint_Delete_orig(_this);
}

static void Patch6()
{
	spdlog::info(__func__);

	MH_CreateHook(hook::get_pattern("48 8B 0D ? ? ? ? E8 ? ? ? ? 48 8B CB E8 ? ? ? ? C6 05", -0xC), CScenarioPoint_Delete_detour, (void**)&CScenarioPoint_Delete_orig);
}

static void Patch7()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(rcx);
			sub(rsp, 0x10);

			mov(rcx, rdi); // param: CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedModelSetId);
			call(rax);

			add(rsp, 0x10);
			pop(rcx);

			cmp(eax, 0xFFFFFFFF);
			ret();
		}
	} getModelSetIndexAndCmpStub;
	{
		hook::pattern pattern("0F B6 47 16 3D ? ? ? ? 74 13 8B D0 48 8B 05");
		pattern.count(2);
		pattern.for_each_result([](const hook::pattern_match& match)
		{
			auto location = match.get<void>();
			hook::nop(location, 0x9);
			hook::call(location, getModelSetIndexAndCmpStub.GetCode());
		});
	}

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(r8);
			push(r9);
			push(rax);
			sub(rsp, 0x20);

			mov(rcx, rdi); // param: CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedScenarioType);
			call(rax);
			mov(ebx, eax);

			add(rsp, 0x20);
			pop(rax);
			pop(r9);
			pop(r8);

			movzx(ecx, word_ptr[rax + 0x10]);
			ret();
		}
	} getScenarioTypeStub;
	{
		hook::pattern pattern("0F B6 5F 15 0F B7 48 10 3B D9");
		pattern.count(2);
		pattern.for_each_result([](const hook::pattern_match& match)
		{
			auto location = match.get<void>();
			hook::nop(location, 0x8);
			hook::call_rcx(location, getScenarioTypeStub.GetCode());
		});
	}
}

static uint32_t GetFinalModelSetHash(uint32_t hash)
{
	constexpr uint32_t any_hash = 0xDF3407B5;
	constexpr uint32_t usepopulation_hash = 0xA7548A2;

	return hash == any_hash ? usepopulation_hash : hash;
}

static void Patch8()
{
	spdlog::info(__func__);

	// CScenarioPoint::InitFromSpawnPointDef

	static struct : jitasm::Frontend
	{
		static int Save(CScenarioPoint* point, char* extensionDefSpawnPoint, CScenarioInfoManager* scenarioInfoMgr)
		{
			uint32_t spawnType = *(uint32_t*)(extensionDefSpawnPoint + 0x30);
			int scenarioType = CScenarioInfoManager_GetScenarioTypeByHash(scenarioInfoMgr, &spawnType, true, true);

			uint32_t pedType = *(uint32_t*)(extensionDefSpawnPoint + 0x34);
			uint32_t modelSetHash = GetFinalModelSetHash(pedType);
			int modelSetType = IsScenarioVehicleInfo(scenarioType) ? 2 : 0;
			uint32_t modelSet = CAmbientModelSetsManager_FindIndexByHash(*g_AmbientModelSetsMgr, modelSetType, modelSetHash);

			//spdlog::info("InitFromSpawnPointDef:: Save -> point:{}, spawnType:{:08X}, scenarioType:{:08X}, modelSetHash:{:08X}, modelSet:{:08X}",
			//			(void*)point, spawnType, scenarioType, modelSetHash, modelSet);
			SavePoint(point, scenarioType, modelSet);

			return scenarioType;
		}

		void InternalMain() override
		{
			push(rcx);
			push(r8);
			sub(rsp, 0x18);

			mov(r8, rcx);    // third param:  CScenarioInfoManager*
			//mov(rdx, rdx); // second param: CExtensionDefSpawnPoint*
			mov(rcx, rbx);   // first param:  CScenarioPoint*
			mov(rax, (uintptr_t)Save);
			call(rax);

			add(rsp, 0x18);
			pop(r8);
			pop(rcx);

			ret();
		}
	} savePointStub;

	auto location = hook::get_pattern("41 B1 01 48 8D 55 20 89 45 20");
	hook::nop(location, 0x12);
	hook::call(location, savePointStub.GetCode());
}

static std::unordered_map<void*, uint32_t> g_SpawnPointsScenarioTypes;

static void(*CSpawnPoint_InitFromDef_orig)(void* spawnPoint, char* extensionDefSpawnPoint);
static void CSpawnPoint_InitFromDef_detour(void* spawnPoint, char* extensionDefSpawnPoint)
{
	uint32_t spawnType = *(uint32_t*)(extensionDefSpawnPoint + 0x30);
	uint32_t scenarioType = CScenarioInfoManager_GetScenarioTypeByHash(*g_ScenarioInfoMgr, &spawnType, true, true);

	g_SpawnPointsScenarioTypes[spawnPoint] = scenarioType;

	CSpawnPoint_InitFromDef_orig(spawnPoint, extensionDefSpawnPoint);
}

static void*(*CSpawnPoint_dtor_orig)(void* spawnPoint, char a2);
static void* CSpawnPoint_dtor_detour(void* spawnPoint, char a2)
{
	g_SpawnPointsScenarioTypes.erase(spawnPoint);

	return CSpawnPoint_dtor_orig(spawnPoint, a2);
}

static void Patch9()
{
	spdlog::info(__func__);

	auto cspawnPointVTable = hook::get_address<void**>(hook::get_pattern("48 8D 05 ? ? ? ? 41 B9 ? ? ? ? 48 89 02", 3));
	CSpawnPoint_dtor_orig = (decltype(CSpawnPoint_dtor_orig))cspawnPointVTable[0];
	CSpawnPoint_InitFromDef_orig = (decltype(CSpawnPoint_InitFromDef_orig))cspawnPointVTable[2];

	cspawnPointVTable[0] = CSpawnPoint_dtor_detour;
	cspawnPointVTable[2] = CSpawnPoint_InitFromDef_detour;


	// CScenarioPoint::ctorWithEntity

	static struct : jitasm::Frontend
	{
		static uint32_t Save(CScenarioPoint* point, void* spawnPoint)
		{
			uint32_t scenarioType = 0;
			auto p = g_SpawnPointsScenarioTypes.find(spawnPoint);
			if (p != g_SpawnPointsScenarioTypes.end())
			{
				scenarioType = p->second;
			}

			uint32_t pedType = *(uint32_t*)((char*)spawnPoint + 0x1C);
			uint32_t modelSetHash = GetFinalModelSetHash(pedType);
			int modelSetType = IsScenarioVehicleInfo(scenarioType) ? 2 : 0;
			uint32_t modelSet = CAmbientModelSetsManager_FindIndexByHash(*g_AmbientModelSetsMgr, modelSetType, modelSetHash);

			//spdlog::info("ctorWithEntity:: Save -> point:{}, spawnPoint:{}, scenarioType:{:08X}, modelSetHash:{:08X}, modelSet:{:08X}",
			//	(void*)point, spawnPoint, scenarioType, modelSetHash, modelSet);
			SavePoint(point, scenarioType, modelSet);

			return modelSetHash;
		}

		void InternalMain() override
		{
			push(rcx);
			push(rdx);
			sub(rsp, 0x18);

			mov(rdx, rdi); // second param: CSpawnPoint*
			mov(rcx, rbx); // first param:  CScenarioPoint*
			mov(rax, (uintptr_t)Save);
			call(rax);

			add(rsp, 0x18);
			pop(rdx);
			pop(rcx);

			ret();
		}
	} savePointStub;

	auto location = hook::get_pattern("E8 ? ? ? ? 89 47 1C 0F B6 4B 15");
	hook::nop(location, 0x5);
	hook::call(location, savePointStub.GetCode());
}

static void(*CSpawnPointOverrideExtension_OverrideScenarioPoint_orig)(char* spawnPointOverrideExtension, CScenarioPoint* point);
static void CSpawnPointOverrideExtension_OverrideScenarioPoint_detour(char* spawnPointOverrideExtension, CScenarioPoint* point)
{
	auto p = g_Points.find(point);
	if (p != g_Points.end()) // every point passed to this method should already be in the map from the CScenarioPoint::ctorWithEntity hook
	{
		uint32_t origiType = p->second.iType;
		uint32_t origModelSetId = p->second.ModelSetId;

		uint32_t overrideScenarioType = *(uint32_t*)(spawnPointOverrideExtension + 0x8);
		if (overrideScenarioType)
		{
			uint32_t newScenarioType = CScenarioInfoManager_GetScenarioTypeByHash(*g_ScenarioInfoMgr, &overrideScenarioType, true, true);
			if (newScenarioType == 0xFFFFFFFF)
				newScenarioType = 0;
			p->second.iType = newScenarioType;
		}

		uint32_t overrideModelSet = *(uint32_t*)(spawnPointOverrideExtension + 0x14);
		if (overrideModelSet)
		{
			overrideModelSet = GetFinalModelSetHash(overrideModelSet);
			int modelSetType = IsScenarioVehicleInfo(p->second.iType) ? 2 : 0;
			uint32_t newModelSet = CAmbientModelSetsManager_FindIndexByHash(*g_AmbientModelSetsMgr, modelSetType, overrideModelSet);
			p->second.ModelSetId = newModelSet;
		}


		//spdlog::info("OverrideScenarioPoint:: detour -> spawnPointOverrideExtension:{}, point:{}, iType:{:08X}, new_iType:{:08X}, ModelSetId:{:08X}, new_ModelSetId:{:08X}",
		//	(void*)spawnPointOverrideExtension, (void*)point, origiType, p->second.iType, origModelSetId, p->second.ModelSetId);
	}

	CSpawnPointOverrideExtension_OverrideScenarioPoint_orig(spawnPointOverrideExtension, point);
}

static void Patch10()
{
	spdlog::info(__func__);

	// CSpawnPointOverrideExtension::OverrideScenarioPoint

	MH_CreateHook(hook::get_pattern("48 83 EC 20 8B 41 08 33 FF 48 8B F2", -0xB), CSpawnPointOverrideExtension_OverrideScenarioPoint_detour,
		(void**)&CSpawnPointOverrideExtension_OverrideScenarioPoint_orig);
}

static void Patch11()
{
	spdlog::info(__func__);

	// patch calls to CScenarioPoint::GetScenarioType

	constexpr int R14_REG = 0;
	constexpr int EBX_REG = 1;
	constexpr int ECX_REG = 2;
	constexpr int ESI_REG = 3;
	struct stub : jitasm::Frontend
	{
		const int m_reg;

		stub(int reg) : m_reg(reg)
		{
		}

		void InternalMain() override
		{
			sub(rsp, 0x8);

			// rcx already is CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedScenarioType);
			call(rax);

			add(rsp, 0x8);

			switch (m_reg)
			{
			case R14_REG: mov(r14d, eax); break;
			case EBX_REG: mov(ebx, eax); break;
			case ECX_REG: mov(ecx, eax); break;
			case ESI_REG: mov(esi, eax); break;
			}
			
			ret();
		}
	};
	static stub getScenarioTypeAndStoreInR14Stub(R14_REG);
	static stub getScenarioTypeAndStoreInEBXStub(EBX_REG);
	static stub getScenarioTypeAndStoreInECXStub(ECX_REG);
	static stub getScenarioTypeAndStoreInESIStub(ESI_REG);

	{
		auto location = hook::get_pattern("E8 ? ? ? ? 44 0F B6 F0 41 8B CE E8");
		hook::nop(location, 0x9);
		hook::call(location, getScenarioTypeAndStoreInR14Stub.GetCode());
	}

	{
		auto location = hook::get_pattern("E8 ? ? ? ? 0F B6 D8 8B CB E8");
		hook::nop(location, 0x8);
		hook::call(location, getScenarioTypeAndStoreInEBXStub.GetCode());
	}
	{
		auto location = hook::get_pattern("E8 ? ? ? ? 4C 8B 0D ? ? ? ? 45 0F B7 51");
		hook::nop(location, 0x5);
		hook::call(location, getScenarioTypeAndStoreInEBXStub.GetCode());
		hook::nop((char*)location + 0x11, 3);
	}
	{
		auto location = hook::get_pattern("E8 ? ? ? ? 48 8B 15 ? ? ? ? 0F B7 4A 10");
		hook::nop(location, 0x5);
		hook::call(location, getScenarioTypeAndStoreInEBXStub.GetCode());
		hook::nop((char*)location + 0x10, 3);
	}

	{
		auto location = hook::get_pattern("E8 ? ? ? ? 0F B6 C8 E8");
		hook::nop(location, 0x8);
		hook::call(location, getScenarioTypeAndStoreInECXStub.GetCode());
	}
	{
		auto location = hook::get_pattern("E8 ? ? ? ? 48 8B D5 0F B6 C8");
		hook::nop(location, 0x5);
		hook::call(location, getScenarioTypeAndStoreInECXStub.GetCode());
		hook::nop((char*)location + 0x8, 3);
	}
	{
		auto location = hook::get_pattern("E8 ? ? ? ? 48 8B D3 0F B6 C8");
		hook::nop(location, 0x5);
		hook::call(location, getScenarioTypeAndStoreInECXStub.GetCode());
		hook::nop((char*)location + 0x8, 3);
	}

	{
		auto location = hook::get_pattern("E8 ? ? ? ? 4C 8B 2D ? ? ? ? 44 8A FF");
		hook::nop(location, 0x5);
		hook::call(location, getScenarioTypeAndStoreInESIStub.GetCode());
		hook::nop((char*)location + 0xF, 3);
	}
}


static uint32_t GetScenarioTypeIndex(CScenarioPoint* point, uint32_t subType)
{
	uint32_t type = GetSavedScenarioType(point);
	CScenarioInfoManager* mgr = *g_ScenarioInfoMgr;
	if (type >= mgr->Scenarios.Count)
	{
		uint32_t idx = type - mgr->Scenarios.Count;
		type = mgr->ScenarioTypeGroups.Items[idx]->Types.Items[subType].ScenarioTypeIndex;
	}

	return type;
}

static bool(*CScenarioPoint_IsScenarioTypeEnabled_orig)(CScenarioPoint*, uint32_t);
static bool CScenarioPoint_IsScenarioTypeEnabled_detour(CScenarioPoint* _this, uint32_t subType)
{
	return (*g_ScenarioInfoMgr)->ScenarioEnabledFlags.Items[GetScenarioTypeIndex(_this, subType)];
}

static uint32_t(*CScenarioPoint_GetScenarioTypeIndex_orig)(CScenarioPoint*, uint32_t);
static uint32_t CScenarioPoint_GetScenarioTypeIndex_detour(CScenarioPoint* _this, uint32_t subType)
{
	return GetScenarioTypeIndex(_this, subType);
}

static void Patch12()
{
	spdlog::info(__func__);

	hook::pattern pattern("0F B6 41 15 4C 8B 05 ? ? ? ? 41 0F B7 48");
	pattern.count(2);

	MH_CreateHook(pattern.get(0).get<void>(), CScenarioPoint_IsScenarioTypeEnabled_detour, (void**)&CScenarioPoint_IsScenarioTypeEnabled_orig);
	MH_CreateHook(pattern.get(1).get<void>(), CScenarioPoint_GetScenarioTypeIndex_detour, (void**)&CScenarioPoint_GetScenarioTypeIndex_orig);
}

static bool(*CScenarioPoint_CanSpawn_orig)(CScenarioPoint*, bool, bool, uint32_t);
static bool CScenarioPoint_CanSpawn_detour(CScenarioPoint* _this, bool a2, bool a3, uint32_t subType)
{
	return CScenarioPoint_CanScenarioSpawn(_this, GetScenarioTypeIndex(_this, subType), a2, a3);
}

static void Patch13()
{
	spdlog::info(__func__);

	MH_CreateHook(hook::get_pattern("40 53 48 83 EC 20 48 8B 05 ? ? ? ? 44 8A DA"), CScenarioPoint_CanSpawn_detour, (void**)&CScenarioPoint_CanSpawn_orig);
}

static void Patch14()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			mov(rbx, rcx);

			sub(rsp, 0x8);

			// rcx already is CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedScenarioType);
			call(rax);
			mov(ecx, eax);

			add(rsp, 0x8);
			
			ret();
		}
	} getScenarioTypeStub;

	auto location = hook::get_pattern("48 8B D9 0F B6 49 15 40 8A EA");
	hook::nop(location, 0x7);
	hook::call(location, getScenarioTypeStub.GetCode());
}

static void Patch15()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			sub(rsp, 0x8);

			// rcx already is CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedScenarioType);
			call(rax);
			mov(ecx, eax);

			add(rsp, 0x8);

			test(ecx, ecx);
			ret();
		}
	} getScenarioTypeStub;
	{
		auto location = hook::get_pattern("0F B6 49 15 85 C9 78 48");
		hook::nop(location, 0x6);
		hook::call(location, getScenarioTypeStub.GetCode());
	}
	{
		auto location = hook::get_pattern("0F B6 49 15 85 C9 0F 88");
		hook::nop(location, 0x6);
		hook::call(location, getScenarioTypeStub.GetCode());
	}
}

static void Patch16()
{
	spdlog::info(__func__);

	// CTaskUseScenario::ctor

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			sub(rsp, 0x8);

			mov(rcx, rax); // param: CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedScenarioType);
			call(rax);

			mov(dword_ptr[rdi + 0x18C], eax);

			add(rsp, 0x8);

			ret();
		}
	} getScenarioTypeStub;

	auto location = hook::get_pattern("0F B6 40 15 89 87");
	hook::nop(location, 0xA);
	// call_rcx because rax contains the CScenarioPoint*
	hook::call_rcx(location, getScenarioTypeStub.GetCode());
}

static void Patch17()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		static uint32_t GetScenarioType(uint64_t offset, CScenarioPoint* points)
		{
			uint64_t index = (offset / sizeof(CScenarioPoint));
			CScenarioPoint* p = &points[index];
			return GetSavedScenarioType(p);
		}

		void InternalMain() override
		{
			push(r8);
			push(r9);
			sub(rsp, 0x18);

			//mov(rdx, rdx); // second param: CScenarioPoint array
			mov(rcx, rdi);   // first param: offset
			mov(rax, (uintptr_t)GetScenarioType);
			call(rax);

			mov(ecx, eax);

			add(rsp, 0x18);
			pop(r9);
			pop(r8);

			ret();
		}
	} getScenarioTypeStub;

	auto location = hook::get_pattern("0F B6 4C 17 ? E8");
	hook::nop(location, 0x5);
	hook::call(location, getScenarioTypeStub.GetCode());
}

static void Patch18()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		static bool IsVehicleInfo(CScenarioPoint* point)
		{
			return IsScenarioVehicleInfo(GetSavedScenarioType(point));
		}

		void InternalMain() override
		{
			sub(rsp, 0x8);

			mov(rcx, rdx);   // first param: CScenarioPoint*
			mov(rax, (uintptr_t)IsVehicleInfo);
			call(rax);

			add(rsp, 0x8);

			ret();
		}
	} isVehicleInfoStub;

	auto location = hook::get_pattern("0F B6 4A 15 E8 ? ? ? ? 84 C0");
	hook::nop(location, 0x9);
	hook::call(location, isVehicleInfoStub.GetCode());
}

static void Patch19()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		static CScenarioInfoManager* GetMgr()
		{
			return *g_ScenarioInfoMgr;
		}

		void InternalMain() override
		{
			push(r8);
			push(r9);
			sub(rsp, 0x18);

			mov(rcx, rdx);   // first param: CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedScenarioType);
			call(rax);
			mov(r9d, eax); // save return value

			mov(rax, (uintptr_t)GetMgr);
			call(rax);

			mov(rdx, rax); // store g_ScenarioInfoMgr
			mov(eax, r9d); // store the scenario type

			add(rsp, 0x18);
			pop(r9);
			pop(r8);

			ret();
		}
	} getScenarioTypeStub;

	auto location = hook::get_pattern("0F B6 42 15 48 8B 15 ? ? ? ? 0F B7 4A 10");
	hook::nop(location, 0xB);
	hook::call(location, getScenarioTypeStub.GetCode());
}

static void Patch20()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(r8);
			push(r9);
			sub(rsp, 0x18);

			mov(rcx, rsi);   // first param: CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedScenarioType);
			call(rax);
			mov(ecx, eax);

			add(rsp, 0x18);
			pop(r9);
			pop(r8);

			movzx(eax, word_ptr[r8 + 0x10]);

			ret();
		}
	} getScenarioTypeStub;

	auto location = hook::get_pattern("0F B6 4E 15 41 0F B7 40 ? 3B C8");
	hook::nop(location, 0x9);
	hook::call(location, getScenarioTypeStub.GetCode());
}

static void Patch21()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		// TODO: verify that this patch is working
		static uint32_t wrap(CScenarioPoint* p) { spdlog::info("Patch21:{}:({}, {}, {})", (void*)p, p->vPositionAndDirection[0], p->vPositionAndDirection[1], p->vPositionAndDirection[2]); spdlog::default_logger()->flush(); return GetSavedScenarioType(p); }
		void InternalMain() override
		{
			push(rax);
			sub(rsp, 0x10);

			mov(rcx, rsi);   // first param: CScenarioPoint*
			mov(rax, (uintptr_t)wrap);
			call(rax);
			mov(r9d, eax);

			add(rsp, 0x10);
			pop(rax);

			ret();
		}
	} getScenarioTypeStub;

	auto location = hook::get_pattern("44 0F B6 4E ? 0F 28 44 24 ? 4C 8D 44 24");
	hook::nop(location, 0x5);
	hook::call(location, getScenarioTypeStub.GetCode());
}

static void Patch22()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(r8);
			push(r9);
			sub(rsp, 0x18);

			mov(rcx, r14);   // first param: CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedScenarioType);
			call(rax);
			mov(ecx, eax);

			add(rsp, 0x18);
			pop(r9);
			pop(r8);

			ret();
		}
	} getScenarioTypeStub;

	auto location = hook::get_pattern("41 0F B6 4E ? 85 C9");
	hook::nop(location, 0x5);
	hook::call(location, getScenarioTypeStub.GetCode());
}

static void Patch23()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		// TODO: verify that this patch is working
		static uint32_t wrap(CScenarioPoint* p) { spdlog::info("Patch23:{}:({}, {}, {})", (void*)p, p->vPositionAndDirection[0], p->vPositionAndDirection[1], p->vPositionAndDirection[2]); spdlog::default_logger()->flush(); return GetSavedScenarioType(p); }
		void InternalMain() override
		{
			push(r8);
			push(r9);
			push(rcx);
			sub(rsp, 0x20);

			mov(rcx, rdx);   // first param: CScenarioPoint*
			mov(rax, (uintptr_t)wrap);
			call(rax);

			add(rsp, 0x20);
			pop(rcx);
			pop(r9);
			pop(r8);

			cmp(eax, dword_ptr[rbx + 0x18C]);

			ret();
		}
	} getScenarioTypeStub;

	auto location = hook::get_pattern("0F B6 42 15 3B 83 ? ? ? ? 75 32");
	hook::nop(location, 0xA);
	hook::call(location, getScenarioTypeStub.GetCode());
}

static void Patch24()
{
	spdlog::info(__func__);

	// CScenarioPoint::TryCreateCargen
	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(r8);
			push(r9);
			sub(rsp, 0x18);

			// rcx already has CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedScenarioType);
			call(rax);
			mov(esi, eax);

			add(rsp, 0x18);
			pop(r9);
			pop(r8);

			test(esi, esi);
			ret();
		}
	} getScenarioTypeStub;
	{
		auto location = hook::get_pattern("0F B6 71 15 85 F6 0F 88");
		hook::nop(location, 0x6);
		hook::call(location, getScenarioTypeStub.GetCode());
	}

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(r8);
			push(r9);
			sub(rsp, 0x18);

			mov(rcx, rbx); // param: CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedModelSetId);
			call(rax);
			mov(edx, eax);

			add(rsp, 0x18);
			pop(r9);
			pop(r8);

			mov(rcx, rdi);
			ret();
		}
	} getModelSetIdStub;
	{
		auto location = hook::get_pattern("0F B6 53 16 48 8B CF E8");
		hook::nop(location, 0x7);
		hook::call(location, getModelSetIdStub.GetCode());
	}
}

static void Patch25()
{
	spdlog::info(__func__);

	hook::pattern pattern("81 FA ? ? ? ? 74 14 48 8B 05");
	pattern.count(2);
	pattern.for_each_result([](const hook::pattern_match& match)
	{
		hook::put(match.get<void>(2), 0xFFFFFFFF);
	});
}

static void Patch26()
{
	spdlog::info(__func__);

	// CreateCargen
	hook::put(hook::get_pattern("81 FD ? ? ? ? 41 0F 95 C6", 2), 0xFFFFFFFF);
}

struct ExtendedCargen
{
	uint32_t ScenarioType;
};
static std::unordered_map<CCargen*, ExtendedCargen> g_Cargens;

static void SaveCargen(CCargen* cargen, uint32_t scenarioType)
{
	g_Cargens[cargen] = { scenarioType };
}

static void RemoveCargen(CCargen* cargen)
{
	if (g_Cargens.erase(cargen) == 0)
	{
		spdlog::warn("RemoveCargen:: CARGEN {} NOT FOUND IN MAP ({}, {}, {})",
			(void*)cargen, cargen->Position[0], cargen->Position[1], cargen->Position[2]);
		LogStackTrace<2>();
	}
}

static uint32_t GetSavedCargenScenarioType(CCargen* cargen)
{
	auto p = g_Cargens.find(cargen);
	if (p != g_Cargens.end())
	{
		return p->second.ScenarioType;
	}
	else
	{
		spdlog::warn("GetSavedScenarioType:: CARGEN {} NOT FOUND IN MAP ({}, {}, {})",
			(void*)cargen, cargen->Position[0], cargen->Position[1], cargen->Position[2]);
		LogStackTrace<2>();
		return cargen->ScenarioType;
	}
}

static void(*CCargen_Initialize_orig)(CCargen* cargen, float x, float y, float z, float dirX, float dirY, float a7, int modelHash, int primaryColor, int secondaryColor,
									  int pearlescentColor, int extraColor, int interiorColor, int accentColor, int unkFlag, int field30, int field38, int scenarioType,
									  char unkFlag2, unsigned int *popGroup, char field3F, char field40, bool unkFlag4, char unkFlag3);
static void CCargen_Initialize_detour(CCargen* cargen, float x, float y, float z, float dirX, float dirY, float a7, int modelHash, int primaryColor, int secondaryColor,
									  int pearlescentColor, int extraColor, int interiorColor, int accentColor, int unkFlag, int field30, int field38, int scenarioType,
									  char unkFlag2, unsigned int *popGroup, char field3F, char field40, bool unkFlag4, char unkFlag3)
{
	SaveCargen(cargen, scenarioType);

	CCargen_Initialize_orig(cargen, x,  y,  z,  dirX,  dirY,  a7, modelHash, primaryColor, secondaryColor,
		pearlescentColor, extraColor, interiorColor, accentColor, unkFlag, field30, field38, scenarioType,
		unkFlag2, popGroup, field3F, field40, unkFlag4, unkFlag3);
}

static void Patch27()
{
	spdlog::info(__func__);

	// CCargen::Initialize
	MH_CreateHook(hook::get_pattern("41 57 48 8B EC 48 83 EC 30 80 61 3B 7F", -0x12), CCargen_Initialize_detour, (void**)&CCargen_Initialize_orig);
}

static void(*DeleteCargenFromPool_orig)(void* pool, CCargen* cargen);
static void DeleteCargenFromPool_detour(void* pool, CCargen* cargen)
{
	RemoveCargen(cargen);

	DeleteCargenFromPool_orig(pool, cargen);
}

static void Patch28()
{
	spdlog::info(__func__);

	// CCargen::Initialize
	MH_CreateHook(hook::get_pattern("48 85 D2 74 7E 48 89 5C 24 ? 48 89 6C 24"), DeleteCargenFromPool_detour, (void**)&DeleteCargenFromPool_orig);
}

static void Patch29()
{
	spdlog::info(__func__);

	// CREATE_SCRIPT_VEHICLE_GENERATOR
	hook::put(hook::get_pattern("C7 84 24 ? ? ? ? ? ? ? ? 83 A4 24", 7), 0xFFFFFFFF);
}

static void Patch30()
{
	spdlog::info(__func__);

	hook::put(hook::get_pattern("C7 84 24 ? ? ? ? ? ? ? ? C1 E9 1C 89 8C 24", 7), 0xFFFFFFFF);
}

static void Patch31()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(r8);
			push(r9);
			sub(rsp, 0x18);

			mov(rcx, rbx); // param: CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedModelSetId);
			call(rax);
			mov(edx, eax);

			add(rsp, 0x18);
			pop(r9);
			pop(r8);

			mov(rcx, rsi);
			ret();
		}
	} getModelSetIdStub;

	auto location = hook::get_pattern("0F B6 53 16 48 8B CE E8");
	hook::nop(location, 0x7);
	hook::call(location, getModelSetIdStub.GetCode());
}

static void Patch32()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(r8);
			push(r9);
			sub(rsp, 0x18);

			mov(rcx, rsi); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);
			mov(esi, eax);

			add(rsp, 0x18);
			pop(r9);
			pop(r8);

			lea(rdx, qword_ptr[rsp + 0x30]);
			ret();
		}
	} getScenarioTypeStub;

	auto location = hook::get_pattern("0F B6 76 39 48 8D 54 24 ? 8B CE");
	hook::nop(location, 0x7);
	hook::call(location, getScenarioTypeStub.GetCode());
}

static void Patch33()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			sub(rsp, 0x8);

			mov(rcx, rbx); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);
			mov(edi, eax);

			add(rsp, 0x8);

			xor(r9d, r9d);
			ret();
		}
	} getScenarioTypeStub;
	{
		auto location = hook::get_pattern("0F B6 7B 39 45 33 C9");
		hook::nop(location, 0x7);
		hook::call(location, getScenarioTypeStub.GetCode());
	}

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(r8);
			push(r9);
			sub(rsp, 0x18);

			mov(rcx, r13); // param: CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedModelSetId);
			call(rax);

			add(rsp, 0x18);
			pop(r9);
			pop(r8);

			ret();
		}
	} getModelSetIdStub;
	{
		auto location = hook::get_pattern("41 0F B6 45 ? 4C 89 65 AF 3D");
		hook::nop(location, 0x5);
		hook::call(location, getModelSetIdStub.GetCode());
	}

	hook::put(hook::get_pattern("4C 89 65 AF 3D ? ? ? ? 74 13 8B D0", 5), 0xFFFFFFFF);
}

static void Patch34()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(r8);
			push(r9);
			push(rax);
			sub(rsp, 0x20);

			mov(rcx, r15); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);
			mov(r12d, eax);

			add(rsp, 0x20);
			pop(rax);
			pop(r9);
			pop(r8);

			ret();
		}
	} getScenarioTypeStub;

	auto location = hook::get_pattern("45 0F B6 67 ? 48 8B F8");
	hook::nop(location, 0x5);
	hook::call_rcx(location, getScenarioTypeStub.GetCode());
}

static void Patch35()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		// TODO: verify that this patch is working
		static uint32_t wrap(CScenarioPoint* p) { spdlog::info("Patch35:{}:({}, {}, {})", (void*)p, p->vPositionAndDirection[0], p->vPositionAndDirection[1], p->vPositionAndDirection[2]); spdlog::default_logger()->flush(); return GetSavedScenarioType(p); }
		void InternalMain() override
		{
			push(r8);
			push(r9);
			push(rax);
			sub(rsp, 0x20);

			lea(rcx, qword_ptr[rbp + 0x57 - 0x70]); // param: CScenarioPoint*
			mov(rax, (uintptr_t)wrap);
			call(rax);
			mov(r12d, eax);

			add(rsp, 0x20);
			pop(rax);
			pop(r9);
			pop(r8);

			ret();
		}
	} getScenarioTypeStub;

	auto location = hook::get_pattern("44 0F B6 65 ? 0D");
	hook::nop(location, 0x5);
	hook::call_rcx(location, getScenarioTypeStub.GetCode());
}

static void Patch36()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(r8);
			push(rcx);
			sub(rsp, 0x18);

			mov(rcx, rdx); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);
			mov(r9d, eax);

			add(rsp, 0x18);
			pop(rcx);
			pop(r8);

			ret();
		}
	} getCargenScenarioTypeStub;
	{
		auto location = hook::get_pattern("44 0F B6 4A ? 49 8B D8");
		hook::nop(location, 0x5);
		hook::call(location, getCargenScenarioTypeStub.GetCode());
	}

	static struct : jitasm::Frontend
	{
		// TODO: verify that this patch is working
		static uint32_t wrap(CScenarioPoint* p) { spdlog::info("Patch36:getPointScenarioTypeStub:{}:({}, {}, {})", (void*)p, p->vPositionAndDirection[0], p->vPositionAndDirection[1], p->vPositionAndDirection[2], (void*)p); spdlog::default_logger()->flush(); return GetSavedScenarioType(p); }
		void InternalMain() override
		{
			push(r8);
			push(rax);
			sub(rsp, 0x18);

			mov(rcx, rax); // param: CScenarioPoint*
			mov(rax, (uintptr_t)wrap);
			call(rax);
			mov(r9d, eax);

			add(rsp, 0x18);
			pop(rax);
			pop(r8);

			ret();
		}
	} getPointScenarioTypeStub;
	{
		auto location = hook::get_pattern("44 0F B6 48 ? 48 8D 0D");
		hook::nop(location, 0x5);
		hook::call_rcx(location, getPointScenarioTypeStub.GetCode());
	}
}

static void Patch37()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(rcx);
			sub(rsp, 0x10);

			mov(rcx, qword_ptr[rbx + 0x48]); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);

			add(rsp, 0x10);
			pop(rcx);

			mov(rdx, qword_ptr[rbx + 0x48]);
			cmp(eax, 0xFFFFFFFF);
			ret();
		}
	} getScenarioTypeStub;

	auto location = hook::get_pattern("48 8B 53 48 80 7A 39 FF");
	hook::nop(location, 0x8);
	hook::call(location, getScenarioTypeStub.GetCode());
}

static void Patch38()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		const uintptr_t returnAddr = (uintptr_t)hook::get_pattern("41 8A D6 48 8B CB E8 ? ? ? ? 84 C0 74 34");
		const uintptr_t jumpAddr = (uintptr_t)hook::get_pattern("40 84 F6 74 4E 48 8D 55 30");

		void InternalMain() override
		{
			mov(rcx, rbx); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);

			cmp(eax, 0xFFFFFFFF);
			jz("doJump");

			mov(rax, returnAddr);
			jmp(rax);


			L("doJump");
			mov(rax, jumpAddr);
			jmp(rax);
		}
	} cmpScenarioTypeStub;

	auto location = hook::get_pattern("80 7B 39 FF 74 43");
	hook::nop(location, 0x6);
	hook::jump(location, cmpScenarioTypeStub.GetCode());
}

static void Patch39()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(r8);
			push(r9);
			sub(rsp, 0x18);

			mov(rcx, rbx); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);

			add(rsp, 0x18);
			pop(r9);
			pop(r8);

			cmp(eax, 0xFFFFFFFF);
			movaps(xmm8, xmm0);
			ret();
		}
	} cmpScenarioTypeStub;

	auto location = hook::get_pattern("80 7B 39 FF 44 0F 28 C0");
	hook::nop(location, 0x8);
	hook::call(location, cmpScenarioTypeStub.GetCode());
}

static void Patch40()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		const uintptr_t returnAddr = (uintptr_t)hook::get_pattern("F3 0F 10 3D ? ? ? ? F3 0F 10 35 ? ? ? ? 48 8B CB");
		const uintptr_t jumpAddr = (uintptr_t)hook::get_pattern("F3 0F 10 44 24 ? F3 0F 5C 43 ? 0F 2F 05 ? ? ? ? 73 07");

		void InternalMain() override
		{
			push(r8);
			push(r9);
			sub(rsp, 0x10);

			mov(rcx, rbx); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);

			add(rsp, 0x10);
			pop(r9);
			pop(r8);

			cmp(eax, 0xFFFFFFFF);
			jz("doJump");

			mov(rax, returnAddr);
			jmp(rax);


			L("doJump");
			mov(rax, jumpAddr);
			jmp(rax);
		}
	} cmpScenarioTypeStub;

	auto location = hook::get_pattern("80 7B 39 FF 74 7C F3 0F 10 3D");
	hook::nop(location, 0x6);
	hook::jump(location, cmpScenarioTypeStub.GetCode());
}

static void Patch41()
{
	spdlog::info(__func__);

	struct cmpScenarioTypeStub : jitasm::Frontend
	{
		const uintptr_t returnAddr, jumpAddr;

		cmpScenarioTypeStub(void* returnAddr, void* jumpAddr)
			: returnAddr((uintptr_t)returnAddr), jumpAddr((uintptr_t)jumpAddr)
		{
		}

		void InternalMain() override
		{
			push(r8);
			push(r9);
			push(rcx);
			sub(rsp, 0x18);

			// rcx already is CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);

			add(rsp, 0x18);
			pop(rcx);
			pop(r9);
			pop(r8);

			cmp(eax, 0xFFFFFFFF);
			jz("doJump");
			mov(rax, returnAddr);
			jmp(rax);


			L("doJump");
			mov(rax, jumpAddr);
			jmp(rax);
		}
	};
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("E8 ? ? ? ? 48 8B D8 48 85 C0 0F 84 ? ? ? ? 8A 48 10"),
			hook::get_pattern("49 8B CE E8 ? ? ? ? 41 8B 16 66 89 45 40")
		);
		auto location = hook::get_pattern("80 79 39 FF 0F 84 ? ? ? ? E8 ? ? ? ? 48 8B D8");
		hook::nop(location, 0xA);
		hook::jump(location, stub.GetCode());
	}
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("F6 41 3A 40 75 28 E8"),
			hook::get_pattern("C0 E8 06 A8 01 74 04 32 C0 EB 02 B0 01", 0xB)
		);
		auto location = hook::get_pattern("80 79 39 FF 74 2E F6 41 3A 40");
		hook::nop(location, 0x6);
		hook::jump(location, stub.GetCode());
	}
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("E8 ? ? ? ? 48 85 C0 74 F2 0F B7 40 10"),
			hook::get_pattern("32 C0 EB 14 E8")
		);
		auto location = hook::get_pattern("80 79 39 FF 75 04");
		hook::nop(location, 0x6);
		hook::jump(location, stub.GetCode());
	}
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("F6 41 3A 40 75 16"),
			hook::get_pattern("8B 48 24 B0 01 C1 E9 0A", 0xC)
		);
		auto location = hook::get_pattern("80 79 39 FF 74 1C");
		hook::nop(location, 0x6);
		hook::jump(location, stub.GetCode());
	}
	{
		static cmpScenarioTypeStub stub(
			hook::get_address<void*>(hook::get_pattern("80 79 39 FF 0F 85 ? ? ? ? 40 38 35", 0x6)),
			hook::get_pattern("40 38 35 ? ? ? ? 74 2E F3 0F 10 01")
		);
		auto location = hook::get_pattern("80 79 39 FF 0F 85 ? ? ? ? 40 38 35");
		hook::nop(location, 0xA);
		hook::jump(location, stub.GetCode());
	}
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("74 19 E8 ? ? ? ? 48 85 C0 74 0F 8B 88", 0x2),
			hook::get_pattern("8B 88 ? ? ? ? B0 01 C1 E9 0A 84 C8", 0xF)
		);
		auto location = hook::get_pattern("80 79 39 FF 74 19");
		hook::nop(location, 0x6);
		hook::jump(location, stub.GetCode());
	}
}

static void Patch42()
{
	spdlog::info(__func__);

	struct cmpScenarioTypeStub : jitasm::Frontend
	{
		const uintptr_t returnAddr, jumpAddr;

		cmpScenarioTypeStub(void* returnAddr, void* jumpAddr)
			: returnAddr((uintptr_t)returnAddr), jumpAddr((uintptr_t)jumpAddr)
		{
		}

		void InternalMain() override
		{
			push(r8);
			push(r9);
			sub(rsp, 0x10);

			mov(rcx, rdi); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);

			add(rsp, 0x10);
			pop(r9);
			pop(r8);

			cmp(eax, 0xFFFFFFFF);
			jz("doJump");
			mov(rax, returnAddr);
			jmp(rax);


			L("doJump");
			mov(rax, jumpAddr);
			jmp(rax);
		}
	};
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("80 7F 39 FF 74 5E", 0x6),
			hook::get_pattern("8A 4F 3A 48 8B C3")
		);
		auto location = hook::get_pattern("80 7F 39 FF 74 5E");
		hook::nop(location, 0x6);
		hook::jump(location, stub.GetCode());
	}
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("83 BE ? ? ? ? ? 0F 84 ? ? ? ? 8A 47 3B"),
			hook::get_pattern("8A 47 3B A8 10 75 65")
		);
		auto location = hook::get_pattern("80 7F 39 FF 74 0D");
		hook::nop(location, 0x6);
		hook::jump(location, stub.GetCode());
	}
}

static void Patch43()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		const uintptr_t returnAddr = (uintptr_t)hook::get_pattern("48 8B CB E8 ? ? ? ? 48 85 C0 74 0E 8B 40 24");
		const uintptr_t jumpAddr = (uintptr_t)hook::get_pattern("48 8B 47 20 48 8B D6 48 8B CF 44 38 65 50");

		void InternalMain() override
		{
			mov(rcx, rbx); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);

			cmp(eax, 0xFFFFFFFF);
			jz("doJump");
			mov(rax, returnAddr);
			jmp(rax);


			L("doJump");
			mov(rax, jumpAddr);
			jmp(rax);
		}
	} cmpScenarioTypeStub;
	auto location = hook::get_pattern("80 7B 39 FF 74 1B");
	hook::nop(location, 0x6);
	hook::jump(location, cmpScenarioTypeStub.GetCode());
}

static void Patch44()
{
	spdlog::info(__func__);

	struct cmpScenarioTypeStub : jitasm::Frontend
	{
		const uintptr_t returnAddr, jumpAddr;

		cmpScenarioTypeStub(void* returnAddr, void* jumpAddr)
			: returnAddr((uintptr_t)returnAddr), jumpAddr((uintptr_t)jumpAddr)
		{
		}

		void InternalMain() override
		{
			push(r8);
			push(r9);
			sub(rsp, 0x10);

			mov(rcx, rbx); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);

			add(rsp, 0x10);
			pop(r9);
			pop(r8);

			cmp(eax, 0xFFFFFFFF);
			jnz("doJump");
			mov(rax, returnAddr);
			jmp(rax);


			L("doJump");
			mov(rax, jumpAddr);
			jmp(rax);
		}
	};
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("EB 48 E8 ? ? ? ? 44 8B 0D"),
			hook::get_pattern("F3 0F 10 05 ? ? ? ? 0F 2F 05 ? ? ? ? 0F 87 ? ? ? ? B0 01", 0x15)
		);
		auto location = hook::get_pattern("80 7B 39 FF 0F 85");
		hook::nop(location, 0xA);
		hook::jump(location, stub.GetCode());
	}
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("24 FD 88 43 3A EB DC"),
			hook::get_pattern("F6 43 3D 01 74 2F E8")
		);
		auto location = hook::get_pattern("80 7B 39 FF 75 07");
		hook::nop(location, 0x6);
		hook::jump(location, stub.GetCode());
	}
}

static void Patch45()
{
	spdlog::info(__func__);

	struct cmpScenarioTypeStub : jitasm::Frontend
	{
		const uintptr_t returnAddr, jumpAddr;

		cmpScenarioTypeStub(void* returnAddr, void* jumpAddr)
			: returnAddr((uintptr_t)returnAddr), jumpAddr((uintptr_t)jumpAddr)
		{
		}

		void InternalMain() override
		{
			push(r8);
			push(r9);
			sub(rsp, 0x10);

			mov(rcx, rbx); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);

			add(rsp, 0x10);
			pop(r9);
			pop(r8);

			cmp(eax, 0xFFFFFFFF);
			jz("doJump");
			mov(rax, returnAddr);
			jmp(rax);


			L("doJump");
			mov(rax, jumpAddr);
			jmp(rax);
		}
	};
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("84 53 3A 75 6D 48 8B 05"),
			hook::get_pattern("84 53 3C 74 2B F6 43 3B 40")
		);
		auto location = hook::get_pattern("80 7B 39 FF 74 29");
		hook::nop(location, 0x6);
		hook::jump(location, stub.GetCode());
	}
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("41 C6 06 01 41 BE"),
			hook::get_pattern("E9 ? ? ? ? 48 8B CF E8 ? ? ? ? 32 C0 48 8B 5C 24", 0x5)
		);
		auto location = hook::get_pattern("80 7B 39 FF 0F 84");
		hook::nop(location, 0xA);
		hook::jump(location, stub.GetCode());
	}
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("48 8B D6 48 8B CB E8 ? ? ? ? 84 C0 75 03"),
			hook::get_pattern("44 09 36 B0 01 E9")
		);
		auto location = hook::get_pattern("80 7B 39 FF 74 0F");
		hook::nop(location, 0x6);
		hook::jump(location, stub.GetCode());
	}


	struct cmpScenarioTypeStub2 : jitasm::Frontend
	{
		const uintptr_t returnAddr, jumpAddr;

		cmpScenarioTypeStub2(void* returnAddr, void* jumpAddr)
			: returnAddr((uintptr_t)returnAddr), jumpAddr((uintptr_t)jumpAddr)
		{
		}

		void InternalMain() override
		{
			push(r8);
			push(r9);
			push(rax);
			sub(rsp, 0x18);

			mov(rcx, rbx); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);
			mov(ecx, eax); // save return value

			add(rsp, 0x18);
			pop(rax);
			pop(r9);
			pop(r8);

			mov(dword_ptr[rbx + 0x20], eax);
			cmp(ecx, 0xFFFFFFFF);
			jnz("doJump");
			mov(rcx, returnAddr);
			jmp(rcx);


			L("doJump");
			mov(rcx, jumpAddr);
			jmp(rcx);
		}
	};
	{
		static cmpScenarioTypeStub2 stub(
			hook::get_pattern("F6 43 3B 40 75 04"),
			hook::get_pattern("33 C0 80 63 3A FE 84 C0")
		);
		auto location = hook::get_pattern("80 7B 39 FF 89 43 20 75 0A");
		hook::nop(location, 0x9);
		hook::jump_rcx(location, stub.GetCode());
	}
}

static void Patch46()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			sub(rsp, 0x8);

			// rcx already is CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);

			add(rsp, 0x8);

			mov(ecx, eax);
			test(ecx, ecx);

			ret();
		}
	} getScenarioTypeStub;
	auto location = hook::get_pattern("0F B6 49 39 85 C9");
	hook::nop(location, 0x6);
	hook::call(location, getScenarioTypeStub.GetCode());
}

static void Patch47()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		// TODO: verify that this patch is working
		static uint32_t wrap(CCargen* c) { spdlog::info("Patch47:cmpScenarioTypeStub:{}:({}, {}, {})", (void*)c, c->Position[0], c->Position[1], c->Position[2]); spdlog::default_logger()->flush(); return GetSavedCargenScenarioType(c); }
		void InternalMain() override
		{
			push(rcx);
			push(rdx);
			sub(rsp, 0x10);

			// rcx already is CCargen*
			mov(rax, (uintptr_t)wrap);
			call(rax);

			add(rsp, 0x10);
			pop(rdx);
			pop(rcx);

			cmp(eax, 0xFFFFFFFF);
			mov(r14, rdx);

			ret();
		}
	} cmpScenarioTypeStub;
	{
		auto location = hook::get_pattern("80 79 39 FF 4C 8B F2");
		hook::nop(location, 0x7);
		hook::jump(location, cmpScenarioTypeStub.GetCode());
	}

	static struct : jitasm::Frontend
	{
		const uintptr_t returnAddr = (uintptr_t)hook::get_pattern("F6 47 3A 04 74 78");
		const uintptr_t jumpAddr = (uintptr_t)hook::get_pattern("0F B6 05 ? ? ? ? 48 8B 5C 24");

		// TODO: verify that this patch is working
		static uint32_t wrap(CCargen* c) { spdlog::info("Patch47:cmpScenarioTypeStub2:{}:({}, {}, {})", (void*)c, c->Position[0], c->Position[1], c->Position[2]); spdlog::default_logger()->flush(); return GetSavedCargenScenarioType(c); }
		void InternalMain() override
		{
			mov(rcx, rdi); // param: CCargen*
			mov(rax, (uintptr_t)wrap);
			call(rax);

			cmp(eax, 0xFFFFFFFF);
			jz("doJump");
			mov(rax, returnAddr);
			jmp(rax);


			L("doJump");
			mov(rax, jumpAddr);
			jmp(rax);
		}
	} cmpScenarioTypeStub2;
	{
		auto location = hook::get_pattern("80 7F 39 FF 74 7E");
		hook::nop(location, 0x6);
		hook::jump(location, cmpScenarioTypeStub2.GetCode());
	}
}

static void Patch48()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(rcx);
			sub(rsp, 0x10);
			
			mov(rcx, r15); // param: CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedScenarioType);
			call(rax);
			mov(edx, eax);

			add(rsp, 0x10);
			pop(rcx);

			ret();
		}
	} getScenarioTypeStub;
	auto location = hook::get_pattern("41 0F B6 57 ? 4C 8D 4C 24");
	hook::nop(location, 0x5);
	hook::call(location, getScenarioTypeStub.GetCode());
}

static void Patch49()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			sub(rsp, 0x8);

			mov(rcx, r14); // param: CScenarioPoint*
			mov(rax, (uintptr_t)GetSavedModelSetId);
			call(rax);

			add(rsp, 0x8);

			cmp(eax, 0xFFFFFFFF);

			ret();
		}
	} cmpModelSetIdStub;
	auto location = hook::get_pattern("41 80 7E ? ? 45 0F B6 E4");
	hook::nop(location, 0x5);
	hook::call(location, cmpModelSetIdStub.GetCode());
}

static void Patch50()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		// TODO: verify that this patch is working
		static uint32_t wrap(CScenarioPoint* c) { spdlog::info("Patch50:getScenarioTypeStub:{}:({}, {}, {})", (void*)c, c->vPositionAndDirection[0], c->vPositionAndDirection[1], c->vPositionAndDirection[2]); spdlog::default_logger()->flush(); return GetSavedScenarioType(c); }
		void InternalMain() override
		{
			sub(rsp, 0x8);

			lea(rcx, qword_ptr[rbp - 0x80]); // param: CScenarioPoint*
			mov(rax, (uintptr_t)wrap);
			call(rax);
			mov(ecx, eax);

			add(rsp, 0x8);

			ret();
		}
	} getScenarioTypeStub;
	auto location = hook::get_pattern("0F B6 45 95 8B C8");
	hook::nop(location, 0x6);
	hook::call(location, getScenarioTypeStub.GetCode());
}

static void Patch51()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			sub(rsp, 0x8);

			mov(rcx, rbx); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);

			add(rsp, 0x8);

			cmp(eax, 0xFFFFFFFF);
			// workaround since jitasm doesn't have `sil` register, so can't do `setnz(sil)`
			jnz("set");	
			mov(esi, 0);
			ret();

			L("set");
			mov(esi, 1);
			ret();
		}
	} cmpScenarioTypeStub;
	auto location = hook::get_pattern("80 7B 39 FF 40 0F 95 C6");
	hook::nop(location, 0x8);
	hook::call(location, cmpScenarioTypeStub.GetCode());
}

static void Patch52()
{
	spdlog::info(__func__);

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			push(r8);
			push(r9);
			push(rax);
			sub(rsp, 0x20);

			mov(rcx, rax); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);
			mov(ecx, eax); // save result

			add(rsp, 0x20);
			pop(rax);
			pop(r9);
			pop(r8);

			cmp(ecx, 0xFFFFFFFF);
			mov(qword_ptr[rbp - 0x19], r13);

			ret();
		}
	} cmpScenarioTypeStub;
	{
		auto location = hook::get_pattern("80 78 39 FF 4C 89 6D E7");
		hook::nop(location, 0x8);
		hook::call_rcx(location, cmpScenarioTypeStub.GetCode());
	}

	static struct : jitasm::Frontend
	{
		void InternalMain() override
		{
			mov(rdx, qword_ptr[r14 + 0x48]);

			push(r8);
			push(r9);
			push(rdx);
			sub(rsp, 0x20);

			mov(rcx, rdx); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);

			add(rsp, 0x20);
			pop(rdx);
			pop(r9);
			pop(r8);

			cmp(eax, 0xFFFFFFFF);

			ret();
		}
	} cmpScenarioTypeStub2;
	{
		auto location = hook::get_pattern("49 8B 56 48 80 7A 39 FF");
		hook::nop(location, 0x8);
		hook::call(location, cmpScenarioTypeStub2.GetCode());
	}
}

static void Patch53()
{
	spdlog::info(__func__);

	struct cmpScenarioTypeStub : jitasm::Frontend
	{
		const uintptr_t returnAddr, jumpAddr;
		const bool ifZero;

		cmpScenarioTypeStub(void* returnAddr, void* jumpAddr, bool ifZero)
			: returnAddr((uintptr_t)returnAddr), jumpAddr((uintptr_t)jumpAddr), ifZero(ifZero)
		{
		}

		void InternalMain() override
		{
			mov(rcx, r14); // param: CCargen*
			mov(rax, (uintptr_t)GetSavedCargenScenarioType);
			call(rax);

			cmp(eax, 0xFFFFFFFF);
			ifZero ? jz("doJump") : jnz("doJump");
			mov(rax, returnAddr);
			jmp(rax);


			L("doJump");
			mov(rax, jumpAddr);
			jmp(rax);
		}
	};
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("41 F6 46 ? ? 75 14 48 8D 4D B0"),
			hook::get_pattern("48 8D 4D B0 E8 ? ? ? ? BA ? ? ? ? 48 8D 0D"),
			false
		);
		auto location = hook::get_pattern("41 80 7E ? ? 75 22 41 F6 46");
		hook::nop(location, 0x7);
		hook::jump(location, stub.GetCode());
	}
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("48 8D 4D B0 E8 ? ? ? ? BA ? ? ? ? 48 8D 0D"),
			hook::get_pattern("48 8D 4D B0 E8 ? ? ? ? BA ? ? ? ? 66 89 45 38"),
			true
		);
		auto location = hook::get_pattern("41 80 7E ? ? 74 1E");
		hook::nop(location, 0x7);
		hook::jump(location, stub.GetCode());
	}
	{
		static cmpScenarioTypeStub stub(
			hook::get_pattern("40 38 3D ? ? ? ? 74 05 40 8A DE"),
			hook::get_pattern("48 8D 4D B0 E8 ? ? ? ? 48 8D 55 C0 49 8B CE"),
			false
		);
		auto location = hook::get_pattern("41 80 7E ? ? 75 17");
		hook::nop(location, 0x7);
		hook::jump(location, stub.GetCode());
	}
}

static DWORD WINAPI Main()
{
	if (EnableLogging)
	{
		spdlog::set_default_logger(spdlog::basic_logger_mt("file_logger", "CScenarioPoint-Patch.log"));
		spdlog::flush_every(std::chrono::seconds(5));
	}
	else
	{
		spdlog::set_level(spdlog::level::off);
	}

	spdlog::info("Initializing MinHook...");
	MH_Initialize();
	
	FindGameFunctions();
	FindGameVariables();

	Patch1();
	Patch2();
	Patch3();
	Patch4();
	Patch5();
	Patch6();
	Patch7();
	Patch8();
	Patch9();
	Patch10();
	Patch11();
	Patch12();
	Patch13();
	Patch14();
	Patch15();
	Patch16();
	Patch17();
	Patch18();
	Patch19();
	Patch20();
	Patch21();
	Patch22();
	Patch23();
	Patch24();
	Patch25();
	Patch26();
	Patch27();
	Patch28();
	Patch29();
	Patch30();
	Patch31();
	Patch32();
	Patch33();
	Patch34();
	Patch35();
	Patch36();
	Patch37();
	Patch38();
	Patch39();
	Patch40();
	Patch41();
	Patch42();
	Patch43();
	Patch44();
	Patch45();
	Patch46();
	Patch47();
	Patch48();
	Patch49();
	Patch50();
	Patch51();
	Patch52();
	Patch53();

	MH_EnableHook(MH_ALL_HOOKS);

	spdlog::info("Initialization finished");
	return 1;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
	{
		CloseHandle(CreateThread(NULL, NULL, (LPTHREAD_START_ROUTINE)Main, NULL, NULL, NULL));
	}
	else if (ul_reason_for_call == DLL_PROCESS_DETACH)
	{
		spdlog::shutdown();
	}

	return TRUE;
}
