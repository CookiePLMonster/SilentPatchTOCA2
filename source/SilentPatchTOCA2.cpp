#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <ddraw.h>
#include <shellapi.h>
#include <Shlwapi.h>

#include "Utils/MemoryMgr.h"
#include "Utils/Patterns.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include <wrl/client.h>

#pragma comment(lib, "Shlwapi.lib")

BOOL UseMetric = TRUE;

bool ShowSteeringWheel = true;
bool ShowArms = true;
bool FullRangeSteeringAnims = false;
uint16_t InCarMirrorRes = 64;

struct ModelEntity
{
	std::byte gap[260];
	uint16_t m_flags;
};
static_assert(offsetof(ModelEntity, m_flags) == 0x104);

void ModelEntitySetFlags(ModelEntity* obj, BOOL set, uint16_t flags)
{
	if (set != FALSE)
	{
		obj->m_flags |= flags;
	}
	else
	{
		obj->m_flags &= ~flags;
	}
}

struct ArmsStruct
{
	int m_currentID;
	int16_t m_armsAngle;
	struct {
		int field_0;
		int field_4;
		int field_8;
		int field_C;
		int field_10;
		int field_14;
		int16_t field_18;
		int16_t field_1A;
		int16_t field_1C;
		int field_20;
		ModelEntity *model104;
		ModelEntity *model105;
		ModelEntity *model102;
		ModelEntity *model103;
		ModelEntity *model106;
		ModelEntity *model107;
		ModelEntity *leftArm;
		ModelEntity *rightArm;
		ModelEntity *model112;
		ModelEntity *model113;
		ModelEntity *steeringWheel;
		ModelEntity* dashboard;
		ModelEntity* model605;
		ModelEntity* gearKnob;
		ModelEntity *model110;
		int field_60;
		int field_64;
	} m_arm[4];
};

ArmsStruct* gArms;

bool* m_isWindowActive;
namespace Timers
{
	static constexpr int64_t TIME_MULT = 3276800;

	int* m_lastTick;
	int* m_currentTime;

	static int64_t timerDenominator;
	static int64_t lastTickTime;
	static int64_t lastTickRemainder;
	static bool resetTimers;
	void __stdcall InitTimers()
	{
		resetTimers = true;
		
		LARGE_INTEGER time;
		QueryPerformanceFrequency(&time);
		timerDenominator = time.QuadPart;

		*m_currentTime = 0;
		QueryPerformanceCounter(&time);
		lastTickTime = time.QuadPart;
		lastTickRemainder = 0;
	}

	void __stdcall TickTimers()
	{
		LARGE_INTEGER time;
		QueryPerformanceCounter(&time);
		int tickTime = 0;
		if (resetTimers)
		{
			resetTimers = false;
			tickTime = 65536;
		}
		else if (*m_isWindowActive)
		{
			int64_t diffTime = time.QuadPart - lastTickTime;
			diffTime *= TIME_MULT;
			auto divided = std::div(diffTime + lastTickRemainder, timerDenominator);
			tickTime = static_cast<int>(divided.quot);
			lastTickRemainder = divided.rem;
		}

		*m_lastTick = tickTime;
		*m_currentTime += tickTime;
		lastTickTime = time.QuadPart;
	}

	void __stdcall WaitTimer(int duration)
	{
		if (duration > 0)
		{
			int64_t diffTime;
			LARGE_INTEGER startTime;
			QueryPerformanceCounter(&startTime);
			do
			{
				LARGE_INTEGER time;
				QueryPerformanceCounter(&time);
				diffTime = time.QuadPart - startTime.QuadPart;
				diffTime *= TIME_MULT;
				diffTime /= timerDenominator;
			}
			while (diffTime < duration);
		}
	}
}

struct {
	uint32_t width, height;
} *m_currentRes;

namespace ResolutionList
{
	struct Resolution
	{
		Resolution(uint32_t width, uint32_t height, uint32_t bitness)
			: width(width), height(height), bitness(bitness)
		{
		}

		uint32_t width, height, bitness;
	};
	std::vector<Resolution> resolutionsList;
	BOOL __stdcall AddResolution(uint32_t width, uint32_t height, uint32_t bitness)
	{
		if (std::find_if(resolutionsList.begin(), resolutionsList.end(), [width, height](const Resolution& e) {
			return width == e.width && height == e.height;
		}) != resolutionsList.end()) return FALSE;

		resolutionsList.emplace_back(width, height, bitness);
		return TRUE;
	}

	BOOL __stdcall CurrentResolutionExists()
	{
		return std::find_if(resolutionsList.begin(), resolutionsList.end(), [](const Resolution& e) {
			return m_currentRes->width == e.width && m_currentRes->height == e.height;
		}) != resolutionsList.end() ? TRUE : FALSE;
	}

