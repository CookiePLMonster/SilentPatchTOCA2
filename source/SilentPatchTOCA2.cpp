#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include "Utils/MemoryMgr.h"
#include "Utils/Patterns.h"

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
}