# BSD44-ELF-x86 — 4.4BSD-Lite2 i386 ELF 可启动内核与 El Torito ISO

用 **MSYS2 mingw32 的 i386 GCC**（`i686-w64-mingw32-gcc 16.2.0`）编译真实的
**4.4BSD-Lite2 `sys/i386`** 机器相关代码，生成 **UNIX System V ELF32 (Intel 80386)**
内核，再装入一张 **El Torito 无仿真引导** 的 ISO。ISO 里的内核是 ELF，不是 PE/EXE。

```
out/BSD44-ELF-x86.iso     4.4BSD-Lite2 i386 ELF 内核 + El Torito 引导
out/BSD44.ELF             同一内核（ELF32 EXEC，Intel 80386）
```

## 1. 结果

QEMU / VMware 从光盘启动后，串口与 VGA 同时输出：

```
4.4BSD-Lite2 (i386) ELF kernel
Kernel: ELF32 i386, KERNBASE fe000000, first free page 50000
Memory: maxmem 4000000, physmem 3fa0000, avail 100000-3ffe000
pmap: kernel_pmap 0xfe047010 pdir 0xfe049000 IdlePTD 49000
VM: virtual_avail fe0bd000 virtual_end ff7ff000

BSD/386 monitor - type 'help' for commands
bsd44> mem
Maxmem    16384 pages  (67108864K)
physmem   16288 pages  (66715648K)
avail     100000 - 3ffe000
virt      fe0bd000 - ff7ff000
kernel    text fe004228 data fe047000 end fe049000
bsd44> map
mapped page at 0xfe0bd000
pte new page: 100063
readback 4f534442 444e4521
ok
```

`map` 命令调用的是 4.4BSD 真正的 `pmap_bootstrap_alloc()`
→ `pmap_map()` → `pmap_enter()` 路径，把物理页映射到内核虚拟地址后再写回验证。

`readelf -h build/bsd44.elf`：

```
Class:      ELF32
Data:       2's complement, little endian
Type:       EXEC (Executable file)
Machine:    Intel 80386
Entry:      0xfe001000
```

`objdump -d` 中没有任何 `xmm/ymm/mm0-7` 指令（`-march=i386 -mno-sse -mno-mmx`）。

## 2. 目录

| 路径 | 说明 |
|---|---|
| `build_kernel.ps1` | 头文件准备、编译、两遍链接、COFF→ELF |
| `build_iso.ps1` | 编译 stage1/stage2、生成 El Torito 引导镜像、mkisofs、附加内核 |
| `build_all.ps1` | 一键构建内核 + ISO |
| `verify.ps1` | 用 QEMU 启动 ISO、发送 `mem` 命令、检查串口输出 |
| `boot/stage1.s` | 16 位实模式引导：INT 13h EDD 读 ELF 到 0x10000，进保护模式 |
| `boot/stage2.s` | 32 位 ELF 加载器：解析 PT_LOAD，按 `p_vaddr-KERNBASE` 装载，跳 `e_entry` |
| `kernel/glue.c` | `proc0`/`curproc`、kprintf、`vm_set_page_size`、`trap`、`main` |
| `kernel/pccon.c` | VGA 文本 + COM1 双路控制台，接 4.4BSD `cons.c` 的 `consdev` |
| `kernel/mon.c` | 板载 monitor：`help/mem/pte/map/banner/reboot/halt` |
| `kernel/stubs.c` | 极小内核服务：`malloc/free`、`pg`、`isa_strayintr`、`__main` 等 |
| `kernel/strings.c` | `strncmp/memcpy/memset/memmove` |
| `src/4.4BSD-Lite2` | 解包后的源码树 |
| `build/inc` | 编译用头文件视图（`sys/ vm/ machine/ i386/ libkern/`） |
| `build/gen` | 代替 `config(8)` 生成的头：`isa.h NISA 0`、`com.h NCOM 0` |
| `out/` | 最终 ISO 与 ELF |

## 3. 工具链（全部在本机 C:\msys64，无网络）

```
C:\msys64\mingw32\bin\gcc.exe      i686-w64-mingw32-gcc 16.2.0
C:\msys64\mingw32\bin\ld.exe       GNU ld 2.47（仅 i386pe）
C:\msys64\mingw32\bin\objcopy.exe  支持 -O elf32-i386
C:\Program Files (x86)\VMware\VMware Workstation\mkisofs.exe
C:\Program Files\qemu\qemu-system-i386.exe
```

关键点：mingw 的 `gcc -c` 输出 COFF，`ld` 只有 `i386pe` 仿真，不能直接
`--oformat elf32-i386`。做法是：

1. `gcc -m32 -c ...` 生成 COFF；
2. `ld -m i386pe --image-base 0xFE000000 -Ttext 0xFE001000` 在 PE 里完成重定位；
3. `objcopy -O elf32-i386` 把链接结果转成 **ET_EXEC** 的 ELF32；
4. 用 `--defsym _end/_edata/_etext=` 把 4.4BSD `locore.s` 需要的边界符号精确补上。

`gcc -B C:/msys64/mingw32/bin/` 是必须的：否则驱动找不到 `as.exe`。

## 4. 引导链

BIOS → El Torito（no-emulation）把 `boot.img` 装到 `0x7C00`。

