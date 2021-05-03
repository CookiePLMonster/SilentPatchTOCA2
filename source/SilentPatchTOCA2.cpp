#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <ddraw.h>

#include "Utils/MemoryMgr.h"
#include "Utils/Patterns.h"

#include <vector>

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

	static float horizontalFOV = 2.0f;
	static float verticalFOV = 2.5f;
	void __stdcall SetViewport_CalculateAR(int width, int unk1, int unk2, int unk3, int height, int unk4)
	{
		// TODO: Adjustable FOV
		const double currentInvAR = static_cast<double>(m_currentRes->height) / m_currentRes->width;

		constexpr double AR_CONSTANT = 2.0 * 4.0 / 3.0; // 2.0f * (4/3)
		horizontalFOV = static_cast<float>(AR_CONSTANT * currentInvAR);

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

static double HUDScale = 1.0/480.0;

void OnInitializeHook()
{
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

		SetViewport_ThunkEnd = set_viewport.get<void>();
		InjectHook(set_viewport.get<void>(-5), SetViewport_CalculateAR, PATCH_JUMP);

		// Adjustable FOV
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
}