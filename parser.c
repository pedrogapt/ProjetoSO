#include "parser.h"

#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stddef.h>

#include "constants.h"
#include "datacenter.h"

/**
 * Skip spaces until finding a non-space character.
 *
 * @param fd File descriptor to read from.
 * @param first Pointer where the firsy non-space character is stored.
 *
 * @return 0 on success.
 * @return -1 on failure.
 */
static int skip_spaces(int fd, char *first){
  ssize_t bytes_read = 0;
  char ch;

  while (1) {
    bytes_read = read(fd, &ch, 1);

    if (bytes_read <= 0)
      return -1;

    if (ch != ' '){
      *first = ch;
      break;
    }
  }
  return 0;
}

/**
 * Reads a string from a file descriptor.
 *
 * Stops when a space or newline is found.
 *
 * @param fd File descriptor to read from.
 * @param buffer Buffer to read into.´
 * @param max Max bytes to read.
 * @param delimiter Pointer where the terminating character is stored.
 *
 * @return 0 on success.
 * @return -1 on error or EOF.
 */
static int read_string(int fd, char *buffer, size_t max, char *delimiter){
  char ch;
  size_t i = 0;

  if (skip_spaces(fd, &ch) != 0) {
    return -1;
  }

  while (1) {
    if (ch == ' ' || ch == '\n') {
      *delimiter = ch;
      break;
    }

    if (i >= max - 1) {
      return -1;
    }

    buffer[i++] = ch;

    if (read(fd, &ch, 1) != 1) {
      *delimiter = '\0';
      break;
    }
  }

  buffer[i] = '\0';

  return 0;
}

/**
 * Reads a size_t value from a file descriptor.
 *
 * Reads characters until a non-digit or EOF is found and converts them to a
 * size_t.
 *
 * @param fd File descriptor to read from.
 * @param value Pointer where the value is stored.
 * @param delimiter Pointer where the terminating character is stored.
 *
 * @return 0 on success.
 * @return 1 if the value exceeds SIZE_MAX.
 * @return -1 on error.
 */
static int read_size_t(int fd, size_t *value, char *delimiter) {
  char buf[32];
  size_t i = 0;
  char ch;

  if (skip_spaces(fd, &ch) != 0) {
    return -1;
  }

  while (1) {
    if (ch < '0' || ch > '9') {
      *delimiter = ch;
      break;
    }

    if (i >= sizeof(buf) - 1) {
      return -1;
    }

    buf[i++] = ch;

    if (read(fd, &ch, 1) != 1) {
      *delimiter = '\0';
      break;
    }
  }

  if (i == 0) {
    return -1;
  }

  buf[i] = '\0';

  errno = 0;
  char *end;
  unsigned long long tmp = strtoull(buf, &end, 10);

  if (errno != 0 || *end != '\0' || tmp > SIZE_MAX) {
    return -1;
  }

  *value = (size_t)tmp;

  return 0;
}

/**
 * Reads a floating-point value from a file descriptor.
 *
 * Skips leading spaces, reads the next token until a space, newline or EOF,
 * and converts it to a double.
 *
 * @param fd File descriptor to read from.
 * @param value Pointer where the parsed value will be stored.
 * @param delimiter Pointer where the terminating character is stored.
 *
 * @return 0 on success.
 * @return -1 if no valid floating-point value could be read.
 */
static int read_double(int fd, double *value, char *delimiter){
  char buffer[64];

  if (read_string(fd, buffer, sizeof(buffer), delimiter) != 0)
    return -1;

  errno = 0;
  char *endptr;

  *value = strtod(buffer, &endptr);

  if (errno != 0 || *endptr != '\0')
    return -1;

  return 0;
}

/**
 * Parses a resource specification.
 *
 * Reads three size_t values representing RAM, disk and cores.
 *
 * @param fd File descriptor to read from.
 * @param resources Pointer where the resources are stored.
 * @param delimiter Pointer where the terminating character is stored.
 *
 * @return 0 on success.
 * @return -1 on error.
 */
static int parse_resources(int fd, Resources *resources, char *delimiter) {
  if (read_size_t(fd, &resources->ram, delimiter) != 0 || *delimiter != ' ') {
    return -1;
  }

  if (read_size_t(fd, &resources->disk, delimiter) != 0 || *delimiter != ' ') {
    return -1;
  }

  if (read_double(fd, &resources->cpu, delimiter) != 0 || resources->cpu <= 0.0) {
    return -1;
  }

  return 0;
}

/**
 * Discards the remaining characters of the current line.
 *
 * Reads and ignores characters until a newline is found.
 *
 * @param fd File descriptor to read from.
 * @param last_char Last character read from fd.
 */
static void cleanup(int fd, char last_char) {
  char ch;

  // Parser consumes delimiters so we do this safe check.
  if(last_char == '\n' || last_char == '\0'){
    return;
  }

  while (read(fd, &ch, 1) == 1 && ch != '\n')
    ;
}

/**
 * Converts a character into a command.
 *
 * @param ch Character representing a command.
 *
 * @return The corresponding command.
 * @return CMD_INVALID if the character is not a command.
 */
