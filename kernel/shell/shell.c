/* The command shell. One instance runs on the full-screen console and one in
 * every GUI terminal window; each instance is a kernel task bound to a TTY.
 * Anything that is not a built-in is looked up as a program in /bin. */
#include "../include/shell.h"
#include "../include/task.h"
#include "../include/fs.h"
#include "../include/mm.h"
#include "../include/drivers.h"
#include "../include/string.h"
#include "../include/kprintf.h"
#include "../include/gfx.h"

#define LINE_MAX  256
#define MAX_ARGS  16
#define HIST_MAX  16
#define OS_VERSION "ClaudeOS 1.0 (i386)"

typedef struct {
    tty_t *tty;
    char   cwd[128];
    char   hist[HIST_MAX][LINE_MAX];
    int    hcount;
    bool   quit;
} shell_t;

typedef struct {
    const char *name, *args, *help;
    void (*fn)(shell_t *sh, int argc, char **argv);
} command_t;

#define OUT(...) tty_printf(sh->tty, __VA_ARGS__)

static void err(shell_t *sh, const char *cmd, const char *what, int code) {
    OUT("\x1b[91m%s: %s: %s\x1b[0m\n", cmd, what, fs_strerror(code));
}

static void abspath(shell_t *sh, const char *p, char *out) { path_normalize(sh->cwd, p, out, 128); }

/* ---- line editor --------------------------------------------------------- */
static void prompt(shell_t *sh) {
    OUT("\x1b[92mroot@claudeos\x1b[0m:\x1b[94m%s\x1b[0m$ ", sh->cwd);
}

static void replace_line(shell_t *sh, char *buf, int *len, const char *with) {
    while (*len) { tty_puts(sh->tty, "\b \b"); (*len)--; }
    strlcpy(buf, with, LINE_MAX);
    *len = strlen(buf);
    tty_puts(sh->tty, buf);
}

/* Returns line length, or -1 if the terminal was closed. */
static int readline(shell_t *sh, char *buf) {
    int len = 0, hpos = sh->hcount;
    buf[0] = 0;
    for (;;) {
        int k = tty_getkey(sh->tty);
        if (k < 0) return -1;
        if (k == '\n') { tty_puts(sh->tty, "\n"); break; }
        if (k == '\b') {
            if (len) { len--; buf[len] = 0; tty_puts(sh->tty, "\b \b"); }
        } else if (k == 3) {                          /* Ctrl+C: discard line */
            tty_puts(sh->tty, "^C\n");
            len = 0;
            break;
        } else if (k == 12) {                         /* Ctrl+L: clear screen */
            tty_puts(sh->tty, "\x1b[2J\x1b[H");
            prompt(sh);
            tty_puts(sh->tty, buf);
        } else if (k == KEY_UP && hpos > 0) {
            replace_line(sh, buf, &len, sh->hist[--hpos]);
        } else if (k == KEY_DOWN && hpos < sh->hcount) {
            hpos++;
            replace_line(sh, buf, &len, hpos < sh->hcount ? sh->hist[hpos] : "");
        } else if (k >= 32 && k < 127 && len < LINE_MAX - 1) {
            buf[len++] = (char)k;
            buf[len] = 0;
            char c = (char)k;
            tty_write(sh->tty, &c, 1);
        }
    }
    buf[len] = 0;
    if (len && (sh->hcount == 0 || strcmp(sh->hist[sh->hcount - 1], buf))) {
        if (sh->hcount == HIST_MAX) {
            memmove(sh->hist[0], sh->hist[1], (HIST_MAX - 1) * LINE_MAX);
            sh->hcount--;
        }
        strlcpy(sh->hist[sh->hcount++], buf, LINE_MAX);
    }
    return len;
}

/* Split a line into words; "double quotes" group words. */
static int tokenize(char *line, char **argv) {
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = 0;
    }
    argv[argc] = NULL;
    return argc;
}

/* ---- built-in commands --------------------------------------------------- */
static const command_t commands[];

