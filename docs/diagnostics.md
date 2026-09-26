# Reading the debug display

The `Core2Again-DEBUG.efi` driver writes directly to the framebuffer. It can still show a result after UEFI boot services are gone. The normal `Core2Again-RELEASE.efi` contains no display diagnostics. Put the DEBUG file at the USB volume root as `Core2Again.efi` to use this guide.

White numbers are checkpoints. Cyan numbers are rows of data, counted from the top starting at row 0. Rows are overwritten but not cleared, so a row that is not defined for the current checkpoint may be stale.

## Checkpoints

| White number | Meaning |
| --- | --- |
| 0 to 4 | Driver entry, VMX probe, shared setup, and feature detection |
| 5 to 9 | `ExitBootServices` hook, boot processor VMX entry, and AP startup |
| 10 to 17 | VMX structures, host tables, VMCS programming, and VM entry |
| 18 | VMXON, VMCS setup, or VMLAUNCH failed; the failed bring-up returns. A VMLAUNCH failure paints its VM-instruction error on row 3. |
| 19 | Unexpected VM exit; rows 0 to 4 give state and exit reason |
| 20 | Double fault; rows record recent guest faults and the last injected exception |
| 22 | VMCS guest state was rejected; rows 0 to 8 show the exit reason and selected guest state |
| 23 | AP real-mode trampoline used an instruction the interpreter cannot handle |
| 24 | VMRESUME failed; rows include the VM-instruction error and guest RIP |
| 25 | The special `apdebug` build parks the AP when Windows sends a SIPI |
| 26 to 29 | `apdebug` AP startup report and timed stages before the live heartbeat |
| 30 | Live heartbeat; use the row table below |

Numbers 0 to 17 mark progress and continue. Checkpoint 18 records a failed VMX bring-up and returns to its caller. The failure paths at 19, 20, 22, 23, and 24 halt the affected CPU; 25 deliberately parks the AP. In the `apdebug` build, 26 is held for eight seconds and 27 to 29 for two seconds each before continuing. Record the white number and all visible cyan rows before changing anything.

## The heartbeat at checkpoint 30

The boot processor repaints these rows periodically. Compare two photographs a few seconds apart to see which counters move.

| Row | Meaning |
| --- | --- |
| 0 | Total VM exits; should keep rising |
| 1 | Instructions emulated after `#UD`; a nonzero value on Penryn proves the path is in use |
| 2 | Unhandled `#UD`, packed as reason times 1000 plus count; **must stay 0** |
| 3 | With row 2 at zero, AP fate and last non-BSP exit reason; otherwise low 32 bits of the faulting RIP |
| 4 | With a healthy AP, non-BSP VM exits; otherwise first four faulting instruction bytes or an AP interpreter address |
| 5 | Guest page faults injected for missing operands; may also show faulting bytes or AP interpreter bytes |
| 6 | Guest general-protection faults reinjected; after an AP double fault, the last exception injected by Core2Again |
| 7 | Last VM-exit reason; after an AP double fault, vector and error code |
| 8 | AP status, packed as `F-L-AA-II-SS` |

Row 2 is the key correctness signal. A nonzero value means Core2Again told the guest an instruction existed but failed to emulate it, so Windows may receive `STATUS_ILLEGAL_INSTRUCTION`. Reasons are `1` unreadable instruction, `2` unsupported 32-bit address size, `3` unknown opcode, `5` read past available instruction bytes, and `6` an internal invariant failure. Rows 3 to 5 then help identify the instruction.

Row 8 groups five decimal fields. `F` is the AP startup failure code (`0` means none); `L` is the number of virtualized CPUs; `AA` is the number of APs that reached C; `II` and `SS` are the INIT and SIPI VM-exit counts. For example, `2010101` means two virtualized CPUs, one AP arrived, and Windows sent one INIT and one SIPI. A value of `1000000` means only the boot processor is virtualized. Failure code `7` means startup IPIs were sent but no AP answered.

The row layout changes after certain fatal AP faults, so preserve a photograph of the whole display rather than transcribing only one value. The `apdebug` variant holds checkpoint 26 for eight seconds and reports the AP trampoline stage and setup addresses before Windows takes the display.
