/* SPDX-License-Identifier: MIT
 * Stand-in for Android's liblog on a Linux host. Prints and records lines. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static char g_lines[1024][256];
static int g_count;

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap)
{
    char buf[256];
    vsnprintf(buf, sizeof buf, fmt, ap);
    fprintf(stderr, "  [%d %s] %s\n", prio, tag, buf);
    if (g_count < 1024)
        strcpy(g_lines[g_count++], buf);
    return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

/* number of logged lines containing `needle` */
int fake_log_count(const char *needle)
{
    int n = 0;
    for (int i = 0; i < g_count; i++)
        n += strstr(g_lines[i], needle) != NULL;
    return n;
}
