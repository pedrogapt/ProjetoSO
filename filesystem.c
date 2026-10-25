#define _XOPEN_SOURCE 700

#include "filesystem.h"
#include "constants.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int path_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISDIR(st.st_mode);
}

int file_exists(const char *path){
  struct stat st;

  if (stat(path, &st) != 0)
    return 0;

  return S_ISREG(st.st_mode);
}

int absolute_path(const char *path, char *buffer, size_t size){
  char *resolved = realpath(path, NULL);

  if (resolved == NULL)
    return 1;

  if (strlen(resolved) >= size) {
    free(resolved);
    return 1;
  }

  strcpy(buffer, resolved);

  free(resolved);
  return 0;
}

int join_path(char *dst, size_t dst_size, const char *a, const char *b) {
  if (dst == NULL || a == NULL || b == NULL)
    return 1;

  size_t len_a = strlen(a);
  size_t len_b = strlen(b);

  while (len_a > 1 && a[len_a - 1] == '/') {
    len_a--;
  }

  size_t need_slash = (len_a > 0 && a[len_a - 1] != '/') ? 1U : 0U;

  if (len_a + need_slash + len_b + 1 > dst_size)
    return 1;

  char tmp[MAX_PATH_SIZE * 2];
  if (len_a + need_slash + len_b + 1 > sizeof(tmp))
    return 1;

  memcpy(tmp, a, len_a);
  if (need_slash) {
    tmp[len_a] = '/';
  }
  memcpy(tmp + len_a + need_slash, b, len_b);
  tmp[len_a + need_slash + len_b] = '\0';

  memcpy(dst, tmp, len_a + need_slash + len_b + 1);
  return 0;
}

int ensure_directory(const char *path) {
  if (path == NULL || path[0] == '\0')
    return 1;

  char tmp[MAX_PATH_SIZE];
  size_t len = strlen(path);
  if (len >= sizeof(tmp))
    return 1;

  memcpy(tmp, path, len + 1);

  while (len > 1 && tmp[len - 1] == '/') {
    tmp[len - 1] = '\0';
    len--;
  }

  for (size_t i = 1; i < len; i++) {
    if (tmp[i] == '/') {
      tmp[i] = '\0';
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return 1;
      }
      tmp[i] = '/';
    }
  }

  if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
    return 1;
  }

  return path_exists(tmp) ? 0 : 1;
}

static int copy_regular_file(const char *src_path, const char *dst_path, mode_t mode) {
  int src_fd = open(src_path, O_RDONLY);
  if (src_fd < 0)
    return 1;

  int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, mode & (mode_t)0777);
  if (dst_fd < 0) {
    close(src_fd);
    return 1;
  }

  char buf[4096];
  int status = 0;

  while (1) {
    ssize_t bytes_read = read(src_fd, buf, sizeof(buf));
    if (bytes_read < 0) {
      if (errno == EINTR)
        continue;
      status = 1;
      break;
    }
    if (bytes_read == 0)
      break;

    ssize_t total_written = 0;
    while (total_written < bytes_read) {
      ssize_t written = write(dst_fd,
                              buf + total_written,
                              (size_t)(bytes_read - total_written));
      if (written < 0) {
        if (errno == EINTR)
          continue;
        status = 1;
        break;
      }
      total_written += written;
    }

    if (status != 0)
      break;
  }

  if (close(dst_fd) != 0)
    status = 1;
  if (close(src_fd) != 0)
    status = 1;

  return status;
}

int copy_directory_contents(const char *src_dir, const char *dst_dir) {
  if (ensure_directory(dst_dir) != 0)
    return 1;

  DIR *dir = opendir(src_dir);
  if (dir == NULL)
    return 1;

  struct dirent *entry;
  int status = 0;

  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    char src_path[MAX_PATH_SIZE];
    char dst_path[MAX_PATH_SIZE];

    if (join_path(src_path, sizeof(src_path), src_dir, entry->d_name) != 0 ||
        join_path(dst_path, sizeof(dst_path), dst_dir, entry->d_name) != 0) {
      status = 1;
      break;
    }

    struct stat st;
    if (stat(src_path, &st) != 0) {
      status = 1;
      break;
    }

    if (S_ISDIR(st.st_mode)) {
      if (copy_directory_contents(src_path, dst_path) != 0) {
        status = 1;
        break;
      }
    } else if (S_ISREG(st.st_mode)) {
      if (copy_regular_file(src_path, dst_path, st.st_mode) != 0) {
        status = 1;
        break;
      }
    }
  }

  closedir(dir);
  return status;
}

static int has_conf_extension(const char *name) {
  size_t len = strlen(name);
  if (len < 5)
    return 0;
  return strcmp(name + len - 5, ".conf") == 0;
}

static int filter_conf_entry(const struct dirent *entry) {
  return has_conf_extension(entry->d_name);
}

int list_conf_files(const char *dir_path, char ***out_files, size_t *out_count) {
  if (dir_path == NULL || out_files == NULL || out_count == NULL)
    return 1;

  struct dirent **namelist = NULL;
  int n = scandir(dir_path, &namelist, filter_conf_entry, alphasort);
  if (n < 0)
    return 1;

  char **files = (char **)malloc(((n > 0) ? (size_t)n : 1U) * sizeof(char *));
  if (files == NULL) {
    for (int i = 0; i < n; i++) {
      free(namelist[i]);
    }
    free(namelist);
    return 1;
  }

  size_t count = 0;
  int status = 0;

  for (int i = 0; i < n; i++) {
    char full_path[MAX_PATH_SIZE];
    if (status == 0 &&
        join_path(full_path, sizeof(full_path), dir_path, namelist[i]->d_name) == 0) {
      if (file_exists(full_path)) {
        size_t path_len = strlen(full_path);
        files[count] = (char *)malloc(path_len + 1);
        if (files[count] == NULL) {
          status = 1;
        } else {
          memcpy(files[count], full_path, path_len + 1);
          count++;
        }
      }
    } else if (status == 0) {
      status = 1;
    }
    free(namelist[i]);
  }
  free(namelist);

  if (status != 0) {
    free_conf_files(files, count);
    return 1;
  }

  *out_files = files;
  *out_count = count;
  return 0;
}

void free_conf_files(char **files, size_t count) {
  if (files == NULL)
    return;
  for (size_t i = 0; i < count; i++) {
    free(files[i]);
  }
  free(files);
}