static void cmd_help(shell_t *sh, int argc, char **argv) {
    UNUSED(argc); UNUSED(argv);
    OUT("\x1b[93mBuilt-in commands:\x1b[0m\n");
    for (const command_t *c = commands; c->name; c++) {
        char left[40];
        snprintf(left, sizeof(left), "%s %s", c->name, c->args);
        OUT("  \x1b[96m%-22s\x1b[0m %s\n", left, c->help);
    }
    OUT("\x1b[93mPrograms in /bin\x1b[0m run by name (e.g. \x1b[96mhello\x1b[0m), "
        "append \x1b[96m&\x1b[0m to run in background.\n"
        "Keys: \x1b[96mF12\x1b[0m CLI<->GUI, \x1b[96mCtrl+C\x1b[0m stop program, "
        "\x1b[96mUp/Down\x1b[0m history, \x1b[96mCtrl+L\x1b[0m clear.\n"
        "Commands can be shortened while unambiguous: \x1b[96mhel\x1b[0m = help, \x1b[96mupt\x1b[0m = uptime.\n");
}

static void cmd_ls(shell_t *sh, int argc, char **argv) {
    char path[128];
    abspath(sh, argc > 1 ? argv[1] : ".", path);
    fs_entry_t st;
    int r = fs_stat(path, &st);
    if (r < 0) { err(sh, "ls", argc > 1 ? argv[1] : path, r); return; }
    if (!st.is_dir) { OUT("%8u  %s\n", st.size, st.name); return; }
    fs_entry_t *list = kmalloc(sizeof(fs_entry_t) * 128);
    if (!list) return;
    int n = fs_list(path, list, 128);
    if (n < 0) err(sh, "ls", path, n);
    for (int i = 0; i < n; i++) {
        if (list[i].is_dir) OUT("   <DIR>  \x1b[94m%s/\x1b[0m\n", list[i].name);
        else OUT("%8u  %s\n", list[i].size, list[i].name);
    }
    if (n == 0) OUT("(empty)\n");
    kfree(list);
}

static void cmd_cd(shell_t *sh, int argc, char **argv) {
    char path[128];
    abspath(sh, argc > 1 ? argv[1] : "/", path);
    fs_entry_t st;
    int r = fs_stat(path, &st);
    if (r < 0) { err(sh, "cd", argv[1], r); return; }
    if (!st.is_dir) { err(sh, "cd", argv[1], FS_ENOTDIR); return; }
    strlcpy(sh->cwd, path, sizeof(sh->cwd));
}

static void cmd_pwd(shell_t *sh, int argc, char **argv) { UNUSED(argc); UNUSED(argv); OUT("%s\n", sh->cwd); }

static void cmd_cat(shell_t *sh, int argc, char **argv) {
    if (argc < 2) { OUT("usage: cat <file>...\n"); return; }
    char buf[513];
    for (int i = 1; i < argc; i++) {
        char path[128];
        abspath(sh, argv[i], path);
        uint32_t off = 0;
        int n;
        while ((n = fs_read(path, off, buf, 512)) > 0) {
            tty_write(sh->tty, buf, n);
            off += n;
        }
        if (n < 0) err(sh, "cat", argv[i], n);
        else if (off && buf[(off - 1) % 512] != '\n') OUT("\n");
    }
}

static void cmd_echo(shell_t *sh, int argc, char **argv) {
    char text[LINE_MAX] = "";
    const char *target = NULL;
    bool append = false;
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], ">") || !strcmp(argv[i], ">>")) && i + 1 < argc) {
            append = argv[i][1] == '>';
            target = argv[++i];
            continue;
        }
        if (text[0]) strlcat(text, " ", sizeof(text));
        strlcat(text, argv[i], sizeof(text));
    }
    strlcat(text, "\n", sizeof(text));
    if (!target) { tty_puts(sh->tty, text); return; }
    char path[128];
    abspath(sh, target, path);
    int r = fs_write(path, text, strlen(text), append);
    if (r < 0) err(sh, "echo", target, r);
}

