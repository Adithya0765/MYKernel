# Plan: Roadmap to Mature Alteo OS

Your kernel has an impressive desktop environment, networking stack, and VFS — but the foundational OS layers (virtual memory, user mode, real multitasking) are not yet in place. **Without these foundations, none of the ambitious goals (.exe support, NVIDIA drivers, browser) are possible.** Below is an honest, phased roadmap prioritized by dependency order. Each phase unlocks the next.

---

## Phase 1 — Core Kernel Foundations (Must Do First)

1. **Implement a real Virtual Memory Manager** — Replace the identity-mapped 1 GB huge pages in `boot.asm` with a proper 4-level page table manager (`PML4 → PDP → PD → PT`). Add a page fault handler in `idt.c`. This enables per-process address spaces, memory protection, and `mmap`.

2. **Implement User Mode (Ring 3)** — Set up a TSS, user-mode code/data segments (`cs=0x23`, `ds=0x2B`), and `IRETQ`-based transition to Ring 3. Update `process.c` so processes run in user space. Wire syscalls via `SYSCALL/SYSRET` MSRs instead of the current direct calls in `syscall.c`.

3. **Implement Real Context Switching** — Write an assembly `switch_to()` stub that saves/restores `rsp`, `rip`, and all callee-saved registers. Hook it into the timer IRQ so `scheduler.c` actually preempts processes instead of the current `while(1)` polling loop in `kernel.c`.

4. **Build an ELF Loader** — Create `elf.c` to parse ELF64 binaries from the VFS, map `.text`/`.data`/`.bss` segments into a process's address space, and jump to the entry point. This replaces the current null-function-pointer process creation in `process.c`.

5. **Build a PE Loader** — Create `pe.c` alongside the ELF loader to parse PE (Portable Executable) headers, map `.text`/`.data`/`.bss`/`.rdata` sections, handle relocations, and resolve imports against the Win32 compatibility layer (Phase 5). This enables loading `.exe` files directly.

6. **Mount FAT32 into VFS** — Add a mount mechanism to `vfs.c` so `fat32.c` can serve as a real on-disk backend, not just sit unused alongside the in-memory FS.

---

## Phase 2 — Hardware & Driver Infrastructure

7. **Implement full PCI bus enumeration** — Currently `e1000.c` and `ac97.c` do ad-hoc PCI scans. Build a proper `pci.c` that enumerates all devices/functions, stores a device tree, and supports BAR mapping. Every driver below depends on this.

8. **Implement ACPI** — Parse RSDP → RSDT/XSDT → MADT (for SMP/APIC), FADT (for power management/shutdown). Without ACPI, you can't discover interrupt routing, CPU cores, or perform clean shutdown.

9. **Implement APIC + SMP** — Replace the legacy 8259 PIC in `irq.c` with Local APIC + I/O APIC. Then bring up Application Processors (multi-core) via SIPI. Add spinlocks/mutexes to all shared data structures.

10. **Implement USB (xHCI)** — This is essential for any real hardware. Build `xhci.c` → `usb_hub.c` → `usb_hid.c` (keyboard/mouse) → `usb_storage.c` (mass storage). The current PS/2 drivers in `keyboard.c` and `mouse.c` only work in emulators.

11. **Implement a block device layer + disk cache** — Abstract ATA/AHCI/NVMe behind a common block interface. Add a page-cache for disk reads. This sits between `fat32.c` and the raw ATA driver.

---

## Phase 3 — Userspace & Application Support

12. **Port a C library (e.g., `mlibc` or `newlib`)** — No userspace application can run without `libc`. This requires your syscall interface, ELF loader, and `mmap` to all be working.

13. **Implement pipes, signals, and IPC** — Add `pipe()`, `dup2()`, POSIX signals (`SIGTERM`, `SIGKILL`, `SIGSEGV`), and shared memory. Required for any multi-process application.

14. **Implement a POSIX-compatible VFS layer** — Extend `vfs.h` with `ioctl`, `fcntl`, `poll`/`select`, `/dev` and `/proc` pseudo-filesystems. Applications expect this interface.

15. **Add ext2/ext4 filesystem support** — FAT32 lacks permissions, symlinks, and large file support. Enterprise OSes need a journaling filesystem.

---

## Phase 4 — Graphics & NVIDIA GPU Driver (Custom Built)

