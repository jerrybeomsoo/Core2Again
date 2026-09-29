## @file
#  Core2Again.dsc - standalone build description for the Core2Again
#  package.  Target: X64, either tool family - the assembly ships as both MASM
#  (VmxAsm.asm, MSFT) and GNU as (VmxAsm.S, GCC); the .inf picks by family.
#
#  Build (Windows / Visual Studio):
#     build -a X64 -t VS2022 -p Core2AgainPkg/Core2Again.dsc
#  Build (Linux / GCC):
#     build -a X64 -t GCC   -p Core2AgainPkg/Core2Again.dsc
##

[Defines]
  PLATFORM_NAME                  = Core2AgainPkg
  PLATFORM_GUID                  = B2C3D4E5-6F70-4812-9A3B-4C5D6E7F8091
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/Core2AgainPkg
  SUPPORTED_ARCHITECTURES        = X64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT

  #
  # Set FALSE to build a single-core hypervisor: the APs are left alone and
  # Windows must be held to one processor (bcdedit /set numproc 1).  Useful as a
  # fallback if AP bring-up misbehaves on a particular board.
  #   build ... -D HV_AP_STARTUP=FALSE
  #
  DEFINE HV_AP_STARTUP           = TRUE

  #
  # Set TRUE for a diagnostic build.  It dumps the AP bring-up outcome inside
  # the ExitBootServices hook and holds it on screen for eight seconds - the
  # last moment the display is reliably ours - and then parks each AP at its
  # SIPI VM-exit instead of resuming it through the trampoline interpreter.
  #   build ... -D HV_AP_DEBUG=TRUE
  #
  DEFINE HV_AP_DEBUG             = FALSE

  #
  # Set TRUE to also build the test applications under tests/.  Off by default:
  # they are development tools, not part of the shipped hypervisor.
  #   build ... -D HV_BUILD_TESTS=TRUE
  #
  DEFINE HV_BUILD_TESTS          = FALSE

  # Send an INIT IPI to the BSP after ExitBootServices in the VMX harness.
  # This is a reboot-path regression test; never enable it in a boot image.
  DEFINE HV_TEST_BSP_INIT        = FALSE

  #
  # Set TRUE to build as if the processor had no EPT.  The target has none, so
  # the pass-through path is what it always runs - and a development host always
  # has EPT, so that path had no coverage anywhere.
  #   build ... -D HV_FORCE_NO_EPT=TRUE
  #
  DEFINE HV_FORCE_NO_EPT         = FALSE

[LibraryClasses]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  CpuLib|MdePkg/Library/BaseCpuLib/BaseCpuLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf

  # Null implementations keep the module self-contained.  Swap DebugLib for
  # BaseDebugLibSerialPort (and provide a SerialPortLib) to see [HV] traces.
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf

[Components]
  Core2AgainPkg/Core2Again.inf
  Core2AgainPkg/Core2AgainLoader.inf

!if $(HV_BUILD_TESTS) == TRUE
  tests/ap/ApTest.inf
  tests/hv/HvLifecycle.inf
  tests/sse/SseLive.inf
!endif

[BuildOptions]
  # No unexpected SSE codegen from the compiler itself: the hypervisor must not
  # rely on the very instructions it exists to emulate.  A compiler that
  # auto-vectorised host code to SSE4.1/4.2/POPCNT would make the exit handler
  # fault on those instructions in HOST context, where nothing can catch it.
  # Verify after any toolchain change with scripts/verify.sh.
  #
  # Accumulate each option into the compiler flags. An !if/!elseif chain would
  # silently discard the second of two requested options. Reassigning a DSC
  # macro to itself is not a fix: DSC macros expand lazily and only the last
  # assignment survives.
!if $(HV_AP_STARTUP) == FALSE
  DEFINE HV_D_AP     = -DHV_NO_AP_STARTUP
  DEFINE HV_M_AP     = /DHV_NO_AP_STARTUP
!else
  DEFINE HV_D_AP     = -DHV_AP_STARTUP_ENABLED
  DEFINE HV_M_AP     = /DHV_AP_STARTUP_ENABLED
!endif
!if $(HV_AP_DEBUG) == TRUE
  DEFINE HV_D_APDBG  = -DHV_AP_DEBUG
  DEFINE HV_M_APDBG  = /DHV_AP_DEBUG
!else
  DEFINE HV_D_APDBG  = -DHV_AP_DEBUG_OFF
  DEFINE HV_M_APDBG  = /DHV_AP_DEBUG_OFF
!endif
!if $(HV_FORCE_NO_EPT) == TRUE
  DEFINE HV_D_NOEPT  = -DHV_FORCE_NO_EPT
  DEFINE HV_M_NOEPT  = /DHV_FORCE_NO_EPT
!else
  DEFINE HV_D_NOEPT  = -DHV_EPT_AS_FOUND
  DEFINE HV_M_NOEPT  = /DHV_EPT_AS_FOUND
!endif
!if $(HV_TEST_BSP_INIT) == TRUE
  DEFINE HV_D_BSPINIT = -DHV_TEST_BSP_INIT
  DEFINE HV_M_BSPINIT = /DHV_TEST_BSP_INIT
!else
  DEFINE HV_D_BSPINIT = -DHV_NO_TEST_BSP_INIT
  DEFINE HV_M_BSPINIT = /DHV_NO_TEST_BSP_INIT
!endif

  MSFT:*_*_X64_CC_FLAGS  = /arch:SSE2 $(HV_M_AP) $(HV_M_APDBG) $(HV_M_NOEPT) $(HV_M_BSPINIT)
  GCC:*_*_X64_CC_FLAGS   = -march=x86-64 -mno-sse4 -mno-popcnt $(HV_D_AP) $(HV_D_APDBG) $(HV_D_NOEPT) $(HV_D_BSPINIT)

  MSFT:DEBUG_*_X64_CC_FLAGS   = /DHV_DIAG_ENABLED=1
  GCC:DEBUG_*_X64_CC_FLAGS    = -DHV_DIAG_ENABLED=1
  MSFT:RELEASE_*_X64_CC_FLAGS = /DHV_DIAG_ENABLED=0
  GCC:RELEASE_*_X64_CC_FLAGS  = -DHV_DIAG_ENABLED=0
