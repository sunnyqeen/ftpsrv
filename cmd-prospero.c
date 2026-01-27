/* Copyright (C) 2025 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/_iovec.h>
#include <sys/mount.h>

#include "cmd.h"
#include "self.h"
#include "elfldr.h"


/**
 * Convenient macros for nmount.
 **/
#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))
#define IOVEC_ENTRY(x) {x ? x : 0, x ? strlen(x)+1 : 0}


/**
 * Remount read-only mount points with write permissions.
 **/
int
ftp_cmd_MTRW(ftp_env_t *env, const char* arg) {
  struct iovec iov_sys[] = {
    IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system"),
    IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system"),
    IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
    IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
    IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
    IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
    IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
  };

  struct iovec iov_sysex[] = {
    IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
    IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
    IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
    IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
    IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
    IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
    IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
  };

  if(nmount(iov_sys, IOVEC_SIZE(iov_sys), MNT_UPDATE)) {
    return ftp_perror(env);
  }

  if(nmount(iov_sysex, IOVEC_SIZE(iov_sysex), MNT_UPDATE)) {
    return ftp_perror(env);
  }

  return ftp_active_printf(env, "226 /system and /system_ex remounted\r\n");
}

static char*
args_decode(const char* s) {
  size_t length = strlen(s);
  char *arg = malloc(length+1);
  size_t off = 0;
  int escape = 0;

  for(size_t i=0; i<length; i++) {
    if(s[i] == '\\' && !escape) {
      escape = 1;
    } else {
      arg[off++] = s[i];
      escape = 0;
    }
  }

  arg[off] = 0;
  return arg;
}

static int
args_split(const char* args, char** argv, size_t size) {
  char* buf = strdup(args);
  size_t len = strlen(buf);
  int escape = 0;
  int argc = 0;

  memset(argv, 0, size*sizeof(char*));
  for(int i=0; i<len && argc<size; i++) {
    if(escape) {
      escape = 0;
      continue;
    }

    if(buf[i] == '\\') {
      escape = 1;
      continue;
    }

    if(buf[i] == ' ') {
      buf[i] = 0;
      continue;
    }

    if(buf[i] && !i) {
      argv[argc++] = buf+i;
      continue;
    }

    if(buf[i] && !buf[i-1]) {
      argv[argc++] = buf+i;
    }
  }

  for(int i=0; i<argc; i++) {
    argv[i] = args_decode(argv[i]);
  }

  free(buf);

  return argc;
}

static uint8_t*
readfile(const char* path, size_t* size) {
  uint8_t* buf;
  ssize_t len;
  FILE* file;

  if(!(file=fopen(path, "rb"))) {
    return 0;
  }

  if(fseek(file, 0, SEEK_END)) {
    return 0;
  }

  if((len=ftell(file)) < 0) {
    return 0;
  }

  if(fseek(file, 0, SEEK_SET)) {
    return 0;
  }

  if(!(buf=malloc(len))) {
    return 0;
  }

  if(fread(buf, 1, len, file) != len) {
    free(buf);
    return 0;
  }

  if(fclose(file)) {
    free(buf);
    return 0;
  }

  if(size) {
    *size = len;
  }

  return buf;
}

/**
 * Launch an elf binary
 **/
static int
launch_elf(const char* cwd, const char* path, const char* args,
          const char* env, int nowait) {
  char* argv[255];
  char* envp[255];
  uint8_t* elf;
  int fds[2] = {-1, -1};
  pid_t pid;

  if(!cwd) {
    cwd = "/";
  }

  if(!args) {
    args = "";
  }

  if(!env) {
    env = "";
  }

  if(!nowait) {
    if(pipe(fds) == -1) {
      return -1;
    }
  }

  if(!(elf=readfile(path, 0))) {
    return -1;
  }

  args_split(args, argv, 255);
  args_split(env, envp, 255);
  pid = elfldr_spawn(cwd, fds[1], elf, argv, envp);

  free(elf);
  for(int i=0; argv[i]; i++) {
    free(argv[i]);
  }
  for(int i=0; envp[i]; i++) {
    free(envp[i]);
  }

  if(!nowait) {
    close(fds[1]);
    if(pid < 0) {
      close(fds[0]);
      return -1;
    }
    return fds[0];
  } else {
    if(pid < 0) {
      return -1;
    }
    return 0;
  }
}

int ftp_cmd_EXEC(ftp_env_t *env, const char* arg) {
  char pathbuf[PATH_MAX];
  char* ptr;
  size_t len;
  int fd;
  int nowait = 0;

  if(arg[0] == '@') {
    nowait = 1;
    arg++;
  }

  if(!arg[0]) {
    return ftp_active_printf(env, "501 Usage: EXEC <PATH> <ARGS>\r\n");
  }

  ptr = strstr(arg, " ");
  if(ptr) {
    len = ptr - arg;
  } else {
    len = strlen(arg);
  }

  strncpy(pathbuf, arg, len);
  pathbuf[len] = 0;

  fd = launch_elf(env->cwd, pathbuf, arg, NULL, nowait);
  if(fd > 0) {
    ftp_active_printf(env, "120-Output start\r\n");
    while(1) {
      int r = read(fd, pathbuf, PATH_MAX);
      if(r <= 0) {
        close(fd);
        break;
      }

      pathbuf[r] = 0;
      ftp_active_printf(env, "%s", pathbuf);
    }
    ftp_active_printf(env, "120 Output end\r\n");
  }

  return ftp_active_printf(env, fd >= 0 ? "200 OK\r\n" : "550 EXEC failed\r\n");
}

/*
  Local Variables:
  c-file-style: "gnu"
  End:
*/
