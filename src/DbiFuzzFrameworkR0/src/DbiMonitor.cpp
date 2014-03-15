/**
* @file CSysCall.cpp
* @author created by: Peter Hlavaty
*/

#include "drv_common.h"

#include "DbiMonitor.h"
#include "../../Common/Kernel/IRQL.hpp"
#include "../../Common/CPU/msr.h"
#include "../../Common/Kernel/MemoryMapping.h"

#include "../../Common/utils/Undoc.hpp"
#include "Common/Constants.h"
#include "../../Common/FastCall/FastCall.h"

#include "../../HyperVisor/src/VmmAutoExit.hpp"

EXTERN_C void sysenter();
EXTERN_C void rdmsr_hook();
EXTERN_C void pagafault_hook();
EXTERN_C void patchguard_hook();

typedef CAutoRef< CRefObjWorker<HANDLE, CProcess2Fuzz>, HANDLE, CProcess2Fuzz> CAutoProcWorkerRef;

CDbiMonitor CDbiMonitor::m_instance;

void* CDbiMonitor::m_pageFaultHandlerPtr[MAX_PROCID];

CStack< TRACE_INFO > CDbiMonitor::m_branchInfoStack;

CDbiMonitor::CDbiMonitor() :
	CSingleton(m_instance)
{
	RtlZeroMemory(m_syscalls, sizeof(m_syscalls));
	RtlZeroMemory(m_pageFaultHandlerPtr, sizeof(m_pageFaultHandlerPtr));
}

CDbiMonitor::~CDbiMonitor()
{
	BYTE core_id = 0;
	CProcessorWalker cpu_w;
	while (cpu_w.NextCore(&core_id, core_id))
	{
		KeSetSystemAffinityThread(PROCID(core_id));

		HookSyscallMSR(m_syscalls[core_id]);

		DbgPrint("Unhooked. procid [%x] <=> syscall addr [%p]\n", core_id, m_syscalls[core_id]);

		core_id++;//follow with next core
	}
}

//----------------------------------------------------------------
// ****************** INSTALLATION - HV + HOOKS ******************
//----------------------------------------------------------------

//************************************
// Method:    SetVirtualizationCallbacks
// FullName:  CDbiMonitor::SetVirtualizationCallbacks
// Access:    virtual protected 
// Returns:   bool 
// Reason:    Install VMM callbacks at VMM-EXIT
//************************************
__checkReturn 
bool CDbiMonitor::SetVirtualizationCallbacks()
{
	DbgPrint("CSysCall::SetVirtualizationCallbacks\n");

	if (!CCRonos::SetVirtualizationCallbacks())
		return false;

	m_traps[VMX_EXIT_RDMSR] = VMMRDMSR;
	m_traps[VMX_EXIT_WRMSR] = VMMWRMSR;
	m_traps[VMX_EXIT_EXCEPTION] = VMMEXCEPTION;

	//disable patchguard
	RegisterCallback(&m_callbacks, AntiPatchGuard);

	return RegisterCallback(&m_callbacks, VMMCPUID);
}

KEVENT w8event;
//************************************
// Method:    Install
// FullName:  CDbiMonitor::Install
// Access:    public 
// Returns:   void
// Reason:    Install VMM, and handling set-up hooks
//************************************
void CDbiMonitor::Install()
{
	ASSERT(CUndoc::IsInitialized());

#ifdef _DEBUG_MODE
	m_exceptionsMask = BTS(TRAP_debug);// | BTS(TRAP_page_fault);
#else
	m_exceptionsMask = BTS(TRAP_debug) | BTS(TRAP_int3);// | BTS(TRAP_page_fault);
#endif

	if (CCRonos::EnableVirtualization())
	{
		for (BYTE i = 0; i < m_vCpu.GetCount(); i++)
		{

#if HYPERVISOR

			if (m_vCpu[i].VirtualizationON())

#endif

			{
				int CPUInfo[4] = {0};
				int InfoType = 0;
				__cpuid(CPUInfo, InfoType);
				DbgPrint("\r\n~~~~~~~~~~~ !CPUID (%i) : %s ~~~~~~~~~~~\r\n", i, CPUInfo);
				KeBreak();
			}
		}

		KeBreak();
		//InstallPageFaultHooks();
		DbgPrint("\n\n******************************************\n***** DBI FuzzFramework, installed!\n******************************************\n");
	}
}

