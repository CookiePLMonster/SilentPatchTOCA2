#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <ddraw.h>
#include <shellapi.h>

#include "Utils/MemoryMgr.h"
#include "Utils/Patterns.h"

#include <functional>
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
		if (details[0].m_carID < 8)
		{
			orgInitializeDecals();
		}
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

	// Fixed and customizable pause menu scale
	try
	{
		auto ctor_res_scale_x = get_pattern<int*>("A1 ? ? ? ? 56 33 F6 89 44 24 04", 1);
		auto ctor_res_scale_y = get_pattern<int*>("8B 0D ? ? ? ? DF 6C 24 04", 2);
		void* scale_values[] = {
			get_pattern("DC 0D ? ? ? ? 8B 4C 24 18", 2),
			get_pattern("DC 0D ? ? ? ? D9 1D ? ? ? ? E8 ? ? ? ? 89 35", 2),
		};
		void* cmp_1000[] = {
			get_pattern("BF ? ? ? ? C6 45 FC FF", 1),
			get_pattern("BF ? ? ? ? 39 3D ? ? ? ? 76 0F", 1),
		};

		Patch(ctor_res_scale_x, *ctor_res_scale_y);
		for (void* addr : scale_values)
		{
			Patch(addr, &GameMenuScale);
		}
		for (void* addr : cmp_1000)
		{
			Patch<uint32_t>(addr, 640);
		}
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
		auto cmp_1000_x = pattern("89 5C 24 6C BE ? ? ? ? 0F 85 2B 02 00 00 A1").get_one();

		Patch(cmp_1000_x.get<int*>(0xF + 1), *cmp_1000_x.get<int*>(0x60 + 1)); // Res scale X -> Res scale Y
		Nop(cmp_1000_x.get<void>(0x19), 2); // Scale X unconditionally
		Nop(cmp_1000_x.get<void>(0x6A), 2); // Scale Y unconditionally
		Patch(cmp_1000_x.get<void>(0x27 + 2), &GameMenuScale);
		Patch(cmp_1000_x.get<void>(0x78 + 2), &GameMenuScale);
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

	// Fix a crash when minimizing during a support car race
	// Windshield decals attempt to reinitialize when they shouldn't (those cars have no decals)
	try
	{
		using namespace DecalsCrashFix;

		auto init_decals = get_pattern("E8 ? ? ? ? E8 ? ? ? ? 85 C0 74 05 E8 ? ? ? ? E8 ? ? ? ? B8");
		auto cars_in_race_details = *get_pattern<CarDetails**>("8B 0D ? ? ? ? 8A 44 01 10", 2);

		ReadCall(init_decals, orgInitializeDecals);
		InjectHook(init_decals, InitializeDecals_IDCheck);
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
}