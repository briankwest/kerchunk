/*
 * kerchunk_ini_edit.c — see header.
 */

#include "kerchunk_ini_edit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Slurp a file into a NUL-terminated buffer. Caller frees. NULL on
 * error. *out_len is set to the byte length (excluding NUL). */
static char *file_slurp(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(fp); return NULL; }
    if ((long)fread(buf, 1, (size_t)n, fp) != n) {
        free(buf); fclose(fp); return NULL;
    }
    buf[n] = '\0';
    fclose(fp);
    if (out_len) *out_len = (size_t)n;
    return buf;
}

/* Atomic write: temp + rename. Preserves the destination's uid/gid +
 * mode so a chown'd config file stays correctly owned. */
static int file_atomic_write(const char *path, const char *data, size_t len)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());

    struct stat st;
    int have_st = (stat(path, &st) == 0);
    mode_t mode = have_st ? (st.st_mode & 0777) : 0640;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    if (have_st) {
        /* Best-effort chown. Failure is non-fatal — file ends up owned
         * by whatever the writer is. */
        if (fchown(fd, st.st_uid, st.st_gid) != 0) { /* ignore */ }
    }

    ssize_t off = 0;
    while ((size_t)off < len) {
        ssize_t w = write(fd, data + off, len - (size_t)off);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd); unlink(tmp); return -1;
        }
        off += w;
    }
    if (fsync(fd) != 0) { close(fd); unlink(tmp); return -1; }
    if (close(fd) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* Find the start of `[section]` followed by newline or EOF. Returns the
 * byte offset to '[', or (size_t)-1 if not found. */
static size_t find_section_header(const char *s, size_t len,
                                  const char *section)
{
    size_t sl = strlen(section);
    size_t i = 0;
    while (i < len) {
        size_t line_start = i;
        if (s[i] == '[' &&
            i + 1 + sl + 1 <= len &&
            memcmp(s + i + 1, section, sl) == 0 &&
            s[i + 1 + sl] == ']' &&
            (i + 1 + sl + 1 == len ||
             s[i + 1 + sl + 1] == '\n' ||
             s[i + 1 + sl + 1] == '\r'))
            return line_start;

        const char *nl = memchr(s + i, '\n', len - i);
        if (!nl) break;
        i = (size_t)(nl - s) + 1;
    }
    return (size_t)-1;
}

/* Walk forward from the line CONTAINING `from` until the next "[" at
 * column 0 of a line. Returns the offset to that header's '[', or len
 * if EOF first. */
static size_t find_next_section_header(const char *s, size_t len, size_t from)
{
    size_t i = from;
    const char *nl = memchr(s + i, '\n', len - i);
    if (!nl) return len;
    i = (size_t)(nl - s) + 1;
    while (i < len) {
        if (s[i] == '[') return i;
        nl = memchr(s + i, '\n', len - i);
        if (!nl) return len;
        i = (size_t)(nl - s) + 1;
    }
    return len;
}

int kerchunk_ini_replace_section(const char *path, const char *section,
                                 const char *body)
{
    size_t len;
    char  *src = file_slurp(path, &len);
    if (!src) return -1;

    size_t hdr = find_section_header(src, len, section);

    /* New section text: "[section]\n" + body (caller-supplied, expected
     * to end with "\n"). */
    size_t body_len = body ? strlen(body) : 0;
    size_t new_section_len = strlen(section) + 3 /* "[", "]", "\n" */ + body_len;
    char  *new_section = malloc(new_section_len + 1);
    if (!new_section) { free(src); return -1; }
    snprintf(new_section, new_section_len + 1, "[%s]\n%s", section,
             body ? body : "");

    char  *out;
    size_t out_len;

    if (hdr == (size_t)-1) {
        /* Append at EOF. Pad with newlines so we don't run into the
         * previous line. */
        const char *prefix =
            (len >= 2 && src[len-2] == '\n' && src[len-1] == '\n') ? ""
          : (len >= 1 && src[len-1] == '\n')                       ? "\n"
                                                                   : "\n\n";
        out_len = len + strlen(prefix) + new_section_len;
        out = malloc(out_len + 1);
        if (!out) { free(src); free(new_section); return -1; }
        memcpy(out,                        src,         len);
        memcpy(out + len,                  prefix,      strlen(prefix));
        memcpy(out + len + strlen(prefix), new_section, new_section_len);
        out[out_len] = '\0';
    } else {
        size_t end = find_next_section_header(src, len, hdr);
        out_len = hdr + new_section_len + (len - end);
        out = malloc(out_len + 1);
        if (!out) { free(src); free(new_section); return -1; }
        memcpy(out,                          src,         hdr);
        memcpy(out + hdr,                    new_section, new_section_len);
        memcpy(out + hdr + new_section_len,  src + end,   len - end);
        out[out_len] = '\0';
    }

    int rc = file_atomic_write(path, out, out_len);
    free(out); free(new_section); free(src);
    return rc;
}

int kerchunk_ini_remove_section(const char *path, const char *section)
{
    size_t len;
    char  *src = file_slurp(path, &len);
    if (!src) return -1;

    size_t hdr = find_section_header(src, len, section);
    if (hdr == (size_t)-1) { free(src); return 0; }   /* idempotent */

    size_t end = find_next_section_header(src, len, hdr);
    /* Gobble trailing blank lines so we don't leave double blanks. */
    while (end < len && (src[end] == '\n' || src[end] == '\r')) end++;
    /* If there's content after, restore one separator newline. */
    int need_sep = (end < len && hdr > 0 && src[hdr - 1] == '\n');

    size_t out_len = hdr + (need_sep ? 1 : 0) + (len - end);
    char  *out = malloc(out_len + 1);
    if (!out) { free(src); return -1; }
    memcpy(out, src, hdr);
    if (need_sep) out[hdr] = '\n';
    memcpy(out + hdr + (need_sep ? 1 : 0), src + end, len - end);
    out[out_len] = '\0';

    int rc = file_atomic_write(path, out, out_len);
    free(out); free(src);
    return rc;
}
