# cloudIST

This repository contains implementation for the cloudIST operating systems project. 
The program simulates a data center that manages servers, VM types, reservations, and VM execution commands.

## Project Structure

### Core source files

- `main.c`: Program entry point, command loop, and command-line configuration.
- `parser.c` / `parser.h`: Parsing of command-line values and commands read from
  standard input.
- `datacenter.c` / `datacenter.h`: Data center configuration, VM type
  registration, reservations, execution, listing, and waits.
- `datacenter_utils.c` / `datacenter_utils.h`: VM type and reservation lookup,
  reservation validation and allocation, cleanup, and VM lifecycle helpers.
- `resources.c` / `resources.h`: Resource operations for CPU, RAM, and disk.
- `filesystem.c` / `filesystem.h`: Filesystem checks and absolute-path
  resolution.
- `constants.h`: Limits used by the data center, reservations, VM types, and
  identifiers.
