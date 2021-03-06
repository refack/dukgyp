#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "duktape.h"

#ifndef DUKGYP_TEST
# include "src/dukgyp-js.h"
#else  /* DUKGYP_TEST */
# include "src/dukgyp-test-js.h"
#endif  /* DUKGYP_TEST */

static size_t kDukgypReadBlock = 65536;


typedef struct dukgyp_exec_opt_s dukgyp_exec_opt_t;

struct dukgyp_exec_opt_s {
  const char* cwd;
  int inherit_stdio;
};


static void dukgyp_syscall_throw(duk_context* ctx, const char* msg) {
  duk_generic_error(ctx, "%s, errno=%d", msg, errno);
}


static char* dukgyp_read_fd(int fd, size_t suggested_size, size_t* out_size) {
  char* res;
  size_t off;
  size_t size;
  int err;

  off = 0;
  size = suggested_size;
  if (size == 0)
    size = kDukgypReadBlock;
  res = malloc(size);
  if (res == NULL)
    return NULL;

  for (;;) {
    do
      err = read(fd, res + off, size - off);
    while (err == -1 && errno == EINTR);

    if (err < 0)
      goto fail_read;

    off += err;
    if (off == size) {
      char* tmp;

      size += kDukgypReadBlock;
      tmp = realloc(res, size);
      if (tmp == NULL)
        goto fail_read;

      res = tmp;
    }

    /* EOF */
    if (err == 0)
      break;
  }

  *out_size = off;
  return res;

fail_read:
  free(res);
  return NULL;
}


static int dukgyp_close_fd(int fd) {
  int err;

  do
    err = close(fd);
  while (err == -1 && errno == EINTR);

  return err;
}


static int dukgyp_mkdir(const char* path) {
  int err;

  do
    err = mkdir(path, 0755);
  while (err == -1 && errno == EINTR);

  return err;
}


static char* dukgyp_exec_cmd(duk_context* ctx, const char* cmd,
                             dukgyp_exec_opt_t* options,
                             size_t* out_len) {
  int err;
  pid_t pid;
  int pair[2];
  char* buf;
  int stat_loc;

  /* TODO(indutny): CLOEXEC */
  if (!options->inherit_stdio) {
    err = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
    if (err != 0)
      dukgyp_syscall_throw(ctx, "socketpair() failure");
  }

  pid = fork();
  if (pid == -1)
    dukgyp_syscall_throw(ctx, "fork() failure");

  /* Child process */
  if (pid == 0) {
    const char* argv[] = { "/bin/sh", "-c", cmd, NULL };

    /* Replace stdout */
    if (!options->inherit_stdio) {
      int fd;

      /* Black hole stdin/stderr */
      do
        fd = open("/dev/null", O_RDWR);
      while (fd == -1 && errno == EINTR);

      if (fd == -1)
        abort();

      do
        err = dup2(fd, 0);
      while (err == -1 && errno == EINTR);
      if (err == -1)
        abort();

      do
        err = dup2(fd, 2);
      while (err == -1 && errno == EINTR);
      if (err == -1)
        abort();

      /* Pipe instead of stdout */
      do
        err = dup2(pair[1], 1);
      while (err == -1 && errno == EINTR);
      if (err == -1)
        abort();

      dukgyp_close_fd(pair[1]);
      dukgyp_close_fd(fd);
    }

    if (options->cwd != NULL) {
      do
        err = chdir(options->cwd);
      while (err == -1 && errno == EINTR);
      if (err == -1)
        abort();
    }

    if (err == -1)
      abort();

    err = execvp(argv[0], (char**) argv);
    if (err != 0)
      abort();

    /* Not reachable */
    abort();
    return NULL;
  }

  buf = NULL;
  if (options->inherit_stdio)
    goto wait_pid;

  dukgyp_close_fd(pair[1]);
  buf = dukgyp_read_fd(pair[0], 0, out_len);
  dukgyp_close_fd(pair[0]);

wait_pid:
  do
    err = waitpid(pid, &stat_loc, 0);
  while (err == -1 && errno == EINTR);

  if (err == -1) {
    free(buf);
    dukgyp_syscall_throw(ctx, "waitpid() failure");
  }

  if (WEXITSTATUS(stat_loc) != 0) {
    free(buf);
    duk_generic_error(ctx, "execSync(): non-zero exit code");
  }

  return buf;
}


static void dukgyp_fatal_handler(void* udata, const char* msg) {
  fprintf(stderr, "Fatal dukgyp error: %s\n", msg);
  fflush(stderr);
  exit(-1);
}


static duk_ret_t dukgyp_native_log(duk_context* ctx) {
  fprintf(stdout, "%s", duk_to_string(ctx, 0));
  return 0;
}


static duk_ret_t dukgyp_native_error(duk_context* ctx) {
  fprintf(stderr, "%s", duk_to_string(ctx, 0));
  return 0;
}


static duk_ret_t dukgyp_native_exit(duk_context* ctx) {
  exit(duk_to_int(ctx, 0));
  return 0;
}


