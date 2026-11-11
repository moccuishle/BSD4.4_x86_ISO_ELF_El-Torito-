# BSD44-ELF-x86 任务恢复文本（暂停点）

生成时间：2026-11-12 07:55（Asia/Shanghai）
状态：**暂停（paused）**，下一步从 S3 继续。
仓库：https://github.com/moccuishle/BSD4.4_x86_ISO_ELF_El-Torito-
最新提交：`d75bffc`（S2：ramfs / init / execve / bin/sh）
上一提交：`2059f70`（S1：ring3 + ELF32 loader + int 0x80）

---

## 1. 任务目标（原始需求）

在 4.4BSD-Lite2 i386 ELF 内核上实现 Solaris 式的 VFS/root 文件系统、
`init(8)` 与 `execve`，把 4.4BSD libc/sh 交叉编译成 i386 ELF 用户程序放进
ISO，最终 ISO 启动后进入用户态运行 BSD 的 `/bin/sh`。

硬约束：
- 用 `C:\msys64` 的 mingw32 **i386** GCC 编译；
- 产物是 UNIX ELF32（不是 PE），ISO 用 El Torito 引导；
- 不使用 SIMD（`-march=i386 -mno-sse -mno-mmx`）。

---

## 2. 当前完成度

| 阶段 | 内容 | 状态 |
|---|---|---|
| S0 | 用户态 ELF 交叉编译链（COFF→PE→objcopy） | ✅ |
| S1 | ring3、用户页目录/栈、ELF32 装载、`int 0x80`、write/exit | ✅ 已验证 |
| S2 | ramfs、fd 表、open/read/close、`execve`、`/sbin/init` → `/bin/sh` | ✅ 已验证（小 sh） |
| S3 | 4.4BSD libc 子集 + **真正的 BSD `/bin/sh`** | ⬜ 未开始（最大一块） |
| S4 | 真 VFS/vnode、vm_map、FFS 或 ISO9660 root、从光盘挂载 | ⬜ 未开始 |

重要区别：现在的 `/bin/sh` 是本项目写的**最小内置命令 sh**（echo/help/exit），
**不是** 4.4BSD 的 `usr/src/bin/sh`。ISO 里的用户程序目前是**嵌入内核数组**
（ramfs），ISO9660 目录里还没有 `/bin/sh` 文件——这两点是 S3/S4 要补的。

---

## 3. 最后一次实测输出（QEMU，当前 ISO）

```
bsd44> usermode
exec /sbin/init: entry 8048000 pd 200000
init: starting /bin/sh
sh> echo hi
hi
sh> 
```

即：monitor → `exec /sbin/init` → `init` 打印 → `execve("/bin/sh")` →
sh 读命令行 → `echo hi` 输出 `hi`。

已知小问题：自动化测试发送 `exit` 时 QEMU sendkey 时序导致只收到 `exi`，
`exit` 路径尚未完整回归；`sendkey` 对 `-` 等字符要用键名（如 `minus`）。

---

## 4. 代码结构（工作区）

工程根：`C:\Users\Administrator\Documents\deepseek-harness\default-workspace\bsd44`
交付副本：`C:\Users\Administrator\Desktop\work2`（ISO/ELF/bsd44 都在）

| 文件 | 作用 |
|---|---|
| `kernel/usermode.c` | 用户页目录/栈、ELF32 装载、ramfs、fd 表、syscall 分发、`execve`、`start_userland` |
| `kernel/usermode_asm.s` | `enter_user`（切 CR3 + iret ring3）、`int 0x80` 入口 |
| `kernel/glue.c` | `proc0`、kprintf、`vm_set_page_size`、`trap`、`main` |
| `kernel/mon.c` | monitor：`usermode/mem/pte/map/banner/reboot/halt` |
| `kernel/pccon.c` | VGA + COM1 控制台 |
| `userland/init.c` | `init(8)` 雏形：打印后 `execve("/bin/sh")` |
| `userland/sh.c` | 最小 sh：echo/help/exit |
| `userland/hello.c` | ring3 hello（调试用） |
| `build_userland.ps1` | 编 hello/init/sh 三个 ELF32 |
| `build_kernel.ps1` | 解包 tarball、应用 overlay、genassym、编内核、把用户 ELF 嵌成 C 数组、链接成 ELF |
| `build_iso.ps1` | 编 stage1/stage2、mkisofs、追加内核（按 2048 对齐） |
| `build_all.ps1` | 依次 userland → kernel → ISO |
| `tools/test_s2.ps1` | QEMU 自动化：等提示符、发 usermode/echo、抓串口 |
| `docs/USERLAND-PLAN.md` | S0–S4 完整路线、接口、验收、风险 |
| `overlay/` | 针对 GCC16/PE 修补过的 4.4BSD 源文件（locore.s/pmap.c/machdep.c/icu.s/segments.h） |