//************************************
// Method:    PerCoreAction
// FullName:  CDbiMonitor::PerCoreAction
// Access:    virtual protected 
// Returns:   void
// Reason:    Hook SYSCALL in MSR[IA64_SYSENTER_EIP]
// Parameter: __in BYTE coreId
//************************************
void CDbiMonitor::PerCoreAction( 
	__in BYTE coreId 
	)
{
	CCRonos::PerCoreAction(coreId);

	if (coreId < sizeof(m_syscalls))
	{
		KeSetSystemAffinityThread(PROCID(coreId));

		//set branching on basig blocks!!! + turn on last branch stack!
		wrmsr(IA32_DEBUGCTL, (rdmsr(IA32_DEBUGCTL) | BTF | LBR));
		//rdmsr / wrmsr dont affect guest MSR!!!!!!! -> vmwrite / read use instead

		m_syscalls[coreId] = reinterpret_cast<void*>(rdmsr(IA64_SYSENTER_EIP));
		HookSyscallMSR(sysenter);

		DbgPrint("Hooked. procid [%x] <=> syscall addr [%p]\n", coreId, m_syscalls[coreId]);
	}
}

//************************************
// Method:    InstallPageFaultHooks
// FullName:  CDbiMonitor::InstallPageFaultHooks
// Access:    public static 
// Returns:   void
// Reason:    Hook IDT[PageFault] per core!
//************************************
void CDbiMonitor::InstallPageFaultHooks()
{
	BYTE core_id = 0;
	CProcessorWalker cpu_w;
	while (cpu_w.NextCore(&core_id, core_id))
	{
		if (!GetPFHandler(core_id))
		{
			KeSetSystemAffinityThread(PROCID(core_id));

			GDT	idtr;
			sidt(&idtr);

			{
				CDispatchLvl irql;
				CMdl mdl(reinterpret_cast<void*>(idtr.base), IDT_SIZE);
				GATE_DESCRIPTOR* idt = static_cast<GATE_DESCRIPTOR*>(mdl.WritePtr());
				if (idt)
				{
					CDbiMonitor::GetInstance().SetPFHandler( core_id, reinterpret_cast<void*>(
						((static_cast<ULONG_PTR>(idt[TRAP_page_fault].ExtendedOffset) << 32) | 
						(static_cast<ULONG>(idt[TRAP_page_fault].Selector) << 16) | 
						idt[TRAP_page_fault].Offset)) );

					//hook ...
					{
						CDisableInterrupts cli_sti;
						idt[TRAP_page_fault].ExtendedOffset = ((reinterpret_cast<ULONG_PTR>(pagafault_hook)) >> 32);
						idt[TRAP_page_fault].Offset = reinterpret_cast<WORD>(pagafault_hook);
						idt[TRAP_page_fault].Selector = static_cast<WORD>(reinterpret_cast<ULONG>(pagafault_hook) >> 16);
					}

					DbgPrint("\r\nIDT HOOKED %x\r\n", core_id);
				}
			}
		}

		core_id++;//follow with next core
	}
}

//-----------------------------------------------------------
// ****************** MONITORING CALLBACKS ******************
//-----------------------------------------------------------

//handle virtual protection / allocation methods
//************************************
// Method:    SysCallCallback
// FullName:  SysCallCallback
// Access:    public 
// Returns:   EXTERN_C void*
// Reason:    Intercepting SYSCALL instruction [ MSR hook ]
// Parameter: register context, INOUT!
//************************************
EXTERN_C void* SysCallCallback( 
	__inout ULONG_PTR reg [REG_COUNT]
	)
{
	if (CIRQL::SufficienIrql(APC_LEVEL))
	{
		DbgPrint("\nIRQL OK1!! %x\n", KeGetCurrentIrql());
		//handle tracer fast-calls
		HANDLE proc_id = reinterpret_cast<HANDLE>(reg[DBI_FUZZAPP_PROC_ID]);
		if (FAST_CALL == reg[DBI_SYSCALL] && PsGetCurrentProcessId() != proc_id)
		{
			DbgPrint("\nIRQL OK2!!\n");
			return CDbiMonitor::GetInstance().GetSysCall(static_cast<BYTE>(KeGetCurrentProcessorNumber()));
			CAutoProcWorkerRef proc(CDbiMonitor::GetInstance().GetProcessWorker(), proc_id);
			if (proc.IsReferenced() && proc.GetObj())
			{
				if (proc.GetObj()->Syscall(reg))
					return NULL;
			}
		}
	}
	else
	{
		DbgPrint("\nINSUFFICIENT IRQL!!\n");
	}
	return CDbiMonitor::GetInstance().GetSysCall(static_cast<BYTE>(KeGetCurrentProcessorNumber()));
}