static Command char_to_command(char ch){
  switch (ch) {
    case 'D':
      return CMD_DEFINE;
    case 'R':
      return CMD_RESERVE;
    case 'A':
      return CMD_EXECUTE;
    case 'E':
      return CMD_WAIT;
    case 'L':
      return CMD_LIST;
    case 'H':
      return CMD_HELP;
    default:
      return CMD_INVALID;
  }
}

static int command_has_arguments(Command command){
  return command != CMD_LIST && command != CMD_HELP;
}

/**
 * Checks if the delimiter after a command is valid.
 *
 * @param command Command being checked.
 * @param delimiter Character after the command.
 *
 * @return 0 if valid.
 * @return -1 otherwise.
 */
static int validate_command_delimiter(Command command, char delimiter){
  if (command_has_arguments(command)) {
    return delimiter == ' ' ? 0 : -1;
  }

  return (delimiter == '\n' || delimiter == '\0') ? 0 : -1;
}

int parse_size_t_arg(const char *str, size_t *out) {
  char *end;
  errno = 0;

  if (str == NULL || str[0] == '\0' || str[0] == '-')
    return 1;

  unsigned long long value = strtoull(str, &end, 10);

  if (errno != 0 || *end != '\0')
    return 1;

  *out = (size_t)value;
  return 0;
}

int parse_double_arg(const char *str, double *out) {
  char *end;
  errno = 0;

  double value = strtod(str, &end);

  if (errno != 0 || *end != '\0')
    return 1;

  *out = value;
  return 0;
}

Command get_next_command(int fd){
  char ch;
  char delimiter;

  if (read(fd, &ch, 1) != 1) {
    return EOC;
  }

  if (ch == '#') {
    cleanup(fd, ch);
    return CMD_EMPTY;
  }

  if (ch == '\n') {
    return CMD_EMPTY;
  }

  Command command = char_to_command(ch);

  if (command == CMD_INVALID) {
    cleanup(fd, ch);
    return CMD_INVALID;
  }

  ssize_t bytes = read(fd, &delimiter, 1);

  if (bytes == 0) {
    delimiter = '\0';
  } else if (bytes < 0) {
    return CMD_INVALID;
  }

  if (validate_command_delimiter(command, delimiter) != 0) {
    cleanup(fd, delimiter);
    return CMD_INVALID;
  }

  return command;
}

int parse_define(int fd, VMType *vm){
  char delimiter;
  VMType tmp;

  // Parse VM id.
  if(read_string(fd, tmp.id, MAX_STRING_SIZE, &delimiter) != 0 || delimiter != ' '){
    cleanup(fd, delimiter);
    return 1;
  }

  // Parse input folder.
  if(read_string(fd, tmp.input_folder, MAX_PATH_SIZE, &delimiter) != 0 || delimiter != ' '){
    cleanup(fd, delimiter);
    return 1;
  }

  // Parse executable file.
  if(read_string(fd, tmp.exec_path, MAX_PATH_SIZE, &delimiter) != 0 || delimiter != ' '){
    cleanup(fd, delimiter);
    return 1;
  }

  // Parse VM resourcws.
  if(parse_resources(fd, &tmp.required, &delimiter) != 0 || (delimiter != '\n' && delimiter != '\0')) {
    cleanup(fd, delimiter);
    return 1;
  }

  *vm = tmp;
  return 0;
}

size_t parse_reserve(int fd, Reservation *reservation, size_t max_pairs){
  char delimiter;
  Reservation tmp = {0};

  // Parse reservation id
  if(read_string(fd, tmp.id, MAX_STRING_SIZE, &delimiter) != 0 || delimiter != ' '){
    cleanup(fd, delimiter);
    return 0;
  }

  // Parse VM type and Number of VMs being reserved pairs.
  tmp.items_count = 0;
  while(tmp.items_count < max_pairs){
    if(read_string(fd, tmp.items[tmp.items_count].vm_type_id, MAX_STRING_SIZE, &delimiter) != 0 || delimiter != ' '){
      cleanup(fd, delimiter);
      return 0;
    }

    if(read_size_t(fd, &tmp.items[tmp.items_count].amount, &delimiter) != 0){
      cleanup(fd, delimiter);
      return 0;
    }

    if(read_size_t(fd, &tmp.items[tmp.items_count].server_id, &delimiter) != 0){
      cleanup(fd, delimiter);
      return 0;
    }

    tmp.items_count++;

    // Check if pairs ended.
    if(delimiter == '\n' || delimiter == '\0'){
      break;
    }
  }

  if (tmp.items_count == max_pairs && delimiter != '\n' && delimiter != '\0') {
    cleanup(fd, delimiter);
    return 0;
  }

  *reservation = tmp;
  return reservation->items_count;
}

int parse_execute(int fd, char *id){
  char delimiter;
  char buf[MAX_STRING_SIZE];

  if(read_string(fd, buf, MAX_STRING_SIZE, &delimiter) != 0 || (delimiter != '\n' && delimiter != '\0')){
    cleanup(fd, delimiter);
    return 1;
  }

  strncpy(id, buf, MAX_STRING_SIZE);
  return 0;
}

int parse_wait(int fd, unsigned int *delay){
  char delimiter;
  size_t tmp;

  if(read_size_t(fd, &tmp, &delimiter) != 0 || (delimiter != '\n' && delimiter != '\0')){
    cleanup(fd, delimiter);
    return 1;
  }

  *delay = (unsigned int)tmp;
  return 0;
}
