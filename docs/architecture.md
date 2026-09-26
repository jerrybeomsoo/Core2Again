# How Core2Again works

The driver runs underneath Windows through Intel VMX. It presents a slightly newer instruction set through `CPUID` and emulates instructions that the physical CPU cannot execute. The design is aimed at Penryn-era hardware, especially a Core 2 Duo T9900 with SSE4.1 and VMX but without SSE4.2 or POPCNT.

## From UEFI to the guest

1. `Core2AgainLoader.efi` starts the driver. `Main.c` probes VMX, prepares shared state and per-processor regions, and hooks `ExitBootServices`.
2. The loader starts Windows Boot Manager. Firmware and the loader still run on the physical CPU. This lets a BIOS-hosted UEFI environment continue using real-mode BIOS thunks.
3. At Windows' successful `ExitBootServices` call, the boot processor enters VMX non-root operation. The driver calls the original firmware service, then starts the other processors.
4. Windows continues as the VMX guest. The exit handler manages `CPUID`, interrupts, control registers, MSRs, exceptions, and processor startup.

The ordering around `ExitBootServices` is deliberate. The boot processor must be under VMX before Windows starts. The other processors must wait until firmware has completed its shutdown; waking them earlier can strand firmware in its own MP teardown. A failed `ExitBootServices` call leaves the hook installed, so the loader's retry still passes through Core2Again.

## Why all processors matter

Windows asks each processor about its features. If only the boot processor reports SSE4.2 while an application processor reports the real, older feature set, Windows can reject the CPU configuration. Core2Again prepares a runtime stack and VMX state for each supported AP, sends its own INIT-SIPI sequence after boot services exit, and keeps the AP in a safe guest park loop until Windows starts it.

Windows then sends its own INIT and SIPI. VMX traps those events. `RealMode.c` interprets the beginning of Windows' AP trampoline until the far jump into protected mode; VMX takes over normal execution from there. This narrow interpreter exists because Penryn has no VMX unrestricted-guest feature and therefore cannot directly run a real-mode guest. The AP harness runs under KVM because QEMU TCG does not model the critical stale real-mode segment limit.

## When an unsupported instruction appears

`CPUID` leaf 1 reports the SSE4.2 and POPCNT bits Windows needs. On an older CPU, executing those instructions raises `#UD`. The VM-exit handler fetches the guest bytes, decodes prefixes and addressing, reads registers or guest memory, calls the scalar or SIMD emulator, writes results and flags, and advances guest RIP. SSE4.1 emulation is present for CPUs that also lack SSE4.1.

Guest memory access can fail. The handler must inject the appropriate guest exception rather than reading an unmapped host address. An instruction it cannot decode is passed on as a guest `#UD`; in the DEBUG build, heartbeat row 2 records that event. It should remain zero during a successful Windows boot.

The host code is built without SSE4.1, SSE4.2, or POPCNT instructions. A fault in VMX root operation would not pass through the guest emulator, so `scripts/verify.sh` checks the resulting EFI file's disassembly.

## Source map

| File | Job |
| --- | --- |
| `Main.c` | Driver entry and `ExitBootServices` hook |
| `VmxSetup.c`, `VmxAsm.S`/`.asm` | VMCS, host and guest state, VM entry and exit stubs |
| `VmxExitHandler.c` | VM-exit routing and guest exception handling |
| `Mp.c`, `ApStartup.c`, `RealMode.c` | Per-CPU setup and Windows AP bring-up |
| `GuestMem.c` | Guest address translation and safe operand access |
| `SseDecoder.c`, `SseString.c`, `Sse41.c` | Instruction decode and emulation |
| `Display.c`, `Diagnostics.c` | DEBUG-only framebuffer diagnostics |
| `Core2AgainLoader.c` | Driver start and Windows Boot Manager chainload |

The driver does not require EPT. On a CPU without it, the VMX setup uses the non-EPT memory path. It also does not claim to implement a general x86 machine emulator; it handles the boot and instruction paths needed by this project.
