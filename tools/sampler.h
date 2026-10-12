#pragma once
// A small sampling profiler for the offline test (SYSTEMTEST_SAMPLE=1): a second thread suspends the
// test's thread every ~0.1 ms, records its instruction pointer, and at the end the samples are
// resolved to function names with the PDB (the test links with /DEBUG). Exclusive time only: which
// functions the time is spent in, not who called them.
#include <windows.h>
#include <dbghelp.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "dbghelp.lib")

namespace sampler
{
	inline std::atomic<bool> gRun{ false };
	inline std::atomic<bool> gActive{ false };   // set by the test around the physics step
	inline std::vector<DWORD> gSamples;
	inline std::thread gThread;
	inline HANDLE gTarget = nullptr;

	inline void Start()
	{
		DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &gTarget, 0, FALSE, DUPLICATE_SAME_ACCESS);
		gRun = true;
		gThread = std::thread([] {
			while (gRun) {
				if (gActive && SuspendThread(gTarget) != (DWORD)-1) {
					CONTEXT c{};
					c.ContextFlags = CONTEXT_CONTROL;
					if (GetThreadContext(gTarget, &c))
						gSamples.push_back(c.Eip);
					ResumeThread(gTarget);
				}
				// Busy-wait ~0.1 ms between samples (Sleep's granularity is 1 ms).
				LARGE_INTEGER f, t0, t1;
				QueryPerformanceFrequency(&f);
				QueryPerformanceCounter(&t0);
				do QueryPerformanceCounter(&t1); while ((t1.QuadPart - t0.QuadPart) * 10000 < f.QuadPart);
			}
		});
	}

	inline void Report(int top = 30)
	{
		gRun = false;
		gThread.join();
		HANDLE proc = GetCurrentProcess();
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		SymInitialize(proc, nullptr, TRUE);
		std::map<std::string, int> byFunc;
		alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
		for (DWORD ip : gSamples) {
			auto* sym = (SYMBOL_INFO*)buf;
			sym->SizeOfStruct = sizeof(SYMBOL_INFO);
			sym->MaxNameLen = 511;
			DWORD64 disp = 0;
			std::string name = SymFromAddr(proc, ip, &disp, sym) ? sym->Name : "(unknown)";
			byFunc[name]++;
		}
		std::vector<std::pair<int, std::string>> sorted;
		for (auto& [n, c] : byFunc) sorted.push_back({ c, n });
		std::sort(sorted.rbegin(), sorted.rend());
		printf("samples: %zu\n", gSamples.size());
		for (int i = 0; i < top && i < (int)sorted.size(); i++)
			printf("  %5.1f%%  %s\n", 100.0 * sorted[i].first / gSamples.size(), sorted[i].second.c_str());
		SymCleanup(proc);
	}
}