static duk_ret_t dukgyp_native_cwd(duk_context* ctx) {
  char* cwd;
  /* TODO(indutny): PATH_MAX */
  char buf[16384];

  cwd = getcwd(buf, sizeof(buf));
  if (cwd == NULL)
    dukgyp_syscall_throw(ctx, "cwd() failure");
  duk_push_string(ctx, cwd);
  return 1;
}


static void dukgyp_bindings_general(duk_context* ctx) {
  char* platform;
  char* arch;

  duk_push_c_function(ctx, dukgyp_native_log, 1);
  duk_put_prop_string(ctx, -2, "log");

  duk_push_c_function(ctx, dukgyp_native_error, 1);
  duk_put_prop_string(ctx, -2, "error");

  duk_push_c_function(ctx, dukgyp_native_exit, 1);
  duk_put_prop_string(ctx, -2, "exit");

  duk_push_c_function(ctx, dukgyp_native_cwd, 0);
  duk_put_prop_string(ctx, -2, "cwd");

  platform = getenv("DUKGYP_PLATFORM");
  if (platform == NULL)
#ifdef DUKGYP_PLATFORM
    platform = DUKGYP_PLATFORM;
#else
    duk_generic_error(ctx, "Please provide DUKGYP_PLATFORM env variable");
#endif  /* DUKGYP_PLATFORM */
  duk_push_string(ctx, platform);
  duk_put_prop_string(ctx, -2, "platform");

  arch = getenv("DUKGYP_ARCH");
  if (arch == NULL)
#ifdef DUKGYP_ARCH
    arch = DUKGYP_ARCH;
#else
  duk_generic_error(ctx, "Please provide DUKGYP_ARCH env variable");
#endif  /* DUKGYP_PLATFORM */
  duk_push_string(ctx, arch);
  duk_put_prop_string(ctx, -2, "arch");
}


/* NOTE: just a stub, we are not going to support it in v1 */
static duk_ret_t dukgyp_native_fs_readdir(duk_context* ctx) {
  duk_push_array(ctx);
  return 1;
}


static duk_ret_t dukgyp_native_fs_realpath(duk_context* ctx) {
  char* p;

  p = realpath(duk_to_string(ctx, 0), NULL);
  if (p == NULL)
    dukgyp_syscall_throw(ctx, "realpath() failure");

  duk_push_string(ctx, p);
  free(p);
  return 1;
}


static duk_ret_t dukgyp_native_fs_exists(duk_context* ctx) {
  struct stat st;
  int err;
  const char* arg;

  arg = duk_to_string(ctx, 0);

  do
    err = lstat(arg, &st);
  while (err == -1 && errno == EINTR);

  if (err == -1 && errno != ENOENT)
    dukgyp_syscall_throw(ctx, "fs.exists failure");

  duk_push_boolean(ctx, err == 0);
  return 1;
}


static duk_ret_t dukgyp_native_fs_read_file(duk_context* ctx) {
  struct stat st;
  int fd;
  int err;
  const char* arg;
  char* buf;
  char* storage;
  size_t len;

  arg = duk_to_string(ctx, 0);

  do
    fd = open(arg, O_RDONLY);
  while (fd == -1 && errno == EINTR);

  if (fd == -1 && errno == ENOENT)
    duk_generic_error(ctx, "fs.readFile() error: no file");
  else if (fd == -1)
    dukgyp_syscall_throw(ctx, "fs.readFile() error: other failure");

  do
    err = fstat(fd, &st);
  while (err == -1 && errno == EINTR);

  if (err != 0)
    dukgyp_syscall_throw(ctx, "fs.readFile() error: fstat error");

  buf = dukgyp_read_fd(fd, st.st_size + 1, &len);

  dukgyp_close_fd(fd);

  if (buf == NULL)
    duk_generic_error(ctx, "fs.readFile() error: can't read");

  /* TODO(indutny): avoid copying */
  storage = duk_push_fixed_buffer(ctx, len);
  memcpy(storage, buf, len);
  free(buf);

  duk_push_buffer_object(ctx, -1, 0, len, DUK_BUFOBJ_NODEJS_BUFFER);
  duk_remove(ctx, -2);

  return 1;
}


static duk_ret_t dukgyp_native_fs_write_file(duk_context* ctx) {
  const char* arg;
  const char* content;
  size_t len;
  int fd;

  arg = duk_to_string(ctx, 0);
  content = duk_to_string(ctx, 1);

  do
    fd = open(arg, O_WRONLY | O_CREAT | O_TRUNC, 0755);
  while (fd == -1 && errno == EINTR);

  if (fd == -1)
    dukgyp_syscall_throw(ctx, "open() failed");

  len = strlen(content);
  while (len != 0) {
    int err;

    do
      err = write(fd, content, len);
    while (err == -1 && errno == EINTR);

    if (err == -1)
      dukgyp_syscall_throw(ctx, "write() failed");

    content += err;
    len -= err;
  }

  dukgyp_close_fd(fd);

  return 0;
}


