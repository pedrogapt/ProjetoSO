#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "parser.h"
#include "datacenter.h"
#include "constants.h"
#include "filesystem.h"

typedef struct {
	DataCenter *dc;
	char **files;
	size_t num_files;
	size_t next_index;
	pthread_mutex_t queue_lock;
} WorkerQueue;

static void process_commands_fd(DataCenter *dc, int fd) {
	while (1) {
		switch (get_next_command(fd)) {
			case CMD_DEFINE: {
				VMType vmtype;

				if (parse_define(fd, &vmtype) != 0) {
					fprintf(stderr, "Invalid define command. See H (help) for usage.\n");
					continue;
				}

				if (datacenter_define_VM(dc, &vmtype) != 0) {
					fprintf(stderr, "Failed to define VM.\n");
					continue;
				}

				printf("VM successfully defined!\n");
				fflush(stdout);

				break;
			}

			case CMD_RESERVE: {
				Reservation reservation = {0};

				size_t num_items = parse_reserve(fd, &reservation, MAX_RESERVATIONS_ITEMS);

				if (num_items == 0) {
					fprintf(stderr, "Invalid reserve command. See H (help) for usage.\n");
					continue;
				}

				if (datacenter_reserve(dc, &reservation) != 0) {
					fprintf(stderr, "Failed to reserve VMs.\n");
					continue;
				}

				printf("Reservation made successfully!\n");
				fflush(stdout);

				break;
			}

			case CMD_EXECUTE: {
				char id[MAX_STRING_SIZE];

				if (parse_execute(fd, id) != 0) {
					fprintf(stderr, "Invalid execute command. See H (help) for usage.\n");
					continue;
				}

				if (datacenter_execute(dc, id) != 0) {
					fprintf(stderr, "Failed to execute reservation.\n");
					continue;
				}

				printf("Finished reservation execution!\n");
				fflush(stdout);

				break;
			}

			case CMD_LIST:
				if (datacenter_list(dc) != 0) {
					fprintf(stderr, "Failed to list VMs.\n");
					continue;
				}

				break;

			case CMD_WAIT: {
				unsigned int delay;

				if (parse_wait(fd, &delay) != 0) {
					fprintf(stderr, "Invalid wait command. See H (help) for usage.\n");
					continue;
				}

				datacenter_wait(delay);
				break;
			}

			case CMD_INVALID:
				fprintf(stderr, "Invalid Command. See H (help) for usage.\n");
				break;

			case CMD_HELP:
				printf(
					"Spaces between arguments are allowed, but not after command end.\n"
					"Available commands:\n"
					" D <VM_TYPE_ID> <INPUT_FOLDER> <EXECUTABLE_PATH> <RAM_NEEDED> <DISK_NEEDED> <VCPU_NEEDED_COUNT>\n"
					" R <RESERVATION_ID> [<VM_TYPE_ID> <COUNT> <SERVER_ID>]+\n"
					" A <RESERVATION_ID>\n"
					" L\n"
					" E <DELAY_MS>\n"
					" H\n"
				);
				fflush(stdout);
				break;

			case CMD_EMPTY:
				break;

			case EOC:
				return;
		}
	}
}

static void *worker_thread_main(void *arg) {
	WorkerQueue *wq = (WorkerQueue *)arg;

	while (1) {
		pthread_mutex_lock(&wq->queue_lock);
		if (wq->next_index >= wq->num_files) {
			pthread_mutex_unlock(&wq->queue_lock);
			break;
		}
		size_t idx = wq->next_index++;
		pthread_mutex_unlock(&wq->queue_lock);

		int fd = open(wq->files[idx], O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "Invalid config file: %s\n", wq->files[idx]);
			continue;
		}

		process_commands_fd(wq->dc, fd);
		close(fd);
	}

	return NULL;
}