	BOOL __stdcall TrySetPreviousResolution()
	{
		auto it = std::find_if(resolutionsList.begin(), resolutionsList.end(), [](const Resolution& e) {
			return m_currentRes->width == e.width && m_currentRes->height == e.height;
		});
		if (it == resolutionsList.end()) return FALSE;

		if (it != resolutionsList.begin())
		{
			auto previous = std::prev(it);
			m_currentRes->width = previous->width;
			m_currentRes->height = previous->height;
			return TRUE;
		}
		return FALSE;
	}

	uint32_t __stdcall GetPackedResolution(int index)
	{
		auto& res = resolutionsList[index];
		return (res.width & 0xFFFF) | ((res.height & 0xFFFF) << 16);
	}

	uint32_t __stdcall GetNumResolutions()
	{
		return static_cast<uint32_t>(resolutionsList.size());
	}

	static HRESULT WINAPI EnumDisplayModeCB(LPDDSURFACEDESC pSurfaceDesc, LPVOID)
	{
		if (pSurfaceDesc->dwWidth >= 640 && pSurfaceDesc->dwHeight >= 480 && pSurfaceDesc->ddpfPixelFormat.dwBumpBitCount == 16)
		{
			AddResolution(pSurfaceDesc->dwWidth, pSurfaceDesc->dwHeight, 0);
		}
		return DDENUMRET_OK;
	}
}

namespace WidescreenFix
{
	// Cheap detour
	static void* SetViewport_ThunkEnd;
	static WRAPPER void __stdcall SetViewport_Thunk(int /*width*/, int /*unk1*/, int /*unk2*/, int /*unk3*/, int /*height*/, int /*unk4*/)
	{
		__asm
		{
			push ecx
			fild dword ptr [esp+4+14h]
			jmp  [SetViewport_ThunkEnd]
		}
	}

	static double FOVNormalMult = 1.0; // Arbitrary values, shown to the user as 70deg by default
	static double FOVDashboardMult = 1.0;
	uint32_t (__stdcall* GetCurrentCamera)(int camID);
	
	static float horizontalFOV = 2.0f;
	static float verticalFOV = 2.5f;
	void __stdcall SetViewport_CalculateAR(int width, int unk1, int unk2, int unk3, int height, int unk4)
	{
		// TODO: Adjustable FOV
		const double currentInvAR = static_cast<double>(m_currentRes->height) / m_currentRes->width;

		uint32_t camID = GetCurrentCamera(0);
		const double FOVMult = camID == 2 || camID == 4 ? FOVDashboardMult : FOVNormalMult;

		constexpr double AR_HOR_CONSTANT = 2.0 * 4.0 / 3.0; // 2.0f * (4/3)
		constexpr double AR_VERT_CONSTANT = 2.5;
		horizontalFOV = static_cast<float>(AR_HOR_CONSTANT * FOVMult * currentInvAR);
		verticalFOV = static_cast<float>(AR_VERT_CONSTANT * FOVMult);

		SetViewport_Thunk(width, unk1, unk2, unk3, height, unk4);
	}

	WRAPPER void MultByFOV()
	{
		__asm
		{
			fmul [horizontalFOV]
			fxch st(2)
			fmul [verticalFOV]
			retn
		}
	}
}

namespace DynamicAllocList
{
	uint32_t* m_currentAllocSize;
	void* (__stdcall* orgMaybeAlloc)(uint32_t size, uint8_t flags);

	static void* currentMemSpace; // At first it points at the game variable, then at currentDynamicAlloc
	static size_t currentAllocCapacity = 1024;
	static void* currentDynamicAlloc = nullptr;
	static std::function<void()> rePatchFunc;

	void* __stdcall MaybeAllocAndExpandArray(uint32_t size, uint8_t flags)
	{
		const uint32_t curIndexToUse = *m_currentAllocSize;
		if (curIndexToUse >= currentAllocCapacity)
		{
			// If it's the first time we reallocate, it'll redirect from the game variable to a custom allocation
			const size_t newCapacity = 2 * currentAllocCapacity;
			void* newMem = realloc(currentDynamicAlloc, sizeof(void*) * newCapacity);
			if (newMem != nullptr)
			{
				if (currentDynamicAlloc == nullptr)
				{
					// This is the first time allocating, copy the data over from the game allocation
					// Further reallocations will implicitly copy the data over
					void** src = static_cast<void**>(currentMemSpace);
					void** dest = static_cast<void**>(newMem);
					std::copy_n(src, currentAllocCapacity, dest);
				}

				void** mem = static_cast<void**>(newMem);
				std::fill(mem+currentAllocCapacity, mem+newCapacity, nullptr);

				currentDynamicAlloc = currentMemSpace = newMem;
				currentAllocCapacity = newCapacity;

				rePatchFunc();
			}
		}
		void* returnMem = orgMaybeAlloc(size, flags);
		return returnMem;
	}
}

LPDIRECTDRAW* g_pDirectDraw;
void (__stdcall* RegisterDestructor)(BOOL (__stdcall* func)(), const char* name);
namespace DynamicPalettesList
{
	using Microsoft::WRL::ComPtr;