static void cmd_touch(shell_t *sh, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        char path[128];
        fs_entry_t st;
        abspath(sh, argv[i], path);
        if (fs_stat(path, &st) == 0) continue;
        int r = fs_write(path, "", 0, false);
        if (r < 0) err(sh, "touch", argv[i], r);
    }
    if (argc < 2) OUT("usage: touch <file>...\n");
}

static void cmd_mkdir(shell_t *sh, int argc, char **argv) {
    if (argc < 2) { OUT("usage: mkdir <dir>...\n"); return; }
    for (int i = 1; i < argc; i++) {
        char path[128];
        abspath(sh, argv[i], path);
        int r = fs_mkdir(path);
        if (r < 0) err(sh, "mkdir", argv[i], r);
    }
}

static void cmd_rm(shell_t *sh, int argc, char **argv) {
    if (argc < 2) { OUT("usage: rm <file|empty dir>...\n"); return; }
    for (int i = 1; i < argc; i++) {
        char path[128];
        abspath(sh, argv[i], path);
        int r = fs_remove(path);
        if (r < 0) err(sh, "rm", argv[i], r);
    }
}

static int copy_file(shell_t *sh, const char *src, const char *dst, const char *cmd) {
    char from[128], to[128];
    fs_entry_t st, dst_st;
    abspath(sh, src, from);
    abspath(sh, dst, to);
    int r = fs_stat(from, &st);
    if (r == 0 && st.is_dir) r = FS_EISDIR;
    if (r < 0) { err(sh, cmd, src, r); return r; }
    if (fs_stat(to, &dst_st) == 0 && dst_st.is_dir) {      /* copy into directory */
        const char *base = strrchr(from, '/');
        if (strcmp(to, "/")) strlcat(to, "/", sizeof(to));
        strlcat(to, base + 1, sizeof(to));
    }
    if (!strcmp(from, to)) { OUT("%s: source and destination are the same\n", cmd); return -1; }
    uint8_t *buf = kmalloc(st.size + 1);
    if (!buf) { err(sh, cmd, src, FS_ENOSPC); return FS_ENOSPC; }
    r = fs_read(from, 0, buf, st.size);
    if (r >= 0) r = fs_write(to, buf, st.size, false);
    kfree(buf);
    if (r < 0) err(sh, cmd, dst, r);
    return r;
}

static void cmd_cp(shell_t *sh, int argc, char **argv) {
    if (argc != 3) { OUT("usage: cp <source> <dest>\n"); return; }
    copy_file(sh, argv[1], argv[2], "cp");
}

static void cmd_mv(shell_t *sh, int argc, char **argv) {
    if (argc != 3) { OUT("usage: mv <source> <dest>\n"); return; }
    if (copy_file(sh, argv[1], argv[2], "mv") >= 0) {
        char from[128];
        abspath(sh, argv[1], from);
        fs_remove(from);
    }
}

static void cmd_clear(shell_t *sh, int argc, char **argv) {
    UNUSED(argc); UNUSED(argv);
    tty_puts(sh->tty, "\x1b[2J\x1b[H");
}

static void cmd_reboot(shell_t *sh, int argc, char **argv) {
    UNUSED(argc); UNUSED(argv);
    OUT("Rebooting...\n");
    task_sleep(200);
    power_reboot();
}

static void cmd_shutdown(shell_t *sh, int argc, char **argv) {
    UNUSED(argc); UNUSED(argv);
    OUT("Shutting down...\n");
    task_sleep(200);
    power_shutdown();
}