//************************************
// Method:    PageFault
// FullName:  PageFault
// Access:    public 
// Returns:   EXTERN_C void*
// Reason:    Intercepting PageFault event [ IDT hook ]
// Parameter: register context, INOUT!
//************************************
EXTERN_C void* PageFault(
	__inout ULONG_PTR reg[REG_COUNT]
	)
{
	if (UserMode == ExGetPreviousMode())
	{
		//previous mode == usermode ?
#define USER_MODE_CS 0x1
		if (PPAGE_FAULT_IRET(reg)->IRet.CodeSegment & USER_MODE_CS)//btf HV callback
		{
			CAutoProcWorkerRef proc(CDbiMonitor::GetInstance().GetProcessWorker(), PsGetCurrentProcessId());
			if (proc.IsReferenced() && proc.GetObj())
			{
				BYTE* fault_addr = reinterpret_cast<BYTE*>(readcr2());
				//DbgPrint("\nPageFault in monitored process %p %x\n", fault_addr, PsGetCurrentProcessId());
				if (proc.GetObj()->PageFault(fault_addr, reg))
					return NULL;
			}
		}
	}
	return CDbiMonitor::GetInstance().GetPFHandler(static_cast<BYTE>(KeGetCurrentProcessorNumber()));
}


//--------------------------------------------------------
// ****************** HYPERVISOR EVENTS ******************
//--------------------------------------------------------

//************************************
// Method:    VMMEXCEPTION
// FullName:  CDbiMonitor::VMMEXCEPTION
// Access:    protected static 
// Returns:   void
// Reason:    Handle Trap-Debug-Exception, load & store info to r0 container, redirect CF
// Parameter: register context, INOUT!
//************************************
void CDbiMonitor::VMMEXCEPTION( 
	__inout ULONG_PTR reg[REG_COUNT]
	)
{
	ULONG_PTR src = 0;
	CVMMAutoExit vmm_exit;

	//sucessfull readed ALL state info ? [ip, sp, flags, inslen, reason]
	if (vmm_exit.GetInsLen())
	{
		//is user mode ?
		if (IsUserModeAddress(vmm_exit.GetIp()))
		{
			vmm_exit.SetIp(reinterpret_cast<const BYTE*>(vmm_exit.GetIp()) - vmm_exit.GetInsLen());

			EVmErrors status;
			ULONG_PTR msr_btf_part = Instrinsics::VmRead(VMX_VMCS_GUEST_DEBUGCTL_FULL, &status);
			if (VM_OK(status))
			{
				if (msr_btf_part & BTF)
				{
					for (BYTE i = static_cast<BYTE>(rdmsr(MSR_LASTBRANCH_TOS)); i >= 0; i--)
					{
						if (reinterpret_cast<const void*>(rdmsr(MSR_LASTBRANCH_0_TO_IP + i)) == vmm_exit.GetIp())
						{
							src = rdmsr(MSR_LASTBRANCH_0_FROM_IP + i);
							if (!IsUserModeAddress(src))
							{
								//not user mode .. in kernel dbg condition will be oposite
								return;//skip handling
							}
							break;
						}
					}
				}
			}

			TRACE_INFO* trace_info_container = m_branchInfoStack.Pop();//interlocked NonPage queue
			if (trace_info_container)
			{
				TRACE_INFO* trace_info = trace_info_container;
				if (trace_info)
				{
					trace_info->StateInfo.IRet.StackPointer = vmm_exit.GetSp();
					trace_info->StateInfo.IRet.Return = const_cast<void*>(vmm_exit.GetIp());
					trace_info->PrevEip = reinterpret_cast<void*>(src);
					trace_info->StateInfo.IRet.Flags = vmm_exit.GetFlags();

					//set eip to non-exec mem for quick recognization by PageFault handler
					vmm_exit.SetSp(&trace_info->StateInfo.IRet.StackPointer[-(IRetCount + REG_COUNT + 1)]);
					vmm_exit.SetIp(trace_info_container);

					if (vmm_exit.IsTrapActive())
						vmm_exit.DisableTrap();	

					return;//is handled!
				}
			}
		}

		//post handling of kernel code
		switch(vmm_exit.GetInterruptionInfo())
		{
		case TRAP_debug:
			if (vmm_exit.IsTrapActive())
				vmm_exit.DisableTrap();	//turn off trap -> traps from kernel mode of windbg would not work as well ...
			break;
		case TRAP_int3:
			vmm_exit.SetIp(static_cast<const BYTE*>(vmm_exit.GetIp()) + vmm_exit.GetInsLen());//skip int3 if we are not able to handle it in following code ...
			break;
			//TODO
			//case TRAP_page_fault:
		}
	}
}