**stage1（512 B，实模式）**
- 从镜像内 `BDMF` manifest 取内核 LBA（2048）和字节数；
- INT 13h AH=42h 扩展读，按 **CD 的 2048 字节扇区** 每次 16 扇区读入 `0x10000`；
- 把 stage2 复制到 `0x90000`，开 A20，装 GDT，进保护模式。

**stage2（32 位，链接在 0x90000）**
- 把 ELF 头快照到 `0x8F000`（后面会被段数据覆盖）；
- 逐条 `PT_LOAD`：`dst = p_vaddr - 0xFE000000`，`rep movsb`，再清 `p_memsz-p_filesz`；
- 压入 `howto/bootdev/unit/cyloffset`，`jmp e_entry - 0xFE000000`（即 `start`）。

**内核入口**
`locore.s:start` 自行建立 GDT/页表，把物理 0..kernel_end 映射到
`0xFE000000+`，再 `call init386` → GDT/IDT/LDT/TSS、RTC 测内存、
`pmap_bootstrap()`，然后进入我们提供的 `main()` 和 monitor。

## 5. 编译

```powershell
cd C:\Users\Administrator\Desktop\work2
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_all.ps1
```

或分开：

```powershell
.\build_kernel.ps1     # -> build\bsd44.elf
.\build_iso.ps1        # -> out\BSD44-ELF-x86.iso
.\verify.ps1           # QEMU 启动并检查
```

## 6. 启动

**QEMU**

```powershell
& "C:\Program Files\qemu\qemu-system-i386.exe" -cdrom out\BSD44-ELF-x86.iso -boot d -m 64 -serial stdio
```

**VMware**：`vms\BSD44-ELF-x86.vmx` 已指向 `..\out\BSD44-ELF-x86.iso`，
在 VMware Workstation 里 File → Open 打开该 vmx 即可。ISO 是标准
El Torito，VMware 的 PhoenixBIOS 直接识别。

## 7. 为现代 GCC 做的移植修补

4.4BSD-Lite2 的 i386 代码是 1993 年用旧 GCC + a.out 工具链写的，直接喂给
GCC 16 + PE 链接器需要少量修正（均已写入源码树，`src/` 中是修补后的副本）：

| 文件 | 修补 | 原因 |
|---|---|---|
| `i386/i386/locore.s` | `ENTRY/ALTENTRY/IDTVEC` 改 `_##name`；`movb %eax`→`%al`；`movl %ax,%ds`→`movw` | 现代 cpp 把 `/**/` 变成空格；gas 拒绝寄存器宽度不匹配 |
| `i386/i386/locore.s` | 增加 `#include "icu.h"`、`"isa.h"`（在 `icu.s` 中） | 原构建由 config 生成的 `isa.h` 提供 `IO_ICU1` 等常量 |
| `i386/i386/pmap.c` | `static boolean_t pmap_testbit();` | C99 起 static 后置声明是错误 |
| `i386/i386/machdep.c` | `sendsig(..., u_long code)`、`extern const char *panicstr` | 与头文件原型一致 |
| `i386/i386/machdep.c` | `lgdt(&r_gdt); lidt(&r_idt);` 取代两参调用 | 本树 `locore.s` 的 `lgdt` 只接受 `region_descriptor *` |
| `i386/include/segments.h` | `region_descriptor` 改 6 字节 packed；`segment_descriptor` 改为两个 32 位字 | GCC 16 的 16+24 位域跨 32 位单元，结构体变 12 字节，GDT 步长错 |
| `i386/include/types.h`（生成视图） | 补 `register_t` | 本 tarball 的 i386 头缺此 typedef |
| `machine/ansi.h`、`limits.h`、`float.h` | 从 `hp300/include` 借通用定义 | i386 目录缺这三个机器头 |
| `machine/endian.h`、`machine/stdarg.h` | 新建 | i386 tarball 缺；`stdarg.h` 转发到 GCC 内建 |

另外：`-fcommon`（K&R 暂定定义）、`-fno-pie -fno-pic`、`-mno-ms-bitfields`、
`-mpreferred-stack-boundary=2`、`-ffunction-sections` + `--gc-sections`
（丢掉本 bring-up 内核不用的 machdep 函数）。

## 8. 边界说明（重要）

这是一颗 **可启动的 4.4BSD-Lite2 i386 内核固件**：机器相关核心
（`locore.s`、`pmap.c`、`machdep.c`、`cons.c`、`icu.s`）是源码树里的原文件；
它完成保护模式、分页、GDT/IDT/TSS、物理内存探测、Mach pmap 引导，并提供
板载 monitor。

它**不是**完整的 4.4BSD 用户态系统：没有 VFS/root 文件系统、没有 `init(8)`、
没有 `exec` 用户进程，因此 ISO 里没有 BSD 的 `/bin/sh` 等 a.out/ELF 用户程序。
“ISO 内装 ELF 内核 + El Torito 引导” 这一目标已达成；内核本身是
`ELF32 EXEC, Intel 80386`，不是 PE。

如果后续要跑用户态，需要在这颗内核上补 `vm_page`/`vm_map` 初始化、块设备
驱动（wd/ata）、FFS 与 `execve`，再把 4.4BSD 的 `libc`/`sh` 交叉编译成
i386 ELF 放进 ISO。当前的 `pmap_enter`/`pmap_map` 已经可用，是这条路的起点。