	std::vector<ComPtr<IDirectDrawPalette>> createdPalettes;

	BOOL __stdcall PalettesDestructor()
	{
		createdPalettes.clear();
		return TRUE;
	}

	LPDIRECTDRAWPALETTE __stdcall CreateD3DPalette(PALETTEENTRY* entry)
	{
		const bool emptyContainer = createdPalettes.empty();
		if (emptyContainer)
		{
			createdPalettes.reserve(1024);
		}

		ComPtr<IDirectDrawPalette> palette;
		if (entry != nullptr)
		{
			(*g_pDirectDraw)->CreatePalette(DDPCAPS_8BIT|DDPCAPS_ALLOW256, entry, palette.GetAddressOf(), nullptr);
		}
		else
		{
			PALETTEENTRY dummyPalette;
			(*g_pDirectDraw)->CreatePalette(DDPCAPS_8BIT|DDPCAPS_ALLOW256, &dummyPalette, palette.GetAddressOf(), nullptr);
		}
		createdPalettes.push_back(palette);

		if (emptyContainer)
		{
			RegisterDestructor(PalettesDestructor, nullptr);
		}
		return palette.Get();
	}
}

static double HUDScale = 1.0/480.0;
static double GameMenuScale = 1.0/480.0;

namespace DecalsCrashFix
{
	struct CarDetails
	{
		std::byte pad[16];
		uint8_t m_carID;
		std::byte pad2[71];
	};
	static_assert(sizeof(CarDetails) == 88);
	static_assert(offsetof(CarDetails, m_carID) == 16);

	static CarDetails** gCarsInRaceDetails;
	static void (__stdcall* orgInitializeDecals)();
	void __stdcall InitializeDecals_IDCheck()
	{
		const CarDetails* details = *gCarsInRaceDetails;
		if (details != nullptr && details[0].m_carID < 8)
		{
			orgInitializeDecals();
		}
	}

	static void (__stdcall* orgSkinsLoad)();
	void __stdcall SkinsLoad_NullCheck()
	{
		if ((*gCarsInRaceDetails) != nullptr)
		{
			orgSkinsLoad();
		}
	}
}

static HMODULE hDLLModule;
static void ReadINI(uint16_t* pMirror, bool* pHookMetricImperial, bool* pForcedMirrors)
{
	wchar_t buffer[32];
	wchar_t wcModulePath[MAX_PATH];
	GetModuleFileNameW(hDLLModule, wcModulePath, _countof(wcModulePath) - 3); // Minus max required space for extension
	PathRenameExtensionW(wcModulePath, L".ini");

	auto convFOV = [](const wchar_t* buf) -> double {
		double userFOV = std::clamp(_wtof(buf), 30.0, 150.0);
		// Mappings:
		// 30 - 2.0f
		// 70 - 1.0f
		// 150 - 0.4f
		if (userFOV <= 70.0)
		{
			return 2.75 - (userFOV / 40.0);
		}
		return 1.525 - (0.0075 * userFOV);
	};

	GetPrivateProfileString(L"SilentPatch", L"HUDScale", L"1.0", buffer, _countof(buffer), wcModulePath);
	HUDScale = _wtof(buffer) / 480.0;

	GetPrivateProfileString(L"SilentPatch", L"MenuTextsScale", L"1.0", buffer, _countof(buffer), wcModulePath);
	GameMenuScale = _wtof(buffer) / 480.0;

	GetPrivateProfileString(L"SilentPatch", L"ExteriorFOV", L"70.0", buffer, _countof(buffer), wcModulePath);
	WidescreenFix::FOVNormalMult = convFOV(buffer);

	GetPrivateProfileString(L"SilentPatch", L"InteriorFOV", L"70.0", buffer, _countof(buffer), wcModulePath);
	WidescreenFix::FOVDashboardMult = convFOV(buffer);

	ShowSteeringWheel = GetPrivateProfileInt(L"SilentPatch", L"ShowSteeringWheel", TRUE, wcModulePath) != FALSE;
	ShowArms = GetPrivateProfileInt(L"SilentPatch", L"ShowArms", TRUE, wcModulePath) != FALSE;
	FullRangeSteeringAnims = GetPrivateProfileInt(L"SilentPatch", L"FullRangeSteeringAnims", FALSE, wcModulePath) != FALSE;

	if (pMirror)
	{
		UINT res = GetPrivateProfileInt(L"SilentPatch", L"MirrorResolution", 64, wcModulePath);
		// Must be in 64 - 512 ranges and a power of two
		res = std::clamp(res, 64u, 512u);
		res = 1 << static_cast<uint32_t>(std::floor(std::log2(res)));
	
		*pMirror = static_cast<uint16_t>(res);
	}

	{
		UINT units = GetPrivateProfileInt(L"SilentPatch", L"MeasurementUnits", -1, wcModulePath);
		if (pHookMetricImperial)
		{
			*pHookMetricImperial = units != -1;
		}
		if (units == 0)
		{
			// OS setting
			DWORD value;
			GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE|LOCALE_RETURN_NUMBER, (LPTSTR)&value, sizeof(value) / sizeof(WCHAR));
			UseMetric = value == 0;
		}
		else
		{
			UseMetric = units == 1;
		}
	}

	if (pForcedMirrors)
	{
		*pForcedMirrors = GetPrivateProfileInt(L"SilentPatch", L"ForceInteriorMirrors", -1, wcModulePath) != FALSE;
	}
}