int main(int argc, char **argv) {
	DataCenter dc;
	datacenter_init(&dc);

	if (argc < 5 || argc > 8) {
		fprintf(stderr,
		        "Usage: %s <servers> <ram> <disk> <cpus> [<cores>] <input_dir> [<max_threads>]\n",
		        argv[0]);
		return 1;
	}

	size_t servers;
	size_t ram;
	size_t disk;
	double cpu;
	size_t host_cores = 1;
	size_t max_threads = 1;
	const char *input_dir = NULL;

	if (parse_size_t_arg(argv[1], &servers) != 0 ||
	    parse_size_t_arg(argv[2], &ram) != 0 ||
	    parse_size_t_arg(argv[3], &disk) != 0 ||
	    parse_double_arg(argv[4], &cpu) != 0) {
		fprintf(stderr, "Invalid command line arguments.\n");
		return 1;
	}

	if (argc == 6) {
		if (parse_size_t_arg(argv[5], &host_cores) == 0) {
			input_dir = NULL;
		} else {
			input_dir = argv[5];
		}
	} else if (argc == 7) {
		if (parse_size_t_arg(argv[5], &host_cores) == 0) {
			input_dir = argv[6];
		} else if (parse_size_t_arg(argv[6], &max_threads) == 0) {
			input_dir = argv[5];
		} else {
			fprintf(stderr, "Invalid command line arguments.\n");
			return 1;
		}
	} else if (argc == 8) {
		if (parse_size_t_arg(argv[5], &host_cores) != 0 ||
		    parse_size_t_arg(argv[7], &max_threads) != 0) {
			fprintf(stderr, "Invalid command line arguments.\n");
			return 1;
		}
		input_dir = argv[6];
	}

	if (host_cores == 0 || max_threads == 0) {
		fprintf(stderr, "Invalid command line arguments.\n");
		return 1;
	}

	datacenter_set_host_cores(&dc, host_cores);

	Resources resources = {
		.ram = ram,
		.disk = disk,
		.cpu = cpu
	};

	if (datacenter_configure(&dc, servers, &resources) != 0) {
		fprintf(stderr, "Failed to configure Data Center.\n");
		datacenter_destroy(&dc);
		return 1;
	}

	if (input_dir == NULL) {
		process_commands_fd(&dc, STDIN_FILENO);
		datacenter_destroy(&dc);
		return 0;
	}

	if (!path_exists(input_dir)) {
		fprintf(stderr, "Invalid input directory.\n");
		datacenter_destroy(&dc);
		return 1;
	}

	char **conf_files = NULL;
	size_t num_files = 0;

	if (list_conf_files(input_dir, &conf_files, &num_files) != 0) {
		fprintf(stderr, "Failed to read input directory.\n");
		datacenter_destroy(&dc);
		return 1;
	}

	if (num_files > 0) {
		size_t num_workers = (max_threads < num_files) ? max_threads : num_files;
		pthread_t *threads = (pthread_t *)malloc(num_workers * sizeof(pthread_t));
		if (threads == NULL) {
			fprintf(stderr, "Failed to allocate worker threads.\n");
			free_conf_files(conf_files, num_files);
			datacenter_destroy(&dc);
			return 1;
		}

		WorkerQueue wq = {
			.dc = &dc,
			.files = conf_files,
			.num_files = num_files,
			.next_index = 0
		};
		pthread_mutex_init(&wq.queue_lock, NULL);

		size_t created = 0;
		for (size_t i = 0; i < num_workers; i++) {
			if (pthread_create(&threads[i], NULL, worker_thread_main, &wq) != 0) {
				fprintf(stderr, "Failed to create worker thread.\n");
				break;
			}
			created++;
		}

		for (size_t i = 0; i < created; i++) {
			pthread_join(threads[i], NULL);
		}

		pthread_mutex_destroy(&wq.queue_lock);
		free(threads);
	}

	free_conf_files(conf_files, num_files);
	datacenter_destroy(&dc);
	return 0;
}