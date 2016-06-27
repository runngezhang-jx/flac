/* libFLAC - Free Lossless Audio Codec library
 * Copyright (C) 2001-2009  Josh Coalson
 * Copyright (C) 2011-2014  Xiph.Org Foundation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * - Neither the name of the Xiph.org Foundation nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include "private/cpu.h"
#include "share/compat.h"
#include <stdlib.h>
#include <memory.h>

#if defined (__NetBSD__) || defined(__OpenBSD__)
#  include <sys/param.h>
#  include <sys/sysctl.h>
#  include <machine/cpu.h>
#endif

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__) || defined(__APPLE__)
#  include <sys/types.h>
#  include <sys/sysctl.h>
#endif

#if  defined(__linux__) && defined(FLAC__CPU_IA32) && !defined FLAC__NO_ASM && (defined FLAC__HAS_NASM || FLAC__HAS_X86INTRIN) && !FLAC__SSE_OS
#  include <sys/ucontext.h>
#endif

#if defined(_MSC_VER)
#  include <windows.h>
#  include <intrin.h> /* for __cpuid() and _xgetbv() */
#endif

#if defined __GNUC__ && defined HAVE_CPUID_H
#  include <cpuid.h> /* for __get_cpuid() and __get_cpuid_max() */
#endif

#ifdef DEBUG
#include <stdio.h>

#define dfprintf fprintf
#else
/* This is bad practice, it should be a static void empty function */
#define dfprintf(file, format, args...)
#endif


/* these are flags in EDX of CPUID AX=00000001 */
static const unsigned FLAC__CPUINFO_IA32_CPUID_CMOV = 0x00008000;
static const unsigned FLAC__CPUINFO_IA32_CPUID_MMX = 0x00800000;
static const unsigned FLAC__CPUINFO_IA32_CPUID_FXSR = 0x01000000;
static const unsigned FLAC__CPUINFO_IA32_CPUID_SSE = 0x02000000;
static const unsigned FLAC__CPUINFO_IA32_CPUID_SSE2 = 0x04000000;

/* these are flags in ECX of CPUID AX=00000001 */
static const unsigned FLAC__CPUINFO_IA32_CPUID_SSE3 = 0x00000001;
static const unsigned FLAC__CPUINFO_IA32_CPUID_SSSE3 = 0x00000200;
static const unsigned FLAC__CPUINFO_IA32_CPUID_SSE41 = 0x00080000;
static const unsigned FLAC__CPUINFO_IA32_CPUID_SSE42 = 0x00100000;

/* these are flags in ECX of CPUID AX=00000001 */
static const unsigned FLAC__CPUINFO_IA32_CPUID_OSXSAVE = 0x08000000;
static const unsigned FLAC__CPUINFO_IA32_CPUID_AVX = 0x10000000;
static const unsigned FLAC__CPUINFO_IA32_CPUID_FMA = 0x00001000;
/* these are flags in EBX of CPUID AX=00000007 */
static const unsigned FLAC__CPUINFO_IA32_CPUID_AVX2 = 0x00000020;

/*
 * Extra stuff needed for detection of OS support for SSE on IA-32
 */
#if  defined(__linux__) && defined(FLAC__CPU_IA32) && !defined FLAC__NO_ASM && (defined FLAC__HAS_NASM || FLAC__HAS_X86INTRIN) && !FLAC__SSE_OS
/*
 * If the OS doesn't support SSE, we will get here with a SIGILL.  We
 * modify the return address to jump over the offending SSE instruction
 * and also the operation following it that indicates the instruction
 * executed successfully.  In this way we use no global variables and
 * stay thread-safe.
 *
 * 3 + 3 + 6:
 *   3 bytes for "xorps xmm0,xmm0"
 *   3 bytes for estimate of how long the follwing "inc var" instruction is
 *   6 bytes extra in case our estimate is wrong
 * 12 bytes puts us in the NOP "landing zone"
 */
static void sigill_handler_sse_os(int signal, siginfo_t *si, void *uc)
{
	(void)signal, (void)si;
	((ucontext_t*)uc)->uc_mcontext.gregs[14/*REG_EIP*/] += 3 + 3 + 6;
}
#endif

#if defined FLAC__CPU_IA32
static void
ia32_disable_sse(FLAC__CPUInfo *info)
{
	info->ia32.sse   = false;
	info->ia32.sse2  = false;
	info->ia32.sse3  = false;
	info->ia32.ssse3 = false;
	info->ia32.sse41 = false;
	info->ia32.sse42 = false;
}

