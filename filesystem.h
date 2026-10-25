#ifndef FILESYSTEM__H
#define FILESYSTEM__H

#include <stddef.h>

/**
 * Checks whether a path exists and is a directory.
 *
 * @param path Directory path.
 *
 * @return 1 if it exists and is a directory, 0 otherwise.
 */
int path_exists(const char *path);

/**
 * Checks whether a path exists and is a regular file.
 *
 * @param path File path.
 *
 * @return 1 if it exists and is a regular file, 0 otherwise.
 */
int file_exists(const char *path);

/**
 * Converts the given path into an absolute path, resolving symbolic links,
 * relative components ('.' and '..'), and redundant separators. The resolved
 * path is copied into the provided buffer.
 *
 * @param path Path to resolve.
 * @param buffer Destination buffer where the absolute path will be stored.
 * @param size Size of the destination buffer, in bytes.
 *
 * @return 0 if the path was successfully resolved and copied to the buffer
 * @return 1 if the path could not be resolved or the buffer is too small.
 */
int absolute_path(const char *path, char *buffer, size_t size);

/**
 * Joins two path components with a '/' separator into dst.
 *
 * @param dst Destination buffer.
 * @param dst_size Size of destination buffer.
 * @param a First path component.
 * @param b Second path component.
 *
 * @return 0 on success, 1 if buffer is too small.
 */
int join_path(char *dst, size_t dst_size, const char *a, const char *b);

/**
 * Ensures a directory and all its parent directories exist (mkdir -p).
 *
 * @param path Directory path to create.
 *
 * @return 0 on success, 1 on failure.
 */
int ensure_directory(const char *path);

/**
 * Recursively copies all files and subdirectories from src_dir into dst_dir
 * using POSIX file descriptor operations.
 *
 * @param src_dir Source directory path.
 * @param dst_dir Destination directory path.
 *
 * @return 0 on success, 1 on failure.
 */
int copy_directory_contents(const char *src_dir, const char *dst_dir);

/**
 * Lists all regular files with the ".conf" extension in dir_path, sorted
 * alphabetically by filename.
 *
 * @param dir_path Directory to scan.
 * @param out_files Output pointer to array of allocated file path strings.
 * @param out_count Output pointer to number of files found.
 *
 * @return 0 on success, 1 on failure.
 */
int list_conf_files(const char *dir_path, char ***out_files, size_t *out_count);

/**
 * Frees the array of file paths returned by list_conf_files().
 *
 * @param files Array of file path strings.
 * @param count Number of elements in the array.
 */
void free_conf_files(char **files, size_t count);

#endif // FILESYSTEM__H