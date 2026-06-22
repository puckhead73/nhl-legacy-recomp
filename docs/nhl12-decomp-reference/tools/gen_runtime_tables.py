#!/usr/bin/env python3
"""Generate runtime/gen_import_tables.inc from docs/kernel_imports.csv.

Emits X-macro lists:
  IMPORT_FUNC(Name)            -- generic-stubbed function imports
  IMPORT_SLOT(slotVA, thunkVA, Name) -- slot fixup for function imports
  IMPORT_VAR(slotVA, Name)     -- data-variable imports (runtime allocates)

Imports listed in SPECIALS get IMPORT_SLOT entries but no IMPORT_FUNC stub â€”
the runtime implements them by hand.
"""

import csv

SPECIALS = {
    # main.cpp
    "NetDll_WSAStartup", "NetDll_WSACleanup", "NetDll_socket",
    "NetDll_closesocket", "NetDll_shutdown", "NetDll_ioctlsocket",
    "NetDll_setsockopt", "NetDll_getsockname", "NetDll_getpeername",
    "NetDll_bind", "NetDll_connect", "NetDll_accept", "NetDll_select",
    "NetDll_WSAGetOverlappedResult", "NetDll_recv", "NetDll_WSARecv",
    "NetDll_recvfrom", "NetDll_WSARecvFrom", "NetDll_send", "NetDll_sendto",
    "NetDll_inet_addr", "NetDll_WSAGetLastError", "NetDll_WSACreateEvent",
    "NetDll_WSACloseEvent", "NetDll_WSASetEvent", "NetDll_WSAResetEvent",
    "NetDll_WSAWaitForMultipleEvents", "NetDll_XNetStartup", "NetDll_XNetCleanup",
    "NetDll_XNetRandom", "NetDll_XNetCreateKey", "NetDll_XNetRegisterKey",
    "NetDll_XNetUnregisterKey", "NetDll_XNetXnAddrToInAddr",
    "NetDll_XNetServerToInAddr", "NetDll_XNetUnregisterInAddr",
    "NetDll_XNetConnect", "NetDll_XNetGetConnectStatus", "NetDll_XNetDnsLookup",
    "NetDll_XNetDnsRelease", "NetDll_XNetQosListen", "NetDll_XNetQosLookup",
    "NetDll_XNetQosServiceLookup", "NetDll_XNetQosRelease",
    "NetDll_XNetGetTitleXnAddr", "NetDll_XNetGetEthernetLinkStatus",
    "NetDll_XnpLogonGetStatus",
    "XNotifyGetNext",
    "XAudioRegisterRenderDriverClient", "XAudioUnregisterRenderDriverClient",
    "XAudioSubmitRenderDriverFrame", "XAudioGetVoiceCategoryVolume",
    "XAudioGetSpeakerConfig", "XAudioGetDuckerLevel",
    "XMACreateContext", "XMAInitializeContext", "XMAReleaseContext",
    "XMAEnableContext", "XMADisableContext",
    "XMAGetOutputBufferWriteOffset", "XMASetOutputBufferReadOffset",
    "XMAGetOutputBufferReadOffset", "XMASetOutputBufferValid",
    "XMAIsOutputBufferValid", "XMASetInputBuffer0Valid",
    "XMAIsInputBuffer0Valid", "XMASetInputBuffer1Valid",
    "XMAIsInputBuffer1Valid", "XMASetInputBuffer0", "XMASetInputBuffer1",
    "XMASetInputBufferReadOffset",
    "XUsbcamSetConfig", "XUsbcamGetState",
    "NtAllocateVirtualMemory", "NtFreeVirtualMemory",
    "KeGetCurrentProcessType", "KeQuerySystemTime",
    "KeBugCheck", "KeBugCheckEx",
    "RtlInitAnsiString", "RtlNtStatusToDosError", "RtlCompareStringN",
    "RtlRaiseException",
    "_snprintf", "sprintf",
    "ExGetXConfigSetting", "XexCheckExecutablePrivilege",
    "XGetAVPack", "XGetGameRegion", "XGetLanguage", "XGetVideoMode",
    "XamGetSystemVersion",
    "XamContentCreateEx", "XamContentClose", "XamContentCreateEnumerator",
    "XamContentGetDeviceData", "XamContentGetDeviceState",
    "XamContentGetLicenseMask",
    "XamShowSigninUI", "XamShowMessageBoxUI", "XamShowDeviceSelectorUI",
    "XamShowMessageBoxUIEx", "XamUserCreateAchievementEnumerator",
    "XamTaskSchedule", "XamTaskCloseHandle", "XamTaskShouldExit",
    "XamAlloc", "XamFree",
    "XMsgInProcessCall", "XMsgCompleteIORequest", "XMsgStartIORequest",
    "XMsgCancelIORequest", "XamGetOverlappedResult", "XMsgStartIORequestEx",
    "XamCreateEnumeratorHandle", "XamGetPrivateEnumStructureFromHandle", "XamEnumerate",
    "XamInputGetCapabilities", "XamInputGetState", "XamInputSetState",
    "XamInputGetKeystrokeEx",
    "XamUserGetDeviceContext", "XamUserGetXUID", "XamUserGetName",
    "XamUserGetSigninState", "XamUserCheckPrivilege", "XamUserAreUsersFriends",
    "XamUserGetMembershipTierFromXUID", "XamUserGetOnlineCountryFromXUID",
    "XamUserReadProfileSettings", "XamUserGetSigninInfo",
    "XamNotifyCreateListener",
    "RtlFillMemoryUlong", "RtlCompareMemoryUlong",
    "XeCryptSha", "XeCryptShaInit", "XeCryptShaUpdate", "XeCryptShaFinal",
    "XeCryptSha256Init", "XeCryptSha256Update", "XeCryptSha256Final",
    "XeCryptSha384Init", "XeCryptSha384Update", "XeCryptSha384Final",
    "XeCryptSha512Init", "XeCryptSha512Update", "XeCryptSha512Final",
    "XeKeysConsolePrivateKeySign", "XeKeysConsoleSignatureVerification",
    # kernel.cpp â€” threads/sync/critsec/TLS (Phase 3)
    "KeTlsAlloc", "KeTlsFree", "KeTlsGetValue", "KeTlsSetValue",
    "KeInitializeDpc", "KeInsertQueueDpc",
    "KeInitializeApc", "KeInsertQueueApc",
    "KeEnterCriticalRegion", "KeLeaveCriticalRegion",
    "KeEnableFpuExceptions", "KiApcNormalRoutineNop",
    "KeAcquireSpinLockAtRaisedIrql", "KeReleaseSpinLockFromRaisedIrql",
    "KfAcquireSpinLock", "KfReleaseSpinLock",
    "NtCreateEvent", "NtSetEvent", "NtClearEvent", "KeSetEvent", "KeResetEvent",
    "NtCreateSemaphore", "NtReleaseSemaphore", "NtCreateMutant", "NtReleaseMutant",
    "NtCreateTimer", "NtSetTimerEx", "NtCancelTimer",
    "NtWaitForSingleObjectEx", "KeWaitForSingleObject", "KeWaitForMultipleObjects",
    "NtYieldExecution", "KeDelayExecutionThread", "NtClose",
    "ObReferenceObjectByHandle", "ObDereferenceObject", "ObReferenceObject",
    "RtlInitializeCriticalSection", "RtlInitializeCriticalSectionAndSpinCount",
    "RtlEnterCriticalSection", "RtlLeaveCriticalSection", "RtlTryEnterCriticalSection",
    "ExCreateThread", "NtResumeThread", "NtSuspendThread", "ExTerminateThread",
    "KeSetAffinityThread", "KeSetBasePriorityThread", "KeQueryBasePriorityThread",
    "KeQueryPerformanceFrequency", "KeSetCurrentProcessType",
    # kernel.cpp â€” physical memory
    "MmAllocatePhysicalMemoryEx", "MmFreePhysicalMemory", "MmGetPhysicalAddress",
    "MmQueryAddressProtect", "MmQueryAllocationSize", "MmQueryStatistics",
    "MmSetAddressProtect", "MmMapIoSpace",
    # kernel.cpp - VFS
    "NtCreateFile", "NtOpenFile", "NtReadFile", "NtReadFileScatter",
    "NtWriteFile", "NtWriteFileGather", "NtQueryInformationFile",
    "NtQueryFullAttributesFile", "NtQueryVolumeInformationFile",
    "NtDeviceIoControlFile", "NtFlushBuffersFile", "NtSetInformationFile",
    "NtQueryDirectoryFile", "IoDismountVolumeByFileHandle",
    # kernel.cpp - video info
    "VdQueryVideoMode", "VdGetCurrentDisplayGamma",
    "VdGetCurrentDisplayInformation", "VdQueryVideoFlags",
    "VdInitializeEngines", "VdSetGraphicsInterruptCallback",
    "VdSetSystemCommandBufferGpuIdentifierAddress", "VdInitializeRingBuffer",
    "VdEnableRingBufferRPtrWriteBack", "VdCallGraphicsNotificationRoutines",
    "VdRetrainEDRAMWorker", "VdRetrainEDRAM", "VdIsHSIOTrainingSucceeded",
    "VdGetSystemCommandBuffer", "VdSwap", "VdSetDisplayMode",
    "VdSetDisplayModeOverride", "VdInitializeScalerCommandBuffer",
    "VdEnableDisableClockGating", "VdPersistDisplay", "VdShutdownEngines",
}

rows = list(csv.DictReader(open("docs/kernel_imports.csv")))
out = open("runtime/gen_import_tables.inc", "w", newline="\n")
out.write("// AUTO-GENERATED by tools/gen_runtime_tables.py â€” do not edit\n\n")

out.write("#ifdef IMPORT_FUNC\n")
n_stub = 0
for r in rows:
    if r["kind"] == "function" and r["name"] not in SPECIALS:
        out.write(f"IMPORT_FUNC({r['name']})\n")
        n_stub += 1
out.write("#endif\n\n")

out.write("#ifdef IMPORT_SLOT\n")
for r in rows:
    if r["kind"] == "function":
        out.write(f"IMPORT_SLOT({r['slot_va']}, {r['thunk_va']}, {r['name']})\n")
out.write("#endif\n\n")

out.write("#ifdef IMPORT_VAR\n")
n_var = 0
for r in rows:
    if r["kind"] == "variable":
        out.write(f"IMPORT_VAR({r['slot_va']}, {r['name']})\n")
        n_var += 1
out.write("#endif\n")
out.close()
print(f"stubs: {n_stub}, specials: {len(SPECIALS)}, variables: {n_var}")

