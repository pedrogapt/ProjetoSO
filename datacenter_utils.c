#include "datacenter_utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>

VMType *VMType_exists(DataCenter *dc, const char* type_id){
	for(size_t i = 0; i < dc->num_vm_types; i++){
		if(strncmp(type_id, dc->vm_types[i].id, MAX_STRING_SIZE) == 0){
			return &dc->vm_types[i];
		}
	}
	return NULL;
}

int reservation_exists(DataCenter *dc, const char* reservation_id){
	for(size_t i = 0; i < dc->num_reservations; i++){
		if(strncmp(reservation_id, dc->reservations[i].id, MAX_STRING_SIZE) == 0){
			return 1;
		}
	}
	return 0;
}

int reservation_validate(DataCenter *dc, Reservation *reservation) {
	if(reservation_exists(dc, reservation->id)){
		fprintf(stderr, "Reservation with id %s already exists.\n", reservation->id);
		return 1;
	}

	Resources available[dc->num_servers];
	size_t hosted_count[dc->num_servers];
	size_t vms_count = 0;

	/* Copy current server resources and hosted_vms so we can simulate the allocation. */
	for (size_t i = 0; i < dc->num_servers; i++) {
		available[i] = dc->servers[i].available;
		hosted_count[i] = dc->servers[i].num_hosted_vms;
	}

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];

		if (item->amount == 0) {
			fprintf(stderr, "Reservation item must request at least one VM.\n");
			return 1;
		}

		VMType *vm_type = VMType_exists(dc, item->vm_type_id);

		if(vm_type == NULL){
			fprintf(stderr, "VM of type %s does not exist\n", item->vm_type_id);
			return 1;
		}

		if (item->server_id == 0 || item->server_id > dc->num_servers) {
			fprintf(stderr, "Invalid server in reservation.\n");
			return 1;
		}

		for (size_t j = 0; j < item->amount; j++) {
			if(vms_count >= MAX_RESERVATION_VMS){
				fprintf(stderr, "Reached max of VMS for a reservation.\n");
				return 1;
			}
			
			if (hosted_count[item->server_id - 1] >= MAX_HOSTED_VMS) {
        fprintf(stderr,
                "Server \"%zu\" reached the maximum number of hosted VMs.\n",
                item->server_id);
        return 1;
    	}

			if (!resources_can_fit(available[item->server_id-1], vm_type->required)) {
				fprintf(stderr, "Server \"%zu\" does not have enough resources.\n", item->server_id);
				return 1;
			}

			resources_sub(&available[item->server_id-1], vm_type->required);
			hosted_count[item->server_id-1]++;
			vms_count++; // One VM "reserved"
		}

		// Attach vm_type to item for later use.
		item->vm_type = vm_type;
	}

	return 0;
}

/**
 * Rolls back the changes made by a reservation operation.
 * 
 * @param reservation Pointer to the reservation being rolled back.
 * @param initial_vms Number of VMs that existed before the operation.
 */
static void reservation_rollback(Reservation *reservation, size_t initial_vms){
	while (reservation->num_vms > initial_vms) {
		VM *vm = reservation->vms[reservation->num_vms - 1];

		Server *server = vm->server;

		// Restore resources
		resources_add(&server->available, vm->type->required);

		// Remove VM from server hosted list
		server->num_hosted_vms--;

		server->hosted_vms[server->num_hosted_vms] = NULL;

		// Remove VM from reservation
		reservation->num_vms--;

		reservation->vms[reservation->num_vms] = NULL;

		free(vm);
	}
}

