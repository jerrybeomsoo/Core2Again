# Testing and what the results mean

Core2Again has two different things to prove: that its instruction emulators return correct results, and that Windows runs on every processor. No single local test proves both.

## Instruction results

Run the differential suite on a host CPU with real SSE4.2:

```bash
./tests/run.sh
```

It compiles the emulators as userspace code and compares results and flags with hardware instructions across roughly 41 million cases. Run it after changes to `Sse41.c`, `SseString.c`, or `SseDecoder.c`. The suite covers SSE4.1, SSE4.2 string comparisons, CRC32, and POPCNT. NaN inputs for `DPPS`/`DPPD` and `ROUND*` forms that take their rounding mode from MXCSR are excluded because those behaviors are not modeled by this test.

This suite does not test guest memory access or VMCS state. The Penryn test below covers the decode path on a CPU that really lacks the advertised instructions.

## VMX and boot under QEMU/KVM

```bash
./tests/ap/run.sh --kvm
./tests/hv/run.sh --kvm --smp 2
./tests/legacy/run.sh --image /path/to/preinstalled-windows.qcow2 \
  --smp 2 --seconds 300
```

`tests/ap` checks the AP transition from real mode to long mode. Use `--kvm`: TCG accepts stale real-mode segment state that physical hardware rejects. `tests/hv` checks the VMX lifecycle. `tests/legacy` starts the USB image with SeaBIOS and saves screenshots. It samples each vCPU's instruction pointer to check that every requested processor reached the guest kernel.

The Windows disk given to `tests/legacy` is never used as a writable base. The harness makes a throwaway qcow2 overlay and removes it at the end. Use an already installed Windows image. **Do not use the Windows installer ISO as the success test**: it boots without the SSE4.2 and POPCNT support this project provides.

On the development Xeon, KVM exposes real SSE4.2. Windows runs those instructions directly, so these QEMU tests do not trigger the `#UD` emulator. QEMU TCG has no VMX. A QEMU desktop shows that the boot chain and VMX plumbing work in that environment; it does not show that instruction emulation works during a Windows boot on Penryn.

## A real older CPU

`tests/penryn/run.sh` drives `SseTryEmulate` from real illegal-instruction signals on a CPU without SSE4.2. It requires SSH access to such a Linux host:

```bash
./tests/penryn/run.sh --host user@older-cpu --key /path/to/key \
  --threads 2 --rounds 1000000
```

The test reports how many real `#UD` faults it handled. A run on a modern CPU that executes SSE4.2 directly cannot establish the same thing. `tests/sse/run.sh` goes further by running the shipping driver inside a guest on a suitable nested-VMX older host; its DEBUG heartbeat row 1 must rise if the emulator is being used.

Finally, boot the release image on the intended physical machine and check both the Windows desktop and processor count. With a DEBUG build, [heartbeat row 2](diagnostics.md) must remain zero, while the AP status and non-BSP exit count show the second processor is active. Screen photographs are useful evidence because there is no serial diagnostic channel after `ExitBootServices`.

## Current evidence

An older Clover-based diagnostic build reached a Windows 11 desktop on the physical T9900 with both processors visible in Task Manager. The current RELEASE driver and OpenDuet image have reached a Windows desktop with two vCPUs under SeaBIOS/KVM. Their physical T9900 boot remains pending. The SSE4.1 path for older Conroe and Kentsfield processors also remains untested in a hardware Windows boot.

`tests/README.md` lists the rest of the harnesses and their purpose.
