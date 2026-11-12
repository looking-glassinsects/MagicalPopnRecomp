/* Sampling profiler for test builds (compile main.c with -DPOPN_PROF, run with POPN_PROF=outfile):
 * a second thread records where the main thread is; resolve the addresses with a linker map. */
#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
static HANDLE prof_target; static volatile int prof_run; static DWORD64 *prof_buf; static volatile LONG prof_n; static int prof_cap;
static DWORD WINAPI prof_thread(void *a) { (void)a; CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
    while (prof_run) { Sleep(1); SuspendThread(prof_target); if (GetThreadContext(prof_target, &c)) { LONG i = prof_n; if (i < prof_cap) { prof_buf[i] = c.Rip; prof_n = i + 1; } } ResumeThread(prof_target); }
    return 0; }
static void prof_start(void) { timeBeginPeriod(1); prof_cap = 4000000; prof_buf = malloc(prof_cap * 8); prof_run = 1;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &prof_target, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, prof_thread, NULL, 0, NULL); }
static void prof_stop(const char *path) { prof_run = 0; Sleep(20); FILE *f = fopen(path, "wb"); if (f) { DWORD64 base = (DWORD64)GetModuleHandleA(NULL); fwrite(&base, 8, 1, f); fwrite(prof_buf, 8, prof_n, f); fclose(f); } }
