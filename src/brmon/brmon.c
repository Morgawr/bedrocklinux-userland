/* This is the Bedrock daemon */
/* It performs consistency checks on files */
/* TODO: Add GPL here */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <string.h>
#include <sys/param.h>
#include <sys/inotify.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <syslog.h>

/* configuration file mapping clients to their path */
#define BRCLIENTSCONF "/bedrock/etc/brclients.conf"

/* config data for brmon, includes list of watched files (abs path) */
#define CONFIG "/bedrock/etc/brmon.conf"

/* Max size for client name, will need to ask paradigm about this */
#define CLIENT_NAME_MAX 50

#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_SIZE (1024*EVENT_SIZE+16)
#define MAX_LINE 512

struct br_client {
	char *name;
	char *chroot;
};

/* TODO: I might want to roll out a simple list implementation
 *	 generic enough to work for both clients and files but
 *	 I honestly cannot be arsed by it at the moment
 */
/* Simple list for watched files */
struct br_file_list {
	char *filename;
	struct br_file_list *next;
} *watched;

/* Simple list for bedrock clients */
struct br_client_list {
	struct br_client *data;
	struct br_client_list *next;
} *clients;

/* This graph-like structure is used for quick searches between
 * related files. It points to the inotify parent watching said file, to the
 * other nodes sharing the same parent, to the client the file refers to
 * and to the other graph instances.
 *
 * Keep in mind that "next" points to a non-circular list (last element is 
 * NULL) whereas "siblings" points to a circular list.
 *
 * XXX: Heuristically it might be even better to use "next" as a circular
 * 	list with a lot of jumping around. That would make the whole process'
 * 	memory more dynamic and react better with event distribution.
 */
struct br_file_graph {
	int *fd_parent; /* Inotify watching this XXX: maybe remove pointer */
	int wd; /* Inotify watch descriptor */
	char *filename; /* File reference */
	struct br_client *client; /* Chroot'd client identifier */
	struct br_file_graph *next; 
	struct br_file_graph *siblings; 
};

/* Creates network graph with nodes making them all related with each other,
 * returns the graph root on success, else NULL
 */
struct br_file_graph* add_graph_nodes(int *fd)
{
	struct br_file_graph *root = NULL;
	struct br_file_graph *graph_it = NULL;
	struct br_file_list *file_it = watched;
	while (file_it != NULL) {
		if (root == NULL)
			graph_it = root = malloc(sizeof(*graph_it));
		else if (graph_it == NULL)
			graph_it = malloc(sizeof(*graph_it));
		if (graph_it == NULL)
			return NULL;
		char *base = file_it->filename;
		struct br_client_list *client_it = clients;
		struct br_file_graph *first = graph_it;
		struct br_file_graph *box = NULL; /* previous step */
		while (client_it != NULL) {
			struct br_client *client = client_it->data;
			char *wholename = NULL;
			int namelength = strlen(base)+strlen(client->chroot);
			++namelength; /* for NULL terminator */
			wholename = malloc(sizeof(char)*namelength);
			if (wholename == NULL)
				return NULL;
			if (snprintf(wholename, namelength, "%s%s", client->chroot, base) < 0)
				return NULL;
			graph_it->fd_parent = fd;
			graph_it->wd = inotify_add_watch(*fd, wholename, IN_DELETE_SELF);
			if (graph_it->wd < 0){
				return NULL;
			}
			graph_it->filename = wholename;
			graph_it->client = client;
			graph_it->siblings = NULL;
			graph_it->next = NULL;
			box = graph_it;
			graph_it = malloc(sizeof(*graph_it));
			if (graph_it == NULL)
				return NULL;
			box->next = box->siblings = graph_it;
			client_it = client_it->next;
		}
		box->siblings = first; /* Loopback for relative nodes */
		file_it = file_it->next;
		fd++;
	}
	/* re-iterate and fix last element */
	struct br_file_graph *second_pass = root;
	while (second_pass->next != graph_it)
		second_pass = second_pass->next;
	second_pass->next = NULL;
	free(graph_it);
	return root;
}

/* This functions copies the new file (src) over the old one (dest).
 * At the moment it is implemented by spawning a child process to handle 
 * the copy. It is not as fast as a native copy but it is more stable (atm).
 * Returns -1 on error.
 * XXX: Change this to use a better copy
 */
int copy_over(char *src, char *dest) 
{
	int child_exit;
	pid_t pid;
	
	pid = fork();

	if (pid == 0) { /* child */
		execl("/bin/cp", "/bin/cp", src, dest, (char *)0);
	}
	else if (pid < 0){
		return -1;
	}
	else {
		pid_t wait = waitpid(pid, &child_exit, WNOHANG);
		if (wait == -1) {
			return -1;
		}

		if (WIFEXITED(child_exit)) {
			return WEXITSTATUS(child_exit);
		}
		else if (WIFSIGNALED(child_exit) || WIFSTOPPED(child_exit)) {
			errno = ECHILD;
			return -1;
		}
	}

	return -1;

}