static BOOL* bRequestsExit;
static LRESULT (CALLBACK* orgWindowProc)(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_CLOSE: // The game ignores WM_CLOSE, force it not to
		return DefWindowProcA(hwnd, uMsg, wParam, lParam);

	case WM_DESTROY:
		*bRequestsExit = TRUE;
		PostQuitMessage(0);
		return 0;
	
	case WM_ACTIVATE:
		if (wParam != WA_INACTIVE)
		{
			ReadINI(nullptr, nullptr, nullptr);
		}
		break;

	default:
		break;
	}
	return orgWindowProc(hwnd, uMsg, wParam, lParam);
}

ATOM WINAPI RegisterClassA_SetIconAndWndProc(WNDCLASSA* lpWndClass)
{
	lpWndClass->hIcon = ExtractIconW(GetModuleHandle(nullptr), L"toca2.exe", 0);
	orgWindowProc = std::exchange(lpWndClass->lpfnWndProc, WindowProc);
	return RegisterClassA(lpWndClass);
}
static auto* const pRegisterClassA_SetIconAndWndProc = &RegisterClassA_SetIconAndWndProc;

namespace MetricSwitch
{
	static void* fakeGamePtrForMetric;
}

namespace ForcedMirrors
{
	static BOOL ForcedMirror = TRUE;
	static void* fakeGamePtrForMirror;
}

namespace FullRangeSteeringAnim
{
	int (__stdcall* orgGetCurrentCamera)(int arg1);
	int __stdcall GetCurrentCamera_FakeInteriorCam(int arg1)
	{
		int result = orgGetCurrentCamera(arg1);
		return (FullRangeSteeringAnims && (result == 2 || result == 4)) ? 2 : result;
	}
}

namespace WheelArmsToggle
{
	void (__stdcall* RotatePart)(ModelEntity* entity, void* data);
	void __stdcall RotatePart_HideWheel(ModelEntity* entity, void* data)
	{
		ModelEntitySetFlags(entity, ShowSteeringWheel, 0xFFFF);
		if (ModelEntity* gearKnob = gArms->m_arm[gArms->m_currentID].gearKnob; gearKnob != nullptr)
		{
			ModelEntitySetFlags(gearKnob, ShowSteeringWheel, 0xFFFF);
		}
		if (ShowSteeringWheel)
		{
			RotatePart(entity, data);
		}
	}

	int (__stdcall* orgGetCurrentCamera)(int index);
	int __stdcall GetCurrentCamera_ToggleArms(int index)
	{
		const bool showArms = ShowArms && ShowSteeringWheel;
		if (ModelEntity* leftArm = gArms->m_arm[gArms->m_currentID].leftArm; leftArm != nullptr)
		{
			ModelEntitySetFlags(leftArm, showArms, 0xFFFF);
		}
		if (ModelEntity* rightArm = gArms->m_arm[gArms->m_currentID].rightArm; rightArm != nullptr)
		{
			ModelEntitySetFlags(rightArm, showArms, 0xFFFF);
		}
		return orgGetCurrentCamera(index);
	}
}

namespace MirrorQuality
{
	void (__stdcall* orgCreateViewport)(void* data, uint32_t x, uint32_t y, uint32_t width, uint32_t height, float multX, float multY);
	void __stdcall CreateViewport_InCarMirrorScale(void* data, uint32_t x, uint32_t y, uint32_t /*width*/, uint32_t /*height*/, float multX, float multY)
	{
		orgCreateViewport(data, x, y, InCarMirrorRes, InCarMirrorRes / 2, multX, multY);
	}

	void (__stdcall* orgSetViewportBounds)(void* data, const uint16_t rect1[4], const uint16_t rect2[4]);
	void __stdcall SetViewportBounds_InCarMirror(void* data, const uint16_t* /*rect1*/, const uint16_t* /*rect2*/)
	{
		const uint16_t rect[] = { 0, 0, InCarMirrorRes, static_cast<uint16_t>(InCarMirrorRes / 2) };
		orgSetViewportBounds(data, rect, rect);
	}
}