static void
ia32_disable_avx(FLAC__CPUInfo *info)
{
	info->ia32.avx     = false;
	info->ia32.avx2    = false;
	info->ia32.fma     = false;
}
#endif

#if defined FLAC__CPU_X86_64
static void
x86_64_disable_avx(FLAC__CPUInfo *info)
{
	info->x86.avx     = false;
	info->x86.avx2    = false;
	info->x86.fma     = false;
}
#endif

#if defined FLAC__CPU_IA32 || defined FLAC__CPU_X86_64
static uint32_t
cpu_xgetbv_x86(void)
{
#if (defined _MSC_VER || defined __INTEL_COMPILER) && FLAC__HAS_X86INTRIN && FLAC__AVX_SUPPORTED
	return (uint32_t)_xgetbv(0);
#elif defined __GNUC__
	uint32_t lo, hi;
	asm volatile (".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c" (0));
	return lo;
#else
	return 0;
#endif
}
#endif

static void
ia32_cpu_info (FLAC__CPUInfo *info)
{
#if !defined FLAC__CPU_IA32
	(void) info;
#elif defined(__ANDROID__) || defined(ANDROID)
	/* no need to check OS SSE support */
	info->use_asm = true;
#else
	FLAC__bool ia32_fxsr = false;
	FLAC__bool ia32_osxsave = false;
	FLAC__uint32 flags_eax, flags_ebx, flags_ecx, flags_edx;

#if !defined FLAC__NO_ASM && (defined FLAC__HAS_NASM || FLAC__HAS_X86INTRIN)
	info->use_asm = true; /* we assume a minimum of 80386 with FLAC__CPU_IA32 */
#if !FLAC__HAS_X86INTRIN
	if(!FLAC__cpu_have_cpuid_asm_ia32())
		return;
#endif
	/* http://www.sandpile.org/x86/cpuid.htm */
    if (FLAC__HAS_X86INTRIN) {
		FLAC__cpu_info_x86(0, &flags_eax, &flags_ebx, &flags_ecx, &flags_edx);
		info->ia32.intel = (flags_ebx == 0x756E6547 && flags_edx == 0x49656E69 && flags_ecx == 0x6C65746E) ? true : false; /* GenuineIntel */
		FLAC__cpu_info_x86(1, &flags_eax, &flags_ebx, &flags_ecx, &flags_edx);
	}
	else {
		FLAC__cpu_info_asm_ia32(&flags_edx, &flags_ecx);
	}

	info->ia32.cmov  = (flags_edx & FLAC__CPUINFO_IA32_CPUID_CMOV ) ? true : false;
	info->ia32.mmx   = (flags_edx & FLAC__CPUINFO_IA32_CPUID_MMX  ) ? true : false;
	      ia32_fxsr  = (flags_edx & FLAC__CPUINFO_IA32_CPUID_FXSR ) ? true : false;
	info->ia32.sse   = (flags_edx & FLAC__CPUINFO_IA32_CPUID_SSE  ) ? true : false;
	info->ia32.sse2  = (flags_edx & FLAC__CPUINFO_IA32_CPUID_SSE2 ) ? true : false;
	info->ia32.sse3  = (flags_ecx & FLAC__CPUINFO_IA32_CPUID_SSE3 ) ? true : false;
	info->ia32.ssse3 = (flags_ecx & FLAC__CPUINFO_IA32_CPUID_SSSE3) ? true : false;
	info->ia32.sse41 = (flags_ecx & FLAC__CPUINFO_IA32_CPUID_SSE41) ? true : false;
	info->ia32.sse42 = (flags_ecx & FLAC__CPUINFO_IA32_CPUID_SSE42) ? true : false;

    if (FLAC__HAS_X86INTRIN && FLAC__AVX_SUPPORTED) {
	    ia32_osxsave = (flags_ecx & FLAC__CPUINFO_IA32_CPUID_OSXSAVE) ? true : false;
		info->ia32.avx   = (flags_ecx & FLAC__CPUINFO_IA32_CPUID_AVX    ) ? true : false;
		info->ia32.fma   = (flags_ecx & FLAC__CPUINFO_IA32_CPUID_FMA    ) ? true : false;
		FLAC__cpu_info_x86(7, &flags_eax, &flags_ebx, &flags_ecx, &flags_edx);
		info->ia32.avx2  = (flags_ebx & FLAC__CPUINFO_IA32_CPUID_AVX2   ) ? true : false;
	}

	dfprintf(stderr, "CPU info (IA-32):\n");
	dfprintf(stderr, "  CMOV ....... %c\n", info->ia32.cmov    ? 'Y' : 'n');
	dfprintf(stderr, "  MMX ........ %c\n", info->ia32.mmx     ? 'Y' : 'n');
	dfprintf(stderr, "  SSE ........ %c\n", info->ia32.sse     ? 'Y' : 'n');
	dfprintf(stderr, "  SSE2 ....... %c\n", info->ia32.sse2    ? 'Y' : 'n');
	dfprintf(stderr, "  SSE3 ....... %c\n", info->ia32.sse3    ? 'Y' : 'n');
	dfprintf(stderr, "  SSSE3 ...... %c\n", info->ia32.ssse3   ? 'Y' : 'n');
	dfprintf(stderr, "  SSE41 ...... %c\n", info->ia32.sse41   ? 'Y' : 'n');
	dfprintf(stderr, "  SSE42 ...... %c\n", info->ia32.sse42   ? 'Y' : 'n');

	if (FLAC__HAS_X86INTRIN && FLAC__AVX_SUPPORTED) {
		dfprintf(stderr, "  AVX ........ %c\n", info->ia32.avx     ? 'Y' : 'n');
		dfprintf(stderr, "  FMA ........ %c\n", info->ia32.fma     ? 'Y' : 'n');
		dfprintf(stderr, "  AVX2 ....... %c\n", info->ia32.avx2    ? 'Y' : 'n');
	}

	/*
	 * now have to check for OS support of SSE instructions
	 */
	if(info->ia32.sse) {
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__) || defined(__APPLE__)
		int sse = 0;
		size_t len = sizeof(sse);
		/* at least one of these must work: */
		sse = sse || (sysctlbyname("hw.instruction_sse", &sse, &len, NULL, 0) == 0 && sse);
		sse = sse || (sysctlbyname("hw.optional.sse"   , &sse, &len, NULL, 0) == 0 && sse); /* __APPLE__ ? */
		if(!sse)
			ia32_disable_sse(info);
#elif defined(__NetBSD__) || defined (__OpenBSD__)
		int val = 0, mib[2] = { CTL_MACHDEP, CPU_SSE };
		size_t len = sizeof(val);
		if(sysctl(mib, 2, &val, &len, NULL, 0) < 0 || !val)
			ia32_disable_sse(info);
		else { /* double-check SSE2 */
			mib[1] = CPU_SSE2;
			len = sizeof(val);
			if(sysctl(mib, 2, &val, &len, NULL, 0) < 0 || !val) {
				ia32_disable_sse(info);
				info->ia32.sse = true;
			}
		}
#elif defined(__linux__) && !FLAC__SSE_OS
		int sse = 0;
		struct sigaction sigill_save;
		struct sigaction sigill_sse;
		sigill_sse.sa_sigaction = sigill_handler_sse_os;
		sigemptyset(&sigill_sse.sa_mask);
		sigill_sse.sa_flags = SA_SIGINFO | SA_RESETHAND; /* SA_RESETHAND just in case our SIGILL return jump breaks, so we don't get stuck in a loop */
		if(0 == sigaction(SIGILL, &sigill_sse, &sigill_save))
		{
			/* http://www.ibiblio.org/gferg/ldp/GCC-Inline-Assembly-HOWTO.html */
			/* see sigill_handler_sse_os() for an explanation of the following: */
			asm volatile (
				"xorps %%xmm0,%%xmm0\n\t" /* will cause SIGILL if unsupported by OS */
				"incl %0\n\t"             /* SIGILL handler will jump over this */
				/* landing zone */
				"nop\n\t" /* SIGILL jump lands here if "inc" is 9 bytes */
				"nop\n\t"
				"nop\n\t"
				"nop\n\t"
				"nop\n\t"
				"nop\n\t"
				"nop\n\t" /* SIGILL jump lands here if "inc" is 3 bytes (expected) */
				"nop\n\t"
				"nop"     /* SIGILL jump lands here if "inc" is 1 byte */
				: "=r"(sse)
				: "0"(sse)
			);

			sigaction(SIGILL, &sigill_save, NULL);
		}

		if(!sse)
			ia32_disable_sse(info);
#elif defined(_MSC_VER)
		__try {
			__asm {
				xorps xmm0,xmm0
			}
		}
		__except(EXCEPTION_EXECUTE_HANDLER) {
			if (_exception_code() == STATUS_ILLEGAL_INSTRUCTION)
				ia32_disable_sse(info);
		}
#elif defined(__GNUC__) /* MinGW goes here */
		int sse = 0;
		/* Based on the idea described in Agner Fog's manual "Optimizing subroutines in assembly language" */
		/* In theory, not guaranteed to detect lack of OS SSE support on some future Intel CPUs, but in practice works (see the aforementioned manual) */
		if (ia32_fxsr) {
			struct {
				FLAC__uint32 buff[128];
			} __attribute__((aligned(16))) fxsr;
			FLAC__uint32 old_val, new_val;

			memset(fxsr.buff, 0, sizeof (fxsr.buff));

			asm volatile ("fxsave %0"  : "=m" (fxsr) : "m" (fxsr));
			old_val = fxsr.buff[50];
			fxsr.buff[50] ^= 0x0013c0de;                             /* change value in the buffer */
			asm volatile ("fxrstor %0" : "=m" (fxsr) : "m" (fxsr));  /* try to change SSE register */
			fxsr.buff[50] = old_val;                                 /* restore old value in the buffer */
			asm volatile ("fxsave %0"  : "=m" (fxsr) : "m" (fxsr));  /* old value will be overwritten if SSE register was changed */
			new_val = fxsr.buff[50];                                 /* == old_val if FXRSTOR didn't change SSE register and (old_val ^ 0x0013c0de) otherwise */
			fxsr.buff[50] = old_val;                                 /* again restore old value in the buffer */
			asm volatile ("fxrstor %0" : "=m" (fxsr) : "m" (fxsr));  /* restore old values of registers */

			if ((old_val^new_val) == 0x0013c0de)
				sse = 1;
		}
		if(!sse)
			ia32_disable_sse(info);
#else
		/* no way to test, disable to be safe */
		ia32_disable_sse(info);
#endif
		dfprintf(stderr, "  SSE OS sup . %c\n", info->ia32.sse ? 'Y' : 'n');
	}
	else /* info->ia32.sse == false */
		ia32_disable_sse(info);

	/*
	 * now have to check for OS support of AVX instructions
	 */
	if (FLAC__HAS_X86INTRIN && info->ia32.avx && ia32_osxsave) {
		FLAC__uint32 ecr = cpu_xgetbv_x86();
		if ((ecr & 0x6) != 0x6)
			ia32_disable_avx(info);

		dfprintf(stderr, "  AVX OS sup . %c\n", info->ia32.avx ? 'Y' : 'n');
	}
	else /* no OS AVX support */
		ia32_disable_avx(info);