//************************************
// Method:    VMMWRMSR
// FullName:  CDbiMonitor::VMMWRMSR
// Access:    protected static 
// Returns:   void
// Reason:    handle enabling / disabling BTF + LBR in guest MSR (via vmread/write)
// Parameter: register context, INOUT!
//************************************
void CDbiMonitor::VMMWRMSR( 
	__inout ULONG_PTR reg[REG_COUNT] 
	)
{
	//special handling for our wrmsr - enable / disable BTF
	if (IA32_DEBUGCTL == reg[RCX])
	{
		//handle just dword-low
		EVmErrors status;
		ULONG_PTR msr_btf_part = Instrinsics::VmRead(VMX_VMCS_GUEST_DEBUGCTL_FULL, &status);
		if (VM_OK(status))
		{
			if (reg[RAX] & BTF)
				reg[RAX] |= msr_btf_part;//enable BTF + LBR
			else
				reg[RAX] &= msr_btf_part;//disable BTF + LBR

			if (VM_OK(Instrinsics::VmWrite(VMX_VMCS_GUEST_DEBUGCTL_FULL, reg[RAX])))
				reg[RDX] = Instrinsics::VmRead(VMX_VMCS_GUEST_DEBUGCTL_HIGH);
		}
	}

	wrmsr(static_cast<ULONG>(reg[RCX]), (reg[RDX] << 32) | static_cast<ULONG>(reg[RAX]));
}

//************************************
// Method:    VMMCPUID
// FullName:  CDbiMonitor::VMMCPUID
// Access:    protected static 
// Returns:   void
// Reason:    recognize special CPUID in codecoverme.exe, just for debug reasons ...
// Parameter: register context, INOUT!
//************************************
void CDbiMonitor::VMMCPUID( 
	__inout ULONG_PTR reg[REG_COUNT]
	)
{
	EVmErrors status;
	ULONG_PTR ExitReason = Instrinsics::VmRead(VMX_VMCS32_RO_EXIT_REASON, &status);
	if (VM_OK(status))
	{
		if (VMX_EXIT_CPUID == ExitReason)
		{
			if (0xBADF00D0 == (ULONG)reg[RAX])
			{
				reg[RBX] = 0xBADF00D0;

				ULONG_PTR rflags = Instrinsics::VmRead(VMX_VMCS_GUEST_RFLAGS, &status);
				if (VM_OK(status))
					NT_ASSERT(VM_OK(Instrinsics::VmWrite(VMX_VMCS_GUEST_RFLAGS, (rflags | TRAP))));
			}
		}
	}
}
	