/* Operates on the watch descriptor handling the event of a file being 
 * modified.
 * Returns -1 on failure.
 */
int propagate_event(int fd, int wd, struct br_file_graph *root)
{
	/* Let's look for the file in our graph */
	struct br_file_graph *iterator = root;
	do {
		if (iterator->wd == wd) /* found! */
			break;
		iterator = iterator->next;
	} while(iterator != NULL);

	if (iterator == NULL || (*iterator->fd_parent != fd)) {
		/* Inconsistency error, this shouldn't happen so we just 
		 * set errno and return.
		 */
		errno = EINVAL;
		return -1;
	}
	/* Now we found the file that got modified, we need to loop through 
	 * the rest of the data, perform consistency checks and re-link
	 * all the new inodes to the inotify watcher.
	 */
	struct br_file_graph *sibling = iterator->siblings;
	
	/* First we add the new file to our inotify instance */
	iterator->wd = inotify_add_watch(fd, iterator->filename, IN_DELETE_SELF);
	if (iterator->wd < 0) 
		return -1;

	while (iterator != sibling) {
		if (inotify_rm_watch(fd, sibling->wd) < 0) {
			return -1;
		}
		/* Here we need to perform the syncing
		 * and move the newly modified file over 
		 * the previosly tracked ones.
		 */
		if (copy_over(iterator->filename, sibling->filename) < 0)
			return -1;
		sibling->wd = inotify_add_watch(fd, sibling->filename, IN_DELETE_SELF);
		sibling = sibling->siblings;
	}

	/* aaaand.. done! */
	return 0;
}

/* Handle the event that a monitored file has been changed. 
 * Returns -1 on error
 */
int dispatch_inotify(int fd, struct br_file_graph *root)
{
	/* When a file is changed it means that we need to remove
	 * it from the inotify watch, re-link the new instance of the file
	 * and propagate the changes to all its siblings (removing and 
	 * re-adding all of them as well). Let's do that.
	 */

	char buffer[EVENT_BUF_SIZE];
	int i = 0;
	
	int length = read(fd, buffer, EVENT_BUF_SIZE);
	if (length < 0)
		return -1;
	while (i < length) {
		struct inotify_event *event = (struct inotify_event *)&buffer[i];
		if (event->mask & IN_DELETE_SELF) {
			if (propagate_event(fd, event->wd, root) < 0) {
				return -1;
			}
		}
		i += EVENT_SIZE + event->len;
	}
	return 0;
}

/* Function that populates the clients data reading from the br config file */
int init_clients()
{
	FILE *fp = fopen(BRCLIENTSCONF,"r");
	char line[MAX_LINE];
	if (fp == NULL)
		return -1;

	int in_client = 0; /* if we are in a client section */
	int retval = 0;

	/* We assume there is at least one client so we allocate the first one,
	 * if this isn't the case then we obviously have a serious error 
	 * here.
	 */
	clients = malloc(sizeof(*clients));
	clients->data = NULL;
	clients->next = NULL;
	struct br_client_list *temp = clients;

	/* This one stores "client" from sscanf */
	char section[7];
	/* This one stores "path" from sscanf */
	char path[5];
	/* This one stores the value read */
	char value[MAX_LINE];

	while (fgets(line, MAX_LINE, fp) != NULL) {
		/* Check for [client "blah"] */
		int res = sscanf(line," [%s %[^]]",section,value);
		if (in_client && res != 0) {
			errno = EINVAL;
			retval = -1;
			goto exit_read;
		}

		if (in_client && res == 0) { /* we look for "path = blah" */
			res = sscanf(line, " %[^= ] = %s", path, value);
			if (res != 0 && strncmp(path,"path",5) == 0) {
				temp->data->chroot = malloc(sizeof(value)*strlen(value));
				strncpy(temp->data->chroot, value, strlen(value));
				
				/* clean some stuff for next iteration */
				in_client = 0;
				memset(section,0,7);
				memset(path,0,5);
				memset(value,0,MAX_LINE); 
				printf("Found %s at %s\n", temp->data->name, temp->data->chroot);
				temp->next = malloc(sizeof(*temp));
				temp->next->data = NULL;
				temp->next->next = NULL;
				temp = temp->next;
			}

			continue;
		}

		if (res == 0)
			continue; /* failed match */


		/* If we got this far, we found a match */
		if (strncmp(section, "client", 7) == 0) {
			if (strncmp(value, "", 1) == 0) {
				errno = EINVAL;
				retval = -1;
				goto exit_read;
			}
			/* strip leading and trailing " */
			char* target = value+1;
			target[strlen(target)-1]='\0';
			in_client = 1;
			temp->data = malloc(sizeof(*temp->data));
			temp->data->name = malloc(sizeof(target)*strlen(value));
			strncpy(temp->data->name, target, strlen(target));
		}
	}

	if (in_client) { /* Malformed config file */
		errno = EINVAL;
		retval = -1;
		goto exit_read;
	}

	/* Perform sanity fix on client list */
	struct br_client_list *box = clients;
	while (box->next != temp)
		box = box->next;
	free(box->next);
	box->next = NULL;

exit_read:
	fclose(fp);
	return retval;
}