#else
	info->use_asm = false;
#endif
#endif
}

static void
x86_64_cpu_info (FLAC__CPUInfo *info)
{
#if !defined FLAC__CPU_X86_64
	(void) info;
#elif defined(__ANDROID__) || defined(ANDROID)
	/* no need to check OS SSE support */
	info->use_asm = true;
#elif !defined FLAC__NO_ASM && FLAC__HAS_X86INTRIN
	FLAC__bool x86_osxsave = false;
	FLAC__uint32 flags_eax, flags_ebx, flags_ecx, flags_edx;

	info->use_asm = true;

	/* http://www.sandpile.org/x86/cpuid.htm */
	FLAC__cpu_info_x86(0, &flags_eax, &flags_ebx, &flags_ecx, &flags_edx);
	info->x86.intel = (flags_ebx == 0x756E6547 && flags_edx == 0x49656E69 && flags_ecx == 0x6C65746E) ? true : false; /* GenuineIntel */
	FLAC__cpu_info_x86(1, &flags_eax, &flags_ebx, &flags_ecx, &flags_edx);
	info->x86.sse3  = (flags_ecx & FLAC__CPUINFO_IA32_CPUID_SSE3 ) ? true : false;
	info->x86.ssse3 = (flags_ecx & FLAC__CPUINFO_IA32_CPUID_SSSE3) ? true : false;
	info->x86.sse41 = (flags_ecx & FLAC__CPUINFO_IA32_CPUID_SSE41) ? true : false;
	info->x86.sse42 = (flags_ecx & FLAC__CPUINFO_IA32_CPUID_SSE42) ? true : false;

	if (FLAC__AVX_SUPPORTED) {
	    x86_osxsave = (flags_ecx & FLAC__CPUINFO_IA32_CPUID_OSXSAVE) ? true : false;
		info->x86.avx   = (flags_ecx & FLAC__CPUINFO_IA32_CPUID_AVX    ) ? true : false;
		info->x86.fma   = (flags_ecx & FLAC__CPUINFO_IA32_CPUID_FMA    ) ? true : false;
		FLAC__cpu_info_x86(7, &flags_eax, &flags_ebx, &flags_ecx, &flags_edx);
		info->x86.avx2  = (flags_ebx & FLAC__CPUINFO_IA32_CPUID_AVX2   ) ? true : false;
	}

	dfprintf(stderr, "CPU info (x86-64):\n");
	dfprintf(stderr, "  SSE3 ....... %c\n", info->x86.sse3  ? 'Y' : 'n');
	dfprintf(stderr, "  SSSE3 ...... %c\n", info->x86.ssse3 ? 'Y' : 'n');
	dfprintf(stderr, "  SSE41 ...... %c\n", info->x86.sse41 ? 'Y' : 'n');
	dfprintf(stderr, "  SSE42 ...... %c\n", info->x86.sse42 ? 'Y' : 'n');

	if (FLAC__AVX_SUPPORTED) {
		dfprintf(stderr, "  AVX ........ %c\n", info->x86.avx   ? 'Y' : 'n');
		dfprintf(stderr, "  FMA ........ %c\n", info->x86.fma   ? 'Y' : 'n');
		dfprintf(stderr, "  AVX2 ....... %c\n", info->x86.avx2  ? 'Y' : 'n');
	}

	/*
	 * now have to check for OS support of AVX instructions
	 */
	if (info->x86.avx && x86_osxsave) {
		FLAC__uint32 ecr = cpu_xgetbv_x86();
		if ((ecr & 0x6) != 0x6)
			x86_64_disable_avx(info);

		dfprintf(stderr, "  AVX OS sup . %c\n", info->x86.avx ? 'Y' : 'n');
	}
	else /* no OS AVX support */
		x86_64_disable_avx(info);
#endif
}