//************************************
// Method:    VMMRDMSR
// FullName:  CDbiMonitor::VMMRDMSR
// Access:    protected static 
// Returns:   void
// Reason:    intercept rdmsr instructio at VMM level, and redirect CF when is readed SYSENTER_EIP -> hook for hidding
// Parameter: register context, INOUT!
//************************************
void CDbiMonitor::VMMRDMSR( 
	__inout ULONG_PTR reg[REG_COUNT] 
)
{
	ULONG_PTR syscall;
	if (IA64_SYSENTER_EIP == reg[RCX])
	{
		syscall = reinterpret_cast<ULONG_PTR>(CDbiMonitor::GetInstance().GetSysCall(CVirtualizedCpu::GetCoreId(reg)));
		
		EVmErrors status;
		ULONG_PTR ins_len = Instrinsics::VmRead(VMX_VMCS32_RO_EXIT_INSTR_LENGTH, &status);
		if (VM_OK(status))
		{
			reg[RCX] = Instrinsics::VmRead(VMX_VMCS64_GUEST_RIP, &status);
			if (VM_OK(status))
				NT_ASSERT(VM_OK(Instrinsics::VmWrite(VMX_VMCS64_GUEST_RIP, rdmsr_hook)));//.asm wrapper to RdmsrHook
		}
	}
	else
	{
		syscall = rdmsr((ULONG)reg[RCX]);
	}

	reg[RAX] = syscall;
	reg[RDX] = static_cast<ULONG>(syscall >> (sizeof(ULONG) << 3));
}

//----------------------------------------------------------------------
// ****************** HYPERVISOR PATCHGUARD DISABLING ******************
//----------------------------------------------------------------------

void CDbiMonitor::AntiPatchGuard( 
	__inout ULONG_PTR reg[REG_COUNT] 
	)
{
	CVMMAutoExit vmm_exit;
	switch (vmm_exit.GetReason())
	{
	case VMX_EXIT_RDTSC:
		if (CUndoc::IsPatchGuardContextOnRTDSC(reg))
		{
			reg[RSI] = reinterpret_cast<ULONG_PTR>(vmm_exit.GetIp());
			vmm_exit.SetIpFromCallback(patchguard_hook);//.asm wrapper to PatchGuardHook
		}
		break;
	default:
		return;
	}
}

//PatchGuard - alice in wonderland  [ virtualization-based hooks ]
//----------------------------------------------------------------------
// ****************** SUPERVISOR PATCHGUARD DISABLING ******************
//----------------------------------------------------------------------

//I. disable patchguard
EXTERN_C size_t PatchGuardHook( 
	__inout ULONG_PTR reg[REG_COUNT]
	)
{
	DbgPrint("\n >>>>>> PatchGuardHook %p [%p] %p --- %p\n\n", reg[RCX], reg, reg[RSI], KeBreak);
	KeBreak();
	return CUndoc::PatchGuardContextStackTopDelta();
}

//II. hide (even for PatchGuard) SYSENTER interception
EXTERN_C void* RdmsrHook( 
	__inout ULONG_PTR reg[REG_COUNT]
	)
{
	void* ret = reinterpret_cast<void*>(reg[RCX]);
	DbgPrint("\n\n---> RdmsrHook; PG active at : %p\n", ret);
	KeBreak();
	reg[RCX] = IA64_SYSENTER_EIP;
	return ret;
}

//----------------------------------------------
// ****************** GETTERS ******************
//----------------------------------------------

void* CDbiMonitor::GetSysCall( 
	__in BYTE coreId 
	)
{
	if (coreId > MAX_PROCID)
		return NULL;

	return CDbiMonitor::m_syscalls[coreId];
}

void CDbiMonitor::HookSyscallMSR(
	__in const void* hook
	)
{
	CDisableInterrupts idis;
	wrmsr(IA64_SYSENTER_EIP, reinterpret_cast<ULONG_PTR>(hook));
}


void* CDbiMonitor::GetPFHandler( 
	__in BYTE coreId 
	)
{
	if (coreId < MAX_PROCID)
		return CDbiMonitor::m_pageFaultHandlerPtr[coreId];
	return NULL;
}

void CDbiMonitor::SetPFHandler( 
	__in BYTE coreId,
	__in void* pfHndlr 
	)
{
	if (coreId < MAX_PROCID)
		CDbiMonitor::m_pageFaultHandlerPtr[coreId] = pfHndlr;
}

//getter
__checkReturn
CRefObjWorker<HANDLE, CProcess2Fuzz>* CDbiMonitor::GetProcessWorker()
{
	return m_procMonitor.GetProcessWorker();
}
