# Lokale Patches für das xboxrecomp-Submodul

Das Submodul (`xboxrecomp/`, v0.14.0) wird nicht committet verändert. Eigene Änderungen liegen hier als Patch und
werden nach einem frischen Checkout / Submodul-Update angewendet:

```bash
git -C xboxrecomp apply ../patches/xboxrecomp/0001-linux-x86_64-mmio-traps-casefold-mingw-oldnames.patch
```

Alle Kandidaten für einen Upstream-PR an sp00nznet/xboxrecomp.

## 0001 – Linux x86-64: Trapped MMIO, Groß/Klein-Schreibung, MinGW-Old-Names
- `src/platform/mmio_decode.h`: x86-64-Decoder nicht mehr auf `_WIN32` beschränkt. Kontext abstrahiert
  (`mmio_ctx_t`, `MMIO_CTX_PC`, `MMIO_CTX_EFLAGS`, `mmio_ctx_reg`): Win32 `CONTEXT` bzw. Linux `ucontext_t.gregs[REG_*]`.
  Decoder-Logik unverändert.
- `src/apu/apu_mmio_hook.c`: APU-MMIO-Hook auch auf Linux x86-64, neuer Einstieg `apu_hook_handle_mmio_posix`.
- `src/kernel/xbox_memory_layout.c`: `xbox_PosixMmioFault` und AC'97-Bus-Master-Schreibtrap auf Linux x86-64
  (bisher nur arm64) – ohne das stürzt XDK-DirectSound mit `RECOMP_AC97_READY=1` beim ersten APU-Register ab.
- `src/usb/ohci.c`: OHCI-Trap auf Linux x86-64 (`OHCI_TRAP_POSIX_X64`).
- `src/kernel/kernel_path.c`: POSIX-`xbox_translate_path` löst Pfadkomponenten groß/klein-unabhängig auf, wenn der
  exakte Pfad fehlt (FATX/XDVDFS und Win32 sind case-insensitive; SSX 3 öffnet `intro_dj.mad`, auf der Disc heißt sie
  `Intro_DJ.mad`).
- `tools/recomp/lifter.py`: MSVC-„Old Names“ aus MinGW-`math.h` (`chgsign`, `fpclass`, `finite`, `j0/j1/jn/y0/y1/yn`)
  in `_FUNC_RESERVED_IDENT` – sonst bricht ein MinGW-Build an Ghidra-Namen wie `fpclass` ab.

- `src/kernel/nv2a_pb_exec.c`: **deaktivierte Vertex-Attribute** liefern als Vertex-Programm-Eingabe ihren zuletzt per
  `SET_VERTEX_DATA*` gesetzten Wert (NV2A-/xemu-Verhalten) statt (0,0,0,1). SSX 3 übergibt die Matrix seines
  2D/FMV-Programms in v8..v15 per `SET_VERTEX_DATA4F_M` – ohne das wurde jedes Vertex auf denselben Punkt transformiert
  und das Intro-Video als degenerierte Dreiecke verworfen. Dazu `RECOMP_VSH_TRACE_EVERY=<n>` (Stride des VSH-Trace,
  Default 20000 wie bisher).

- `src/kernel/nv2a_pb_exec.c`: **Rasterizer-Thread-Pool auch auf POSIX** (pthreads; bisher nur `_WIN32`, Linux renderte
  einthreadig) – SSX-3-Titelbild 4,4 → 15,4 fps; Pool-Größe wie unter Windows (`RECOMP_RASTER_THREADS`, Default CPUs−4).
  Dazu die fps-Anzeige des Executors auf Wanduhr (`GetTickCount64`) statt `clock()` (= Prozess-CPU-Zeit unter POSIX,
  zeigte bei mehreren beschäftigten Threads ein Vielfaches zu wenig fps).

Geprüft: Linux-Build + Windows-Cross-Build (MinGW) bauen; eigener Decoder-Test (8 Fälle, ucontext) grün;
`tools.recomp.test_reserved_idents` grün; SSX 3 erreicht unter Linux denselben Stand wie unter Proton
(APU/AC'97/DSP-Doorbell/APU-Interrupts, Intro-Videos werden geöffnet).