void FLAC__cpu_info (FLAC__CPUInfo *info)
{
	memset(info, 0, sizeof(*info));

#ifdef FLAC__CPU_IA32
	info->type = FLAC__CPUINFO_TYPE_IA32;
#elif defined FLAC__CPU_X86_64
	info->type = FLAC__CPUINFO_TYPE_X86_64;
#else
	info->type = FLAC__CPUINFO_TYPE_UNKNOWN;
	info->use_asm = false;
#endif

	switch (info->type) {
	case FLAC__CPUINFO_TYPE_IA32:
		ia32_cpu_info (info);
		break;
	case FLAC__CPUINFO_TYPE_X86_64:
		x86_64_cpu_info (info);
		break;
	default:
		info->use_asm = false;
		break;
	}
}

#if (defined FLAC__CPU_IA32 || defined FLAC__CPU_X86_64) && FLAC__HAS_X86INTRIN

void FLAC__cpu_info_x86(FLAC__uint32 level, FLAC__uint32 *eax, FLAC__uint32 *ebx, FLAC__uint32 *ecx, FLAC__uint32 *edx)
{
#if defined _MSC_VER || defined __INTEL_COMPILER
	int cpuinfo[4];
	int ext = level & 0x80000000;
	__cpuid(cpuinfo, ext);
	if((unsigned)cpuinfo[0] >= level) {
#if FLAC__AVX_SUPPORTED
		__cpuidex(cpuinfo, ext, 0); /* for AVX2 detection */
#else
		__cpuid(cpuinfo, ext); /* some old compilers don't support __cpuidex */
#endif

		*eax = cpuinfo[0]; *ebx = cpuinfo[1]; *ecx = cpuinfo[2]; *edx = cpuinfo[3];

		return;
	}
#elif defined __GNUC__ && defined HAVE_CPUID_H
	FLAC__uint32 ext = level & 0x80000000;
	__cpuid(ext, *eax, *ebx, *ecx, *edx);
	if (*eax >= level) {
		__cpuid_count(level, 0, *eax, *ebx, *ecx, *edx);

		return;
	}
#endif
	*eax = *ebx = *ecx = *edx = 0;
}

#endif /* (FLAC__CPU_IA32 || FLAC__CPU_X86_64) && FLAC__HAS_X86INTRIN */