static duk_ret_t dukgyp_native_fs_mkdirp(duk_context* ctx) {
  char* arg;
  char* p;
  int err;

  arg = strdup(duk_to_string(ctx, 0));
  if (arg == NULL)
    duk_generic_error(ctx, "strdup() no memory");

  for (p = arg; *p != '\0'; p++) {
    if (*p != '/')
      continue;

    if (p == arg)
      continue;

    *p = '\0';
    err = dukgyp_mkdir(arg);
    if (err != 0 && errno != EEXIST)
      dukgyp_syscall_throw(ctx, "mkdir() failure");
    *p = '/';
  }

  err = dukgyp_mkdir(arg);
  free(arg);
  if (err != 0 && errno != EEXIST)
    dukgyp_syscall_throw(ctx, "mkdir() failure");

  return 0;
}


static void dukgyp_bindings_fs(duk_context* ctx) {
  duk_push_object(ctx);

  duk_push_c_function(ctx, dukgyp_native_fs_readdir, 0);
  duk_put_prop_string(ctx, -2, "readdirSync");

  duk_push_c_function(ctx, dukgyp_native_fs_realpath, 1);
  duk_put_prop_string(ctx, -2, "realpathSync");

  duk_push_c_function(ctx, dukgyp_native_fs_exists, 1);
  duk_put_prop_string(ctx, -2, "existsSync");

  duk_push_c_function(ctx, dukgyp_native_fs_read_file, 1);
  duk_put_prop_string(ctx, -2, "readFileSync");

  duk_push_c_function(ctx, dukgyp_native_fs_write_file, 2);
  duk_put_prop_string(ctx, -2, "writeFileSync");

  duk_push_c_function(ctx, dukgyp_native_fs_mkdirp, 1);
  duk_put_prop_string(ctx, -2, "mkdirpSync");

  duk_put_prop_string(ctx, -2, "fs");
}


static duk_ret_t dukgyp_native_cp_exec(duk_context* ctx) {
  const char* cmd;
  dukgyp_exec_opt_t opts;
  char* out;
  size_t len;
  char* storage;

  cmd = duk_to_string(ctx, 0);
  memset(&opts, 0, sizeof(opts));
  if (duk_is_object(ctx, 1)) {
    const char* stdio;

    duk_get_prop_string(ctx, 1, "cwd");
    if (duk_is_string(ctx, -1))
      opts.cwd = duk_to_string(ctx, -1);
    duk_pop(ctx);

    duk_get_prop_string(ctx, 1, "stdio");
    stdio = duk_to_string(ctx, -1);
    duk_pop(ctx);

    opts.inherit_stdio = strcmp(stdio, "inherit") == 0;
  }

  out = dukgyp_exec_cmd(ctx, cmd, &opts, &len);
  if (out == NULL) {
    if (!opts.inherit_stdio)
      duk_generic_error(ctx, "dukgyp_exec_cmd() failure");

    duk_push_null(ctx);
    return 1;
  }

  storage = duk_push_fixed_buffer(ctx, len);
  memcpy(storage, out, len);
  free(out);

  duk_push_buffer_object(ctx, -1, 0, len, DUK_BUFOBJ_NODEJS_BUFFER);
  duk_remove(ctx, -2);

  return 1;
}


static void dukgyp_bindings_child_process(duk_context* ctx) {
  duk_push_object(ctx);

  duk_push_c_function(ctx, dukgyp_native_cp_exec, 2);
  duk_put_prop_string(ctx, -2, "execSync");

  duk_put_prop_string(ctx, -2, "childProcess");
}


static duk_ret_t dukgyp_native_getenv(duk_context* ctx) {
  const char* arg;
  char* env;

  arg = duk_to_string(ctx, 0);

  env = getenv(arg);
  if (env == NULL)
    duk_push_undefined(ctx);
  else
    duk_push_string(ctx, env);

  return 1;
}


static void dukgyp_bindings_env(duk_context* ctx) {
  duk_push_c_function(ctx, dukgyp_native_getenv, 1);
  duk_put_prop_string(ctx, -2, "getenv");
}


static void dukgyp_bindings_argv(duk_context* ctx, int argc, char** argv) {
  int i;

  duk_push_array(ctx);

  /* argv[0] is usually `node` */
  duk_push_string(ctx, "duktape");
  duk_put_prop_index(ctx, -2, 0);

  for (i = 0; i < argc; i++) {
    duk_push_string(ctx, argv[i]);
    duk_put_prop_index(ctx, -2, i + 1);
  }

  duk_put_prop_string(ctx, -2, "argv");
}


int main(int argc, char** argv) {
  duk_context* ctx;

  signal(SIGPIPE, SIG_IGN);

  ctx = duk_create_heap(NULL, NULL, NULL, NULL, dukgyp_fatal_handler);
  if (ctx == NULL)
    abort();

  duk_push_global_object(ctx);

  /* `bindings` */
  duk_push_object(ctx);
  dukgyp_bindings_general(ctx);
  dukgyp_bindings_fs(ctx);
  dukgyp_bindings_child_process(ctx);
  dukgyp_bindings_env(ctx);
  dukgyp_bindings_argv(ctx, argc, argv);

  duk_put_prop_string(ctx, -2, "bindings");

  duk_pop(ctx);

  duk_eval_string_noresult(ctx, dukgyp_js);

  duk_destroy_heap(ctx);
  return 0;
}