static void cmd_ps(shell_t *sh, int argc, char **argv) {
    UNUSED(argc); UNUSED(argv);
    OUT("\x1b[93m  PID  STATE      CPU(ms)  MEM(KB)  TYPE    NAME\x1b[0m\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        task_t *t = task_by_index(i);
        if (t->state == T_UNUSED) continue;
        uint32_t mem = KSTACK_SIZE / 1024 + paging_count_user_pages(t->pd) * 4;
        OUT("%5d  %-9s %8u %8u  %-6s  %s\n", t->pid, task_state_name(t->state), t->cpu_ticks,
            t->pid == 0 ? 16 : mem, t->is_user ? "user" : "kernel", t->name);
    }
}

static void cmd_kill(shell_t *sh, int argc, char **argv) {
    if (argc < 2) { OUT("usage: kill <pid>\n"); return; }
    int pid = atoi(argv[1]);
    if (task_kill(pid) < 0) OUT("kill: cannot kill pid %d (no such task or protected)\n", pid);
    else OUT("killed %d\n", pid);
}

static void cmd_mem(shell_t *sh, int argc, char **argv) {
    UNUSED(argc); UNUSED(argv);
    uint32_t tot = pmm_total_frames() * 4, fr = pmm_free_frames() * 4;
    uint32_t ht, hu, hb;
    heap_stats(&ht, &hu, &hb);
    OUT("Physical memory: %u KB total, %u KB used, %u KB free\n", tot, tot - fr, fr);
    OUT("Kernel heap:     %u KB total, %u KB used in %u blocks\n", ht / 1024, hu / 1024, hb);
    OUT("Page size 4 KB; kernel identity-mapped 0-1 GB; user space 0x40000000-0x7FFFFFFF\n");
}

static void cmd_uptime(shell_t *sh, int argc, char **argv) {
    UNUSED(argc); UNUSED(argv);
    uint32_t s = timer_ticks() / 1000;
    OUT("up %u:%02u:%02u (%u ms)\n", s / 3600, (s / 60) % 60, s % 60, timer_ticks());
}

static void cmd_date(shell_t *sh, int argc, char **argv) {
    UNUSED(argc); UNUSED(argv);
    rtc_time_t t;
    rtc_read(&t);
    OUT("%04d-%02d-%02d %02d:%02d:%02d (RTC)\n", t.year, t.month, t.day, t.hour, t.min, t.sec);
}

static void cmd_gui(shell_t *sh, int argc, char **argv) {
    UNUSED(argc); UNUSED(argv);
    if (display_set_mode(DISPLAY_GUI) < 0) OUT("gui: no framebuffer (booted in text mode)\n");
}

static void cmd_cli(shell_t *sh, int argc, char **argv) {
    UNUSED(sh); UNUSED(argc); UNUSED(argv);
    display_set_mode(DISPLAY_CLI);
}

static void cmd_uname(shell_t *sh, int argc, char **argv) {
    UNUSED(argc); UNUSED(argv);
    OUT("%s, built " __DATE__ " " __TIME__ "\n", OS_VERSION);
    if (gfx_available()) OUT("Display: %dx%d framebuffer\n", gfx_width, gfx_height);
    else OUT("Display: VGA text mode 80x25\n");
}

static void cmd_df(shell_t *sh, int argc, char **argv) {
    UNUSED(argc); UNUSED(argv);
    uint32_t total, freeb;
    if (!fs_device()) { OUT("no file system mounted\n"); return; }
    fs_usage(&total, &freeb);
    OUT("Device  Type    Size(KB)  Used(KB)  Free(KB)\n");
    OUT("%-6s  %-6s %9u %9u %9u\n", fs_device(), fs_type(), total / 1024, (total - freeb) / 1024, freeb / 1024);
}

static void cmd_mount(shell_t *sh, int argc, char **argv) {
    if (argc < 2) {
        OUT("Block devices:\n");
        for (int i = 0; i < MAX_BLOCKDEVS; i++) {
            blockdev_t *d = blockdev_get(i);
            if (d) OUT("  %-5s %6u KB%s\n", d->name, d->sectors / 2,
                       fs_device() && !strcmp(fs_device(), d->name) ? "  (mounted on /)" : "");
        }
        OUT("usage: mount <device>   e.g. mount hd0\n");
        return;
    }
    blockdev_t *d = blockdev_find(argv[1]);
    if (!d) { OUT("mount: no such device: %s\n", argv[1]); return; }
    int r = fs_mount(d);
    if (r < 0) err(sh, "mount", argv[1], r);
    else { strlcpy(sh->cwd, "/", sizeof(sh->cwd)); OUT("mounted %s on /\n", argv[1]); }
}