当前尺寸：`BSD44.ELF` ≈ 336,908 B；`BSD44-ELF-x86.iso` ≈ 4,532,224 B。

---

## 5. 当前内核实现要点

- 用户页目录：新分配一页，复制内核 PDE `0x3F6–0x3FA`（含 proc0 内核栈）；
- 用户映射：PDE/PTE = `PG_V|PG_RW|PG_u`（注意是 `PG_u`=4，不是 `PG_U`=0x20）；
- 用户栈：`0xBFBF8000–0xBFC00000`，ESP=`0xBFC00000-16`；
- 段选择子：`_ucodesel=0x1f`、`_udatasel=0x27`（LDT）；
- syscall：`int 0x80`，DPL=3，号沿用 4.4BSD：exit=1, read=3, write=4,
  open=5, close=6, brk=17, execve=59；
- `execve`：按路径查 ramfs → 新建 PD/装载 PT_LOAD/建栈 →
  `mov cr3,newpd` → 改 iret 帧的 eip/uesp；
- ramfs 文件：`/sbin/init`、`/bin/sh`、`/hello`（内核数组，只读）；
- 尚无 fork/vfork/wait、信号、termios、pipe、块设备、真 VFS vnode。

---

## 6. 构建 / 运行 / 测试

```powershell
cd C:\Users\Administrator\Documents\deepseek-harness\default-workspace\bsd44
powershell -NoProfile -ExecutionPolicy Bypass -File .\build_all.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\test_s2.ps1
```

手动 QEMU：

```powershell
& "C:\Program Files\qemu\qemu-system-i386.exe" `
  -cdrom out\BSD44-ELF-x86.iso -boot d -m 64 -serial stdio
```

启动后在 monitor 输入 `usermode`（串口/VGA 键盘均可），即可看到
init→sh 流程。VMware 用 `bsd44\vms\BSD44-ELF-x86.vmx`。

工具链自动探测在 `bsd44/tools-find.ps1`（MSYS2/mkisofs/QEMU，可用
`MSYS2_ROOT`/`MKISOFS`/`QEMU` 覆盖）。GCC include 由
`gcc -print-file-name=include` 得到，不再写死 16.2.0。

---

## 7. 已踩过并修复的关键坑（下次别重犯）

1. `pmap_map()` **没有 pmap 参数**：`pmap_map(virt, start, end, prot)`；
   多传 `kernel_pmap` 会把 `kernel_pmap_store` 的映射覆盖掉。
2. `locore.s` 的 `calltrap` 原本没压 trapframe 指针，`trap()` 会收到选择子；
   已加 `pushl %esp` / `addl $4,%esp`。
3. `idt[32+16]` 放不下 `int 0x80`，`machdep.c` 已改成 `idt[256]`。
4. 用户 PTE 必须用 `PG_u`(0x4)；用 `PG_U`(0x20) 会变成 supervisor 页，
   取指即 #PF(e=5)。
5. 用户页目录必须复制 PDE `0x3F6`（proc0 内核栈），否则异常压栈就 #PF。
6. ISO 末尾的内核必须 **补零到 2048 字节对齐**；否则 stage1 最后一次
   CD 读越过镜像末尾，BIOS 整块返回 0，内核尾部/rdata 丢失。
7. stage1 用 CD 的 2048 字节扇区计数（不是 512）；每块最多 16 扇区。
8. `build_kernel.ps1` 解包要用 Windows `System32\tar.exe`；PATH 里的
   MSYS2 GNU tar 会错误处理 Windows `-C` 路径。
9. `build_iso.ps1` 的 `$ErrorActionPreference` 必须是 Continue，否则 ld 的
   “section below image base” 警告会中止脚本。
10. GCC16 下 `region_descriptor` 必须 6 字节 packed、`segment_descriptor`
    必须是 8 字节（16+24 位域会跨 32 位单元）。

---

## 8. 下一步（S3，建议从这里恢复）

1. 先补完 S2 的边角：验证 `exit`、sh 行编辑、错误命令；
2. **移植 4.4BSD libc 子集**（`usr/src/lib/libc` 里 sh 需要的：
   crt0/syscall stubs/stdio/malloc/string/ctype/errno；signal/termios 先 stub）；
3. **编译真正的 4.4BSD `/bin/sh`**（`usr/src/bin/sh`），修 K&R/ANSI 与头文件冲突，
   链接成静态 i386 ELF（`0x08048000`）；
4. 把它放进 ramfs（替换现在的 `userland/sh.c`），验收：
   `sh -c 'echo hello; exit 3'` → 输出 hello、退出码 3；
5. 再考虑 S4：真 VFS/vnode、vm_map、FFS 或 ISO9660 root，把 `/bin/sh`
   作为 ISO9660 文件而不是内核数组。

恢复方式：打开本文件，按第 6 节构建；从第 8 节继续。goal 已暂停，
下次说“继续 BSD44 用户态任务”即可恢复。