int reservation_commit(DataCenter *dc, Reservation *reservation){
	// Keep track of how many vms existed before.
	size_t initial_vms = reservation->num_vms;

	char res_dir[MAX_PATH_SIZE];
	if (join_path(res_dir, sizeof(res_dir), CLOUDIST_TMP_DIR, reservation->id) != 0) {
		return 1;
	}

	for (size_t i = 0; i < reservation->items_count; i++) {
		ReservationItem *item = &reservation->items[i];
		Server *server = &dc->servers[item->server_id - 1];

		for (size_t j = 0; j < item->amount; j++) {
			VM *new_vm = calloc(1, sizeof(VM));
			if (new_vm == NULL) {
				fprintf(stderr, "Failed to allocate VM.\n");
				reservation_rollback(reservation, initial_vms);
				return 1;
			}

			snprintf(new_vm->id,
				MAX_VM_ID_STRING,
				"%s_%zu",
				reservation->id,
				reservation->num_vms + 1
			);

			if (join_path(new_vm->dir_path, sizeof(new_vm->dir_path), res_dir, new_vm->id) != 0) {
				free(new_vm);
				reservation_rollback(reservation, initial_vms);
				return 1;
			}

			new_vm->type = item->vm_type;
			new_vm->server = server;
			new_vm->state = VM_STATE_RESERVED;

			double denom = server->total.cpu * (double)dc->num_servers;
			double raw_pct = (denom > 0.0)
				? (100.0 * item->vm_type->required.cpu * (double)dc->host_cores) / denom
				: 100.0;
			int max_limit = (int)(100 * dc->host_cores);
			int cpu_limit = (int)(raw_pct + 0.5);
			if (cpu_limit < 1) {
				cpu_limit = 1;
			} else if (cpu_limit > max_limit) {
				cpu_limit = max_limit;
			}
			new_vm->cpu_limit = cpu_limit;

			resources_sub(&server->available, item->vm_type->required);

			server->hosted_vms[server->num_hosted_vms++] = new_vm;
			reservation->vms[reservation->num_vms++] = new_vm;
		}
	}

	reservation->state = RES_STATE_PENDING;

	return 0;
}

Reservation *find_pending_reservation(DataCenter *dc, const char *reservation_id) {
	Reservation *res = NULL;

	for (size_t i = 0; i < dc->num_reservations; i++) {
		if (strcmp(dc->reservations[i].id, reservation_id) == 0) {
			res = &dc->reservations[i];
			break;
		}
	}

	if (!res) {
		fprintf(stderr, "Reservation ID %s not found.\n", reservation_id);
		return NULL;
	}

	if (res->state != RES_STATE_PENDING) {
		fprintf(stderr, "Reservation ID %s is not in a pending state.\n", reservation_id);
		return NULL;
	}

	return res;
}

void reservation_destroy(DataCenter *dc, Reservation *reservation) {
	// Free every VM belonging to the reservation.
	for (size_t i = 0; i < reservation->num_vms; i++) {
		VM *vm = reservation->vms[i];
		Server *server = vm->server;

		// Return the VM resources back to the hosting server.
		resources_add(&server->available, vm->type->required);

		// Remove the VM from the server's hosted VM list.
		for (size_t j = 0; j < server->num_hosted_vms; j++) {
			if (server->hosted_vms[j] == vm) {
				// Shift the remaining VMs one position to the left.
				memmove(&server->hosted_vms[j],
								&server->hosted_vms[j + 1],
								(server->num_hosted_vms - j - 1) * sizeof(VM *));

				server->num_hosted_vms--;
				break;
			}
		}

		free(vm);
		reservation->vms[i] = NULL;
	}

	reservation->num_vms = 0;

	// Find the reservation inside the Data Center reservation list.
	size_t idx;
	for (idx = 0; idx < dc->num_reservations; idx++) {
			if (&dc->reservations[idx] == reservation)
				break;
	}

	// Reservation not found (should never happen).
	if (idx == dc->num_reservations)
		return;

	// Remove the reservation by shifting the remaining ones.
	memmove(&dc->reservations[idx],
					&dc->reservations[idx + 1],
					(dc->num_reservations - idx - 1) * sizeof(Reservation));

	dc->num_reservations--;
}

static void format_uint_str(unsigned int val, char *buf) {
	char rev[16];
	size_t i = 0;
	if (val == 0) {
		buf[0] = '0';
		buf[1] = '\0';
		return;
	}
	while (val > 0 && i < sizeof(rev)) {
		rev[i++] = (char)('0' + (val % 10U));
		val /= 10U;
	}
	for (size_t j = 0; j < i; j++) {
		buf[j] = rev[i - 1 - j];
	}
	buf[i] = '\0';
}

void spawn_vm_child(VM *vm) {
	struct rlimit lim;

	rlim_t ram_bytes = (rlim_t)vm->type->required.ram << 30;
	lim.rlim_cur = ram_bytes;
	lim.rlim_max = ram_bytes;
	if (setrlimit(RLIMIT_AS, &lim) != 0) {
		_exit(1);
	}

	rlim_t disk_bytes = (rlim_t)vm->type->required.disk << 30;
	lim.rlim_cur = disk_bytes;
	lim.rlim_max = disk_bytes;
	if (setrlimit(RLIMIT_FSIZE, &lim) != 0) {
		_exit(1);
	}

	if (vm->dir_path[0] != '\0') {
		if (chdir(vm->dir_path) != 0) {
			_exit(1);
		}
	}

	unsigned int limit_val = (vm->cpu_limit > 0) ? (unsigned int)vm->cpu_limit : 1U;
	char limit_str[16];
	format_uint_str(limit_val, limit_str);

	execlp("cpulimit",
	       "cpulimit",
	       "-q",
	       "-f",
	       "-l",
	       limit_str,
	       "--",
	       vm->type->exec_path,
	       (char *)NULL);

	if (errno == ENOENT) {
		execl(vm->type->exec_path, vm->type->exec_path, (char *)NULL);
	}

	_exit(1);
}

