/* init(8) - first user process; execs /bin/sh */
static int
sys_write(int fd, const char *b, unsigned n)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(4), "b"(fd), "c"(b), "d"(n) : "memory");
	return r;
}

static int
sys_execve(const char *path)
{
	int r;
	__asm__ volatile("int $0x80" : "=a"(r) : "a"(59), "b"(path) : "memory");
	return r;
}

static void
sys_exit(int code)
{
	__asm__ volatile("int $0x80" :: "a"(1), "b"(code));
	for (;;)
		;
}

void
_start(void)
{
	static const char msg[] = "init: starting /bin/sh\n";

	sys_write(1, msg, sizeof(msg) - 1);
	if (sys_execve("/bin/sh") < 0)
		sys_write(1, "init: exec failed\n", 18);
	sys_exit(1);
}