Build your own NVIDIA driver from scratch using reverse-engineered documentation from [envytools](https://envytools.readthedocs.io/) and NVIDIA's open-source kernel module source (for reference). Target **Tesla-era (GT 200 series)** first — best documented, cheapest to acquire.

16. **Stage 1: PCI Discovery + Framebuffer** — Detect NVIDIA GPU via PCI vendor ID `0x10DE`, map MMIO BARs. Use the existing VBE/GOP framebuffer for display output. No GPU programming yet — just confirming the GPU is recognized and addressable.

17. **Stage 2: Display Engine / Mode Setting** — Program the GPU's CRTC (display timing), encoder (HDMI/DP output), and connector registers using envytools docs. Support runtime resolution switching, multi-monitor detection, and hardware cursor. This replaces the fixed 1024×768 VBE mode from GRUB.

18. **Stage 3: 2D Acceleration** — Program the GPU's 2D engine for hardware-accelerated rectangle fills, blits, and screen-to-screen copies via DMA. Replace the current software `flip_buffer()` pixel loop in `graphics.c` with GPU-accelerated transfers. Result: near-instant screen updates and smooth window compositing.

19. **Stage 4: Command Submission & 3D Engine** — Initialize the GPU's FIFO/channel system for command buffer submission. Program the graphics pipeline: vertex processing, rasterization, fragment shading. Implement a fixed-function 3D pipeline first, then basic programmable shaders.

20. **Stage 5: Graphics API (OpenGL subset)** — Expose 3D acceleration to userspace through your own graphics API or a subset of OpenGL (e.g., OpenGL 1.x fixed-function, then 2.x with shaders). Build a display server / compositor that uses GPU acceleration for window management.

21. **Stage 6: GPU Memory Management & Multi-Process** — Implement GPU virtual memory (the GPU has its own page tables), GPU context switching so multiple apps can share the GPU, power management (clock gating, P-states), and optionally video decode/encode (NVDEC/NVENC).

### NVIDIA GPU Generation Targets (in order)

| Priority | Generation | Codename | Why |
|----------|-----------|----------|-----|
| 1st | NV50 | Tesla (2006–2010) | Best envytools docs, simplest architecture |
| 2nd | GF100 | Fermi (2010–2012) | Good docs, still widely available |
| 3rd | GK/GM/GP | Kepler → Pascal (2012–2018) | Partial docs, mainstream performance |
| 4th | TU/GA/AD | Turing → Ada (2018+) | Open-source kernel module source available for reference |

---

## Phase 5 — Windows .exe Compatibility Layer

Build an incremental Win32 API compatibility layer so `.exe` files loaded by the PE loader (Phase 1, step 5) can actually run. Each tier unlocks a new class of applications.

22. **Tier 1: Core NT Stubs** — Implement the bare minimum `ntdll.dll` equivalents: `NtCreateFile`, `NtReadFile`, `NtWriteFile`, `NtAllocateVirtualMemory`, `NtClose`. Map these to your VFS and VMM syscalls. **Result**: simple console `.exe` files that do file I/O can run.

23. **Tier 2: kernel32 + msvcrt** — Implement `kernel32.dll` functions (`CreateFileA/W`, `ReadFile`, `WriteFile`, `VirtualAlloc`, `GetStdHandle`, `GetCommandLineA`, `ExitProcess`, `GetLastError`) and `msvcrt.dll` C runtime (`printf`, `malloc`, `fopen`, `exit`). **Result**: basic C/C++ console applications run unmodified.

24. **Tier 3: user32 + gdi32 (GUI support)** — Implement `CreateWindowExA/W`, `DefWindowProc`, `GetMessage`/`DispatchMessage`, `ShowWindow`, `BeginPaint`/`EndPaint`, `BitBlt`, `TextOut`. Bridge these to your desktop environment's window manager. **Result**: simple Win32 GUI apps (Notepad-style) display and interact correctly.

25. **Tier 4: Advanced APIs** — As needed, implement subsets of `ws2_32.dll` (Winsock → your TCP/IP stack), `advapi32.dll` (registry → your config system), `comctl32.dll` (common controls), and `shell32.dll`. Each API is added on-demand as you target specific `.exe` applications.

26. **Tier 5: DirectX Translation** — Implement a minimal `d3d9.dll` or `d3d11.dll` shim that translates DirectX draw calls into your GPU driver's graphics API (Phase 4, step 20). This is the final step to running DirectX-based Windows games. Alternatively, implement a Vulkan subset and use DXVK-style translation.

---

## Phase 6 — Browser & Networking

27. **Implement a DNS resolver** — Your TCP/IP stack in `tcp.c`/`ip.c` works but has no DNS. Build a UDP-based DNS client to resolve hostnames, using the network config's DNS server field.

28. **Implement a DHCP client** — Replace the hardcoded `10.0.2.15` QEMU IP with dynamic address acquisition. Send DHCP Discover/Request/Ack via UDP broadcast on the E1000 interface.

29. **Port NetSurf browser** — NetSurf is a lightweight C browser that has been ported to multiple hobby OSes (ToaruOS, RISC OS, Haiku). It requires: `libc`, `libcurl` (or your own HTTP client), `libpng`, `libjpeg`, `libfreetype`, `libcss`, and a framebuffer backend. **This is the realistic path to a real browser** — far more achievable than Firefox.

30. **Enhance the built-in "Surf" browser** — In parallel, continue improving your existing Surf app in `kernel.c` to support HTTP/1.1, chunked transfer encoding, basic HTML parsing, CSS styling, and image rendering. This serves as a lightweight fallback browser native to Alteo OS.

---

## Phase 7 — Security (Runs Parallel to Everything)

31. **Enforce Ring 0 / Ring 3 separation** — This is the #1 security prerequisite. Currently everything runs in Ring 0, meaning any code can overwrite the kernel. Phase 1 step 2 fixes this.

32. **Add ASLR (Address Space Layout Randomization)** — Randomize stack, heap, and library base addresses per process. Requires a working VMM (Phase 1).

33. **Add stack canaries and guard pages** — Compile userspace with `-fstack-protector`; place unmapped guard pages around kernel/user stacks to catch overflows.

34. **Implement capability-based permissions / ACLs** — Extend the `permissions` field in `vfs.h` to enforce real user/group access control. Add a concept of users/groups and authentication.

35. **Implement SMEP/SMAP** — Enable Supervisor Mode Execution/Access Prevention (CPU features) so the kernel can't accidentally execute or read user memory.

36. **Implement seccomp-style syscall filtering** — Restrict which syscalls each process can make, to limit blast radius of exploits.

37. **Implement secure boot chain** — Verify kernel and driver signatures before loading. Protect against bootkits and rootkits.

38. **Implement encrypted filesystem support** — Add AES-XTS encryption at the block layer so user data is encrypted at rest. Essential for enterprise use.

---

## Estimated Timeline (Solo Developer)

| Phase | Milestone | Estimated Time |
|-------|-----------|----------------|
| **Phase 1** | Core foundations (VMM, Ring 3, context switch, ELF/PE loader, VFS mount) | 4–6 months |
| **Phase 2** | Hardware drivers (PCI, ACPI, SMP, USB, block layer) | 4–8 months |
| **Phase 3** | Userspace (libc, signals, POSIX VFS, ext2) | 3–6 months |
| **Phase 4** | NVIDIA Stage 1–2 (detect + mode setting) | 2–4 months |
| **Phase 4** | NVIDIA Stage 3 (2D acceleration) | 3–6 months |
| **Phase 4** | NVIDIA Stage 4–5 (3D + OpenGL subset) | 1–3 years |
| **Phase 5** | Win32 Tier 1–2 (console .exe apps run) | 2–4 months |
| **Phase 5** | Win32 Tier 3 (GUI .exe apps run) | 4–8 months |
| **Phase 5** | Win32 Tier 4–5 (advanced APIs + DirectX) | 1–3 years |
| **Phase 6** | DNS/DHCP + NetSurf port | 2–4 months |
| **Phase 7** | Security hardening (ongoing) | Continuous |
| | **Total to "enterprise-capable" OS** | **~3–5 years** |

---

## Further Considerations

1. **Prioritize ruthlessly** — Phase 1 is the absolute prerequisite. Every other phase depends on having a working VMM, user mode, and context switching. Don't skip ahead or nothing will work.

2. **Study existing hobby OSes** — Projects like [SerenityOS](https://github.com/SerenityOS/serenity), [ToaruOS](https://github.com/klange/toaruos), and [Managarm](https://github.com/managarm/managarm) have solved many of these problems with readable code. ToaruOS has ported NetSurf. SerenityOS built its own browser engine. Managarm has real GPU drivers.

3. **Consider a cross-compiler toolchain** — Your `Makefile` uses host `gcc`, which works but can cause subtle issues. Building an `x86_64-elf` cross-compiler ensures clean freestanding compilation.

4. **Buy a Tesla-era NVIDIA card early** — A GT 218 / GT 200 series card costs ~$5 on eBay. Having real hardware to test against from day one makes GPU driver development dramatically easier than guessing in an emulator.

5. **Build incrementally and test constantly** — For the Win32 layer, pick one target `.exe` at a time (e.g., a "Hello World" console app, then Notepad, then a simple game) and implement exactly the APIs it needs. Don't try to implement all of Win32 upfront.

6. **Design your own native app format (AEF)** — While `.exe` compatibility is a powerful feature, encourage developers to build native Alteo OS apps. Define an "Alteo Executable Format" with your own headers, your own SDK, and your own UI toolkit. This is what makes your OS *yours*, not a Windows clone.