int spawn_all_vms(Reservation *res) {
	for (size_t i = 0; i < res->num_vms; i++) {
		VM *vm = res->vms[i];

		pid_t pid = fork();
		if (pid < 0) {
			return 1;
		}
		if (pid == 0) {
			spawn_vm_child(vm);
		}

		vm->pid = pid;
		vm->state = VM_STATE_RUNNING;
	}

	return 0;
}

void wait_for_all_vms(Reservation *res) {
	for (size_t i = 0; i < res->num_vms; i++) {
		if (res->vms[i]->state == VM_STATE_RUNNING && res->vms[i]->pid > 0) {
			int status;
			while (waitpid(res->vms[i]->pid, &status, 0) < 0 && errno == EINTR)
				;
		}
		res->vms[i]->state = VM_STATE_TERMINATED;
	}
}

void reap_vm_by_pid(DataCenter *dc, pid_t pid) {
	if (pid <= 0 || !dc->configured)
		return;

	int found = 0;

	for (size_t i = 0; i < dc->num_servers; i++) {
		pthread_mutex_lock(&dc->servers[i].lock);
	}
	pthread_mutex_lock(&dc->reservations_lock);

	for (size_t r = 0; r < dc->num_reservations && !found; r++) {
		Reservation *res = &dc->reservations[r];
		for (size_t v = 0; v < res->num_vms; v++) {
			VM *vm = res->vms[v];
			if (vm != NULL && vm->state == VM_STATE_RUNNING && vm->pid == pid) {
				Server *server = vm->server;

				resources_add(&server->available, vm->type->required);

				for (size_t j = 0; j < server->num_hosted_vms; j++) {
					if (server->hosted_vms[j] == vm) {
						memmove(&server->hosted_vms[j],
						        &server->hosted_vms[j + 1],
						        (server->num_hosted_vms - j - 1) * sizeof(VM *));
						server->num_hosted_vms--;
						server->hosted_vms[server->num_hosted_vms] = NULL;
						break;
					}
				}

				memmove(&res->vms[v],
				        &res->vms[v + 1],
				        (res->num_vms - v - 1) * sizeof(VM *));
				res->num_vms--;
				res->vms[res->num_vms] = NULL;

				free(vm);

				if (res->num_vms == 0) {
					res->state = RES_STATE_FINISHED;
					memmove(&dc->reservations[r],
					        &dc->reservations[r + 1],
					        (dc->num_reservations - r - 1) * sizeof(Reservation));
					dc->num_reservations--;
				}

				found = 1;
				break;
			}
		}
	}

	pthread_mutex_unlock(&dc->reservations_lock);
	for (size_t i = dc->num_servers; i > 0; i--) {
		pthread_mutex_unlock(&dc->servers[i - 1].lock);
	}

	if (found) {
		pthread_mutex_lock(&dc->reaper_lock);
		if (dc->active_vms > 0) {
			dc->active_vms--;
		}
		pthread_cond_broadcast(&dc->reaper_cond);
		pthread_mutex_unlock(&dc->reaper_lock);
	}
}

void reap_finished_vms_nonblocking(DataCenter *dc) {
	if (!dc->configured)
		return;

	while (1) {
		int status;
		pid_t pid = waitpid(-1, &status, WNOHANG);
		if (pid > 0) {
			reap_vm_by_pid(dc, pid);
		} else {
			break;
		}
	}
}

const char *vm_state_to_string(VMState state) {
    switch (state) {
        case VM_STATE_RESERVED:   return "RESERVED";
        case VM_STATE_RUNNING:    return "RUNNING";
        case VM_STATE_TERMINATED: return "TERMINATED";
        default:                  return "UNKNOWN";
    }
}

const char *res_state_to_string(ReservationState state) {
    switch (state) {
			case RES_STATE_PENDING:  return "PENDING";
			case RES_STATE_RUNNING:  return "RUNNING";
			case RES_STATE_FINISHED: return "FINISHED";
			default:                 return "UNKNOWN";
    }
}