void OnInitializeHook()
{
	bool hookUnits = false, forcedMirrors = false;
	ReadINI(&InCarMirrorRes, &hookUnits, &forcedMirrors);

	std::unique_ptr<ScopedUnprotect::Unprotect> Protect = ScopedUnprotect::UnprotectSectionOrFullModule( GetModuleHandle( nullptr ), ".text" );

	using namespace Memory;
	using namespace hook::txn;

	// Timers rewritten for accuracy
	// Not locking up on modern CPUs, counting time backwards
	try
	{
		using namespace Timers;

		auto init_timers = get_pattern("83 EC 08 8D 44 24 00 50 FF 15 ? ? ? ? 8D 54 24 00");
		auto tick_timers = get_pattern("50 FF 15 ? ? ? ? A1 ? ? ? ? 85 C0", -7);
		auto wait_timer = get_pattern("53 56 57 50 33 FF FF 15 ? ? ? ?", -7);

		bool* isWindowActive = *get_pattern<bool*>("A1 ? ? ? ? 85 C0 75 04 33 C0", 1);
		int* lastTick = *get_pattern<int*>("A3 ? ? ? ? EB 3A", 1);
		int* currentTime = *get_pattern<int*>("8B 0D ? ? ? ? 8B 54 24 00", 2);

		m_isWindowActive = isWindowActive;
		m_lastTick = lastTick;
		m_currentTime = currentTime;

		InjectHook(init_timers, InitTimers, PATCH_JUMP);
		InjectHook(tick_timers, TickTimers, PATCH_JUMP);
		InjectHook(wait_timer, WaitTimer, PATCH_JUMP);
	}
	TXN_CATCH();

	// Unlimited resolutions list + allowed for all resolutions
	// Filtering out resolutions under 640x480
	try
	{
		using namespace ResolutionList;

		auto on_enum_resolution = get_pattern("68 ? ? ? ? 6A 00 6A 00 8B 08 6A 01", 1);
		auto res_exists = get_pattern("33 C9 56 85 D2", -6);
		auto try_set_previous_res = get_pattern("53 33 DB 33 C0", -6);
		auto get_packed_res = get_pattern("C1 E0 02 66 8B 88", -7);
		auto get_num_resolutions_ptr = get_pattern("E8 ? ? ? ? 68 ? ? ? ? 8B E8");

		auto current_resx = *get_pattern<decltype(m_currentRes)>("8B 3D ? ? ? ? B8 ? ? ? ? 3B 78 FC", 2);

		m_currentRes = current_resx;

		Patch(on_enum_resolution, EnumDisplayModeCB);
		InjectHook(res_exists, CurrentResolutionExists, PATCH_JUMP);
		InjectHook(try_set_previous_res, TrySetPreviousResolution, PATCH_JUMP);
		InjectHook(get_packed_res, GetPackedResolution, PATCH_JUMP);

		void* get_num_resolutions;
		ReadCall(get_num_resolutions_ptr, get_num_resolutions);
		InjectHook(get_num_resolutions, GetNumResolutions, PATCH_JUMP);
	}
	TXN_CATCH();

	// Arbitrary aspect ratio and FOV support
	try
	{
		using namespace WidescreenFix;

		auto set_viewport = pattern("DB 44 24 08 DB 44 24 0C D9 C2").get_one();
		auto calculate_fov = pattern("D8 74 24 00 D9 44 24 0C").get_one();
		auto get_current_camera_ptr = get_pattern("E8 ? ? ? ? 83 F8 04 75 1A");

		SetViewport_ThunkEnd = set_viewport.get<void>();
		InjectHook(set_viewport.get<void>(-5), SetViewport_CalculateAR, PATCH_JUMP);

		// Adjustable FOV
		ReadCall(get_current_camera_ptr, GetCurrentCamera);
		InjectHook(calculate_fov.get<void>(10), MultByFOV, PATCH_CALL);
		Nop(calculate_fov.get<void>(10 + 5), 5);
	}
	TXN_CATCH();

	// Fixed and customizable HUD scale
	try
	{
		auto cmp_1000 = get_pattern("3D ? ? ? ? 57 76 3D", 1);
		auto scale_values = pattern("DC 0D ? ? ? ? D9 C9 DC 0D ? ? ? ? D9 C9 D9 1D ? ? ? ? D9 1D ? ? ? ? EB 14").get_one();
		auto res_scale_x = get_pattern<int*>("A1 ? ? ? ? 83 EC 08 53", 1);
		auto res_scale_y = get_pattern<int*>("A1 ? ? ? ? 89 5C 24 14", 1);

		Patch<uint32_t>(cmp_1000, 480);
		Patch(scale_values.get<void>(2), &HUDScale);
		Patch(scale_values.get<void>(8 + 2), &HUDScale);

		Patch(res_scale_x, *res_scale_y);
	}
	TXN_CATCH();

	// Fixed and customizable pause menu scale
	try
	{
		auto ctor_res_scale_x = get_pattern<int*>("A1 ? ? ? ? 56 33 F6 89 44 24 04", 1);
		auto ctor_res_scale_y = get_pattern<int*>("8B 0D ? ? ? ? DF 6C 24 04", 2);
		void* scale_values[] = {
			get_pattern("DC 0D ? ? ? ? 8B 4C 24 18", 2),
			get_pattern("DC 0D ? ? ? ? D9 1D ? ? ? ? E8 ? ? ? ? 89 35", 2),
		};
		auto cmp_100 = pattern("BF E8 03 00 00").count(2);

		Patch(ctor_res_scale_x, *ctor_res_scale_y);
		for (void* addr : scale_values)
		{
			Patch(addr, &GameMenuScale);
		}
		cmp_100.for_each_result([](hook::pattern_match match)
		{
			Patch<uint32_t>(match.get<void>(1), 640);
		});
	}
	TXN_CATCH();

	// Fixed and customizable pre-race menu scale
	try
	{
		auto ctor_res_scale_x = get_pattern<int*>("A1 ? ? ? ? 83 EC 08 3D", 1);
		auto ctor_res_scale_y = get_pattern<int*>("89 44 24 00 A1", 4 + 1);
		auto scale_values = pattern("DF 6C 24 00 D9 C9").get_one();
		void* cmp_1000_y[] = {
			get_pattern("3D ? ? ? ? 76 43", 1),
		};
		void* cmp_1000_x[] = {
			get_pattern("81 3D ? ? ? ? ? ? ? ? 76 29", 6),
			get_pattern("81 3D ? ? ? ? ? ? ? ? 76 0F", 6),
		};

		Patch(ctor_res_scale_x, *ctor_res_scale_y);
		Patch(scale_values.get<void>(6 + 2), &GameMenuScale);
		Patch(scale_values.get<void>(14 + 2), &GameMenuScale);
		for (void* addr : cmp_1000_y)
		{
			Patch<uint32_t>(addr, 480);
		}
		for (void* addr : cmp_1000_x)
		{
			Patch<uint32_t>(addr, 640);
		}
	}
	TXN_CATCH();

	// Fixed and customizable loading screen text scale
	try
	{
		auto ctor_res_scale_x = get_pattern<int*>("66 89 44 24 ? A1 ? ? ? ? 56", 5 + 1);
		auto ctor_res_scale_y = get_pattern<int*>("76 30 8B 0D", 2 + 2);
		auto cmp_1000 = get_pattern("3D ? ? ? ? 57 66 89 6C 24", 1);
		auto scale_values = pattern("DC 0D ? ? ? ? D9 C9 DC 0D ? ? ? ? EB 0C").get_one();

		Patch(ctor_res_scale_x, *ctor_res_scale_y);
		Patch<uint32_t>(cmp_1000, 480);
		Patch(scale_values.get<void>(2), &GameMenuScale);
		Patch(scale_values.get<void>(8 + 2), &GameMenuScale);
	}
	TXN_CATCH();

	// Fixed and customizable post-race screen scale
	try
	{
		auto res_x_check = pattern("A1 ? ? ? ? 3D 00 04 00 00 76 1A").get_one();
		auto res_y_check = pattern("A1 ? ? ? ? 3D 00 03 00 00 76 1A").get_one();
		auto scales = pattern("DF 6C 24 18 DC 0D ? ? ? ? D9 1D ? ? ? ? EB 2B").count(2);

		Patch(res_x_check.get<int*>(1), *res_y_check.get<int*>(1)); // Res scale X -> Res scale Y
		Nop(res_x_check.get<void>(5 + 5), 2); // Scale X unconditionally
		Nop(res_y_check.get<void>(5 + 5), 2); // Scale Y unconditionally

		scales.for_each_result([](hook::pattern_match match)
		{
			Patch(match.get<void>(4 + 2), &GameMenuScale);
		});
	}
	TXN_CATCH();

	// Remove CD check
	try
	{
		auto cd_check = get_pattern("F3 A4 E8 ? ? ? ? 85 DB", 9);
		Nop(cd_check, 10);
	}
	TXN_CATCH();

	// Make the (presumably?) allocation list dynamic so it doesn't overflow
	// Fixes a crash when continuously minimizing and maximizing (+ ~50 allocations per maximize)
	try
	{
		using namespace DynamicAllocList;

		auto alloc_size_var = *get_pattern<uint32_t*>("89 35 ? ? ? ? 89 35 ? ? ? ? A3", 2);
		auto alloc_function = get_pattern("E8 ? ? ? ? 8B 0D ? ? ? ? 6A 00 50");

		uint32_t* alloc_sizes[] = {
			get_pattern<uint32_t>("B9 ? ? ? ? 33 C0 BF ? ? ? ? 33 F6 F3 AB B8", 1),
		};
		void** allocs_begin[] = {
			get_pattern<void*>("BF ? ? ? ? 33 F6 F3 AB B8", 1),
			get_pattern<void*>("BE ? ? ? ? 8B 06 85 C0 74 22", 1),

			get_pattern<void*>("50 89 04 8D", 3 + 1),
			get_pattern<void*>("89 54 24 68 8B 14 8D", 4 + 3),
			get_pattern<void*>("8B 04 85 ? ? ? ? 8B 08", 3),
			get_pattern<void*>("8B 14 8D ? ? ? ? 89 10", 3),
		};
		void** allocs_end[] = {
			get_pattern<void*>("81 FE ? ? ? ? 72 CD", 2),
		};

		ReadCall(alloc_function, orgMaybeAlloc);
		InjectHook(alloc_function, MaybeAllocAndExpandArray);
		m_currentAllocSize = alloc_size_var;
		currentMemSpace = *allocs_begin[0];

		rePatchFunc = [alloc_sizes, allocs_begin, allocs_end] {
			std::unique_ptr<ScopedUnprotect::Unprotect> Protect = ScopedUnprotect::UnprotectSectionOrFullModule( GetModuleHandle( nullptr ), ".text" );
			
			using namespace DynamicAllocList;

			void** mem = static_cast<void**>(currentMemSpace);

			for (uint32_t* addr : alloc_sizes)
			{
				Patch<uint32_t>(addr, currentAllocCapacity);
			}
			for (void** addr : allocs_begin)
			{
				Patch<void**>(addr, mem);	
			}
			for (void** addr : allocs_end)
			{
				Patch<void**>(addr, mem+currentAllocCapacity);	
			}
		};
	}
	TXN_CATCH();

	// Lift the 1024 palettes limit
	// Fixes a crash when minimizing excessively
	try
	{
		using namespace DynamicPalettesList;

		auto direct_draw_ptr = *get_pattern<LPDIRECTDRAW*>("A1 ? ? ? ? 33 FF 57", 1);
		auto register_destructor_func = static_cast<decltype(RegisterDestructor)>(get_pattern("3B 31 74 24", -0x21));

		auto create_palette_func = get_pattern("3D ? ? ? ? 73 6C", -0xB);

		g_pDirectDraw = direct_draw_ptr;
		RegisterDestructor = register_destructor_func;

		InjectHook(create_palette_func, CreateD3DPalette, PATCH_JUMP);
	}
	TXN_CATCH();

	// Fix a crash when minimizing during a support car race
	// Windshield decals attempt to reinitialize when they shouldn't (those cars have no decals)
	try
	{
		using namespace DecalsCrashFix;

		auto init_decals = pattern("E8 ? ? ? ? E8 ? ? ? ? 85 C0 74 05 E8 ? ? ? ? E8 ? ? ? ? B8").get_one();
		auto cars_in_race_details = *get_pattern<CarDetails**>("8B 0D ? ? ? ? 8A 44 01 10", 2);

		ReadCall(init_decals.get<void>(-5), orgSkinsLoad);
		InjectHook(init_decals.get<void>(-5), SkinsLoad_NullCheck);

		ReadCall(init_decals.get<void>(0), orgInitializeDecals);
		InjectHook(init_decals.get<void>(0), InitializeDecals_IDCheck);
		gCarsInRaceDetails = cars_in_race_details;
	}
	TXN_CATCH();

	// Take the process icon from toca2.exe
	// + overriden window proc
	try
	{
		auto register_class = get_pattern("FF 15 ? ? ? ? 66 85 C0", 2);
		auto requests_exit = *get_pattern<BOOL*>("A1 ? ? ? ? 85 C0 74 83", 1);

		bRequestsExit = requests_exit;
		Patch(register_class, &pRegisterClassA_SetIconAndWndProc);
	}
	TXN_CATCH();

	// Metric/imperial switch
	if (hookUnits)
	{
		try
		{
			using namespace MetricSwitch;

			void* addresses[] = {
				get_pattern("8B EC A1 ? ? ? ? 8B 90", 2 + 1), // Distance unit conversion
				get_pattern("83 EC 08 A1 ? ? ? ? 53 56", 3 + 1),
				get_pattern("89 43 EC A1", 3 + 1),
			};

			auto get_distance_unit_string = pattern("A1 ? ? ? ? 8B 88 ? ? ? ? B8").get_one();
			auto prepare_ui_data = pattern("A1 ? ? ? ? 8B 88 ? ? ? ? 85 C9 75 1C A1").count_hint(2); // 2 in 4.1, 1 in 1.0
			auto prepare_ui_data_10_only = pattern("39 9A ? ? ? ? 75 1C").count_hint(1); // 1.0 only, in 4.1 it shares the above pattern

			fakeGamePtrForMetric = reinterpret_cast<char*>(&UseMetric) - *get_distance_unit_string.get<uint32_t>(5 + 2);

			for (void* addr : addresses)
			{
				Patch(addr, &fakeGamePtrForMetric);
			}

			Patch(get_distance_unit_string.get<void>(1), &fakeGamePtrForMetric);

			prepare_ui_data.for_each_result([](hook::pattern_match match) {
				Patch(match.get<void>(1), &fakeGamePtrForMetric);	
			});
			prepare_ui_data_10_only.for_each_result([](hook::pattern_match match) {
				Patch(match.get<void>(1), &fakeGamePtrForMetric);	
			});
		}
		TXN_CATCH();
	}

	// Forced in-car rear view mirrors
	if (forcedMirrors)
	{
		try
		{
			using namespace ForcedMirrors;

			void* addresses[] = {
				get_pattern("8B 46 04 8B 15", 3 + 2),
				get_pattern("8B 15 ? ? ? ? 8A 8A", 2),
			};

			void* short_jmps[] = {
				get_pattern("8B 14 AD ? ? ? ? 85 D2", -2),
			};

			const std::pair<void*, size_t> nops[] = {
				{ get_pattern("8B 04 AD ? ? ? ? 85 C0", -2), 2 },
				{ get_pattern("A1 ? ? ? ? 3B F3", -0x28), 6 }, // Unknown
				{ get_pattern("0F BE BE ? ? ? ? 38 9A", 7 + 6), 6 },
				{ get_pattern("0F 84 ? ? ? ? 3A CB", 6 + 2), 6 },
			};

			auto mov_dl_1_nop = pattern("33 C9 84 D2 5F").get_one();

			uint32_t offset = *get_pattern<uint32_t>("8A 8A ? ? ? ? 84 C9", 2);
			fakeGamePtrForMirror = reinterpret_cast<char*>(&ForcedMirror) - offset;

			// mov dl, 1
			// nop
			Patch(mov_dl_1_nop.get<void>(-6), { 0xB2, 0x01 });
			Nop(mov_dl_1_nop.get<void>(-4), 4);

			for (void* addr : addresses)
			{
				Patch(addr, &fakeGamePtrForMirror);
			}

			for (void* addr : short_jmps)
			{
				Patch<uint8_t>(addr, 0xEB);
			}

			for (auto& addr : nops)
			{
				Nop(addr.first, addr.second);
			}
		}
		TXN_CATCH();
	}

	// Full range steering & gear shifting anim
	// when using the center interior cam
	try
	{
		using namespace FullRangeSteeringAnim;

		auto arms_animate = get_pattern("E8 ? ? ? ? 83 F8 02 75 60");
		auto dashboard_update = get_pattern("E8 ? ? ? ? 83 F8 04 75 26");

		ReadCall(arms_animate, orgGetCurrentCamera);
		InjectHook(arms_animate, GetCurrentCamera_FakeInteriorCam);
		InjectHook(dashboard_update, GetCurrentCamera_FakeInteriorCam);
	}
	TXN_CATCH();

	// Options to hide the steering wheel and arms
	try
	{
		using namespace WheelArmsToggle;

		auto rotate_wheel = get_pattern("66 89 3D ? ? ? ? E8", 7);
		auto animate_arms_get_cam = get_pattern("E8 ? ? ? ? 83 F8 02 75 60");

		auto arms = *get_pattern<ArmsStruct*>("8B 0D ? ? ? ? 6A 69", 2);

		gArms = arms;

		ReadCall(rotate_wheel, RotatePart);
		InjectHook(rotate_wheel, RotatePart_HideWheel);

		ReadCall(animate_arms_get_cam, orgGetCurrentCamera);
		InjectHook(animate_arms_get_cam, GetCurrentCamera_ToggleArms);
	}
	TXN_CATCH();

	// Mirror quality setting for the in-car mirror
	// Default size is 64x32
	try
	{
		using namespace MirrorQuality;

		auto create_mirror_rt = get_pattern("E8 ? ? ? ? 8B C3 68");
		auto set_mirror_bounds = get_pattern("8D 54 24 1C 8D 44 24 24 52 50 51 E8", 11);

		auto d3d_resources_ptr = *get_pattern<void*>("68 ? ? ? ? E8 ? ? ? ? 8D 54 24 14", 1);
		auto mirror_surface_id = *get_pattern<uint32_t>("68 ? ? ? ? E8 ? ? ? ? A3 ? ? ? ? E8 ? ? ? ? E8 ? ? ? ? E8", 1);

		ReadCall(create_mirror_rt, orgCreateViewport);
		InjectHook(create_mirror_rt, CreateViewport_InCarMirrorScale);

		ReadCall(set_mirror_bounds, orgSetViewportBounds);
		InjectHook(set_mirror_bounds, SetViewportBounds_InCarMirror);

		void* entriesInfoPtr = *reinterpret_cast<void**>(static_cast<char*>(d3d_resources_ptr) + 16);
		void* mirrorEntry = static_cast<char*>(entriesInfoPtr) + 1132*mirror_surface_id;
		uint16_t* size = reinterpret_cast<uint16_t*>(static_cast<char*>(mirrorEntry) + 4);
		*size = InCarMirrorRes;
	}
	TXN_CATCH();
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	UNREFERENCED_PARAMETER(lpvReserved);

	if (fdwReason == DLL_PROCESS_ATTACH)
	{
		hDLLModule = hinstDLL;
	}
	return TRUE;
}