int main(int argc, const char *argv[])
{
	/* TODO: Use getopt to check params */
	/* TODO: Check if we were called from root */

	pid_t pid, sid;
	int exit_status = 0;
	struct br_file_graph *file_network = NULL; /* all our file instances */
	watched = NULL;
	clients = NULL;
	
	/* Dynamically allocated array holding all the instances
	 * for inotify watchers
	 */
	int *inotify_instances = NULL;
	int inotify_count = 0;

	/* Fork parent to detach from terminal */
	pid = fork();
	if (pid < 0) {
		perror("fork()");
		exit(EXIT_FAILURE);
	}

	/* Kill off the parent */
	if (pid > 0) {
		exit(EXIT_SUCCESS);
	}

	umask(0);

	openlog("brmon", LOG_PID | LOG_NDELAY, LOG_DAEMON);

	sid = setsid();
	if (sid < 0) {
		exit_status = EXIT_FAILURE;
		syslog(LOG_ERR, "%s", strerror(errno));
		goto just_exit;
	}

	/* Close standard streams */
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	/* we need to setup all the inotify instances, 
	 * but first we need to read from the config file
	 * and discover all the clients available
	 */

	/* Setup the watched list reading from config file */
	/* TODO: read config */
	watched = malloc(sizeof(*watched));
	watched->next = NULL;
	/* Temporary stuff */
	#define FILENAME "/etc/resolv.conf"
	watched->filename = malloc(strlen(FILENAME)+1);
	sprintf(watched->filename,FILENAME);
	#undef FILENAME


	/* Populate the clients structure */
	if (init_clients() < 0) {
		syslog(LOG_ERR, "%s. Unable to read clients.", strerror(errno));
		goto exit_free;
	}

	/* Init inotify */
	struct br_file_list *iterator = watched;
	inotify_count = 0;
	while (iterator != NULL) {
		inotify_count++;
		iterator = iterator->next;
	}

	inotify_instances = malloc(sizeof(int)*inotify_count); 
	for (int i = 0; i < inotify_count; i++) {
		inotify_instances[i] = inotify_init();
		if (inotify_instances[i] < 0) {
			syslog(LOG_ERR, "%s. Unable to start inotify instances", strerror(errno));
			goto exit_free;
		}
	}

	file_network = add_graph_nodes(inotify_instances);
	if (file_network == NULL) {
		syslog(LOG_ERR, "%s. Failed to initialize sibling files.", strerror(errno));
		goto exit_free;
	}

	/* Now we create the fd_set for all the inotify instances */
	fd_set fds;
	FD_ZERO(&fds);
	for(int i = 0; i < inotify_count; i++)
		FD_SET(inotify_instances[i],&fds);

	/* Enter infinite loop for daemon */
	for (;;) {
		fd_set fds_copy;
		memcpy(&fds_copy, &fds, sizeof(fd_set));
		int nfds = inotify_instances[inotify_count-1]+1;
		int retval = select(nfds, &fds_copy, NULL, NULL, NULL);
		
		if (retval < 0) { /* error! */
			if (errno == EINTR)
				continue; /* just kidding, only a signal :D */
			syslog(LOG_ERR, "%s. Unable to select on inotify descriptors.", strerror(errno);
			goto exit_inotify;
		}
		
		if (retval == 0) /* this should never happen, just to be safe */
			continue;

		for (int i = 0; i < inotify_count; i++) {
			if (FD_ISSET(inotify_instances[i], &fds_copy)) {
				if (dispatch_inotify(inotify_instances[i],
							file_network) < 0) {
					syslog(LOG_ERR, "%s. Unable to dispatch and propagate file sync.", strerror(errno));
					goto exit_inotify;
				}
			}
		}
	}

/* If something goes wrong and we need to exit the daemon */
exit_inotify:

	for(struct br_file_graph *tmp = file_network; tmp != NULL; tmp = tmp->next) {
		inotify_rm_watch(*tmp->fd_parent, tmp->wd);
	}

	/* Close inotify instances */
	for (int i = 0; i < inotify_count; i++) {
		close(inotify_instances[i]);
	}

exit_free:

	/* Free allocated memory here */
	free(inotify_instances);
	struct br_file_graph *temp_network = file_network;
	while (temp_network != NULL) {
		struct br_file_graph *p = temp_network;
		free(p->filename);
		temp_network = temp_network->next;
		free(p);
		/* don't need to free anything else because other pointers are
		 * already handled by others 
		 */
	}

	/* The list of watched files */
	while (watched != NULL) {
		struct br_file_list *p = watched;
		watched = watched->next;
		free(p->filename);
		free(p);
	}

	/* The list of clients */
	while (clients != NULL) {
		struct br_client_list *p = clients;
		clients = clients->next;
		free(p->data->name);
		free(p->data->chroot);
		free(p->data);
		free(p);
	}

just_exit:
	syslog(LOG_WARNING, "Daemon has been stopped. Client sync will not work.");
	closelog();
	exit(exit_status);
}