static void cmd_sleep(shell_t *sh, int argc, char **argv) {
    UNUSED(sh);
    task_sleep(argc > 1 ? (uint32_t)atoi(argv[1]) : 1000);
}

static void cmd_history(shell_t *sh, int argc, char **argv) {
    UNUSED(argc); UNUSED(argv);
    for (int i = 0; i < sh->hcount; i++) OUT("%3d  %s\n", i + 1, sh->hist[i]);
}

static void cmd_exit(shell_t *sh, int argc, char **argv) {
    UNUSED(argc); UNUSED(argv);
    if (sh->tty->is_console) OUT("exit: this is the system console (use shutdown or reboot)\n");
    else sh->quit = true;
}

static void cmd_run(shell_t *sh, int argc, char **argv);

static const command_t commands[] = {
    { "help",     "",              "show this help (also: h, ?)",       cmd_help },
    { "ls",       "[path]",        "list directory contents",           cmd_ls },
    { "cd",       "[dir]",         "change directory",                  cmd_cd },
    { "pwd",      "",              "print working directory",           cmd_pwd },
    { "cat",      "<file>...",     "print file contents",               cmd_cat },
    { "echo",     "<text> [>|>> f]", "print text or write it to a file", cmd_echo },
    { "touch",    "<file>",        "create an empty file",              cmd_touch },
    { "mkdir",    "<dir>",         "create a directory",                cmd_mkdir },
    { "rm",       "<path>",        "remove a file or empty directory",  cmd_rm },
    { "cp",       "<src> <dst>",   "copy a file",                       cmd_cp },
    { "mv",       "<src> <dst>",   "move/rename a file",                cmd_mv },
    { "clear",    "",              "clear the screen",                  cmd_clear },
    { "ps",       "",              "list tasks",                        cmd_ps },
    { "kill",     "<pid>",         "terminate a task",                  cmd_kill },
    { "mem",      "",              "memory usage",                      cmd_mem },
    { "df",       "",              "file system usage",                 cmd_df },
    { "mount",    "[dev]",         "list devices / mount one on /",     cmd_mount },
    { "uptime",   "",              "time since boot",                   cmd_uptime },
    { "date",     "",              "current date and time",             cmd_date },
    { "uname",    "",              "system information",                cmd_uname },
    { "run",      "<prog> [args]", "run a program (also just: prog)",   cmd_run },
    { "sleep",    "[ms]",          "wait (default 1000 ms)",            cmd_sleep },
    { "history",  "",              "show command history",              cmd_history },
    { "gui",      "",              "switch to the graphical desktop",   cmd_gui },
    { "cli",      "",              "switch to the full-screen console", cmd_cli },
    { "exit",     "",              "close this terminal window",        cmd_exit },
    { "reboot",   "",              "restart the computer",              cmd_reboot },
    { "shutdown", "",              "power off (ACPI)",                  cmd_shutdown },
    { NULL, NULL, NULL, NULL },
};

