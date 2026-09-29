# Test harnesses

Run these from the repository root. [The testing guide](../docs/testing.md) explains what each result can and cannot prove.

| Command | What it checks |
| --- | --- |
| `./tests/run.sh` | Roughly 41 million differential comparisons between the emulator and real SSE4.1, SSE4.2, CRC32, and POPCNT instructions |
| `./tests/ap/run.sh --kvm` | AP real-mode trampoline through long mode; KVM is required for the meaningful segment check |
| `./tests/hv/run.sh --kvm --smp 2` | Driver load, VMX lifecycle, and both vCPUs |
| `./tests/hv/run.sh --kvm --smp 2 --bsp-init` | A boot-processor INIT must reset the VM; the pre-fix driver stays frozen |
| `./tests/hv/run.sh --kvm --smp 2 --bsp-init --no-ept` | Same reset path without EPT, as on a Penryn processor |
| `./tests/bootgate/run.sh` | Windows EFI loader paths accepted; Linux, macOS, generic and malformed paths rejected |
| `./tests/hv/run.sh --kvm --smp 2 --boot-gate` | A Windows-like EFI loader enters VMX under KVM |
| `./tests/hv/run.sh --kvm --smp 2 --boot-gate --non-windows` | A generic EFI loader leaves boot services without entering VMX |
| `./tests/legacy/run.sh --image /path/to/windows.qcow2 --smp 2` | SeaBIOS, OpenDuet image, Windows boot, and per-vCPU progress |
| `./tests/win/run.sh --kvm --image /path/to/windows.qcow2` | UEFI/QEMU Windows boot path |
| `./tests/penryn/run.sh --host user@older-cpu --key /path/to/key` | Real `#UD` decode and emulation on an older CPU |
| `./tests/sse/run.sh --host user@older-cpu --key /path/to/key` | Driver exit path on a suitable older nested-VMX host |

The QEMU Windows harnesses use disposable qcow2 overlays. They do not write to the supplied base disk. Use a preinstalled Windows disk, not an installer ISO. On a modern KVM host with real SSE4.2, a successful QEMU boot does not exercise the instruction emulators.