/* ---- running programs ---------------------------------------------------- */
static bool exec_program(shell_t *sh, int argc, char **argv) {
    bool bg = false;
    if (argc > 1 && !strcmp(argv[argc - 1], "&")) { bg = true; argv[--argc] = NULL; }
    char path[128];
    if (strchr(argv[0], '/')) abspath(sh, argv[0], path);
    else snprintf(path, sizeof(path), "/bin/%s", argv[0]);
    int pid = elf_exec(path, argc, argv, sh->tty, bg, sh->cwd);
    if (pid == FS_ENOENT) return false;
    if (pid < 0) { OUT("%s: cannot execute: %s\n", argv[0], pid == FS_EINVAL ? "not a valid ELF program" : fs_strerror(pid)); return true; }
    if (bg) { OUT("[%d] %s running in background\n", pid, argv[0]); return true; }
    sh->tty->fg_pid = pid;
    int code = task_wait(pid);
    sh->tty->fg_pid = 0;
    if (code == -1) OUT("\x1b[93m[process %d terminated]\x1b[0m\n", pid);
    else if (code) OUT("\x1b[93m[exit code %d]\x1b[0m\n", code);
    return true;
}

static void cmd_run(shell_t *sh, int argc, char **argv) {
    if (argc < 2) { OUT("usage: run <program> [args] [&]\n"); return; }
    if (!exec_program(sh, argc - 1, argv + 1)) OUT("run: %s: program not found\n", argv[1]);
}

/* Short aliases for built-in commands. */
static const struct { const char *alias, *command; } aliases[] = {
    { "h",  "help" },
    { "?",  "help" },
    { NULL, NULL },
};

static const command_t *find_command(const char *name) {
    for (const command_t *c = commands; c->name; c++)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

/* An unambiguous prefix also works: "hel" -> help, "hist" -> history. */
static const command_t *find_by_prefix(shell_t *sh, const char *name, bool *ambiguous) {
    const command_t *found = NULL;
    size_t len = strlen(name);
    int matches = 0;
    *ambiguous = false;
    for (const command_t *c = commands; c->name; c++)
        if (!strncmp(c->name, name, len)) { found = c; matches++; }
    if (matches <= 1) return found;
    *ambiguous = true;
    OUT("%s: ambiguous, did you mean:", name);
    for (const command_t *c = commands; c->name; c++)
        if (!strncmp(c->name, name, len)) OUT(" %s", c->name);
    OUT("\n");
    return NULL;
}

static void execute(shell_t *sh, char *line) {
    char *argv[MAX_ARGS + 1];
    int argc = tokenize(line, argv);
    if (!argc) return;
    const command_t *c = find_command(argv[0]);
    for (int i = 0; !c && aliases[i].alias; i++)
        if (!strcmp(aliases[i].alias, argv[0])) c = find_command(aliases[i].command);
    if (c) { c->fn(sh, argc, argv); return; }
    if (exec_program(sh, argc, argv)) return;       /* programs in /bin win over prefixes */
    bool ambiguous;
    c = find_by_prefix(sh, argv[0], &ambiguous);
    if (c) c->fn(sh, argc, argv);
    else if (!ambiguous) OUT("%s: command not found (type 'help' or 'h')\n", argv[0]);
}

void shell_main(void *arg) {
    shell_t *sh = kzalloc(sizeof(shell_t));
    sh->tty = arg;
    strlcpy(sh->cwd, "/", sizeof(sh->cwd));
    char line[LINE_MAX];

    if (sh->tty->is_console) {
        char motd[1024];
        int n = fs_read("/etc/motd", 0, motd, sizeof(motd) - 1);
        if (n > 0) tty_write(sh->tty, motd, n);
        else OUT("Welcome to %s. Type 'help'.\n", OS_VERSION);
    } else {
        OUT("\x1b[96m%s terminal\x1b[0m - type 'help', 'exit' closes the window.\n", OS_VERSION);
    }

    while (!sh->quit) {
        prompt(sh);
        if (readline(sh, line) < 0) break;            /* window closed */
        execute(sh, line);
        if (sh->tty->hangup) break;
    }

    /* GUI terminal closed or 'exit': stop its programs and let go of the TTY.
     * The TTY is shared with the window and freed by whoever releases it last. */
    tty_t *tty = sh->tty;
    if (!tty->is_console) {
        tty->hangup = 1;
        task_kill_tty(tty);
        extern void gui_terminal_release(tty_t *tty);
        gui_terminal_release(tty);
    }
    kfree(sh);
}
