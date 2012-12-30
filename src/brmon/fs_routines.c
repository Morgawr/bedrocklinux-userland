#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <syslog.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>

/* local include */
#include "data.h"

/* This functions copies the new file (src) over the old one (dest).
 * At the moment it is implemented by spawning a child process to handle 
 * the copy. It is not as fast as a native copy but it is more stable (atm).
 * Returns -1 on error.
 * XXX: Change this to use a better copy
 */
static int copy_over(char *src, char *dest) 
{
	int child_exit;
	pid_t pid;
	
	pid = fork();

	dbg("Copying %s into %s", src, dest);

	if (pid == 0) { /* child */
		execl("/bin/cp", "/bin/cp", src, dest, (char *)0);
	}
	else if (pid < 0){
		return -1;
	}
	else {
		pid_t wait = waitpid(pid, &child_exit, 0);
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
	if (root == NULL) {
		errno = EINVAL;
		syslog(LOG_DEBUG,"root is null!");
		return -1;
	}
	/* Let's look for the file in our graph */
	struct br_file_graph *iterator = root;
	do {
		if (iterator->wd == wd && iterator->fd_parent == fd) 
			break;
		iterator = iterator->next;
	} while(iterator != NULL);

	if (iterator == NULL) {
		errno = EINVAL;
		return -1;
	}
	/* Now we found the file that got modified, we need to loop through 
	 * the rest of the data, perform consistency checks and re-link
	 * all the new inodes to the inotify watcher.
	 */
	struct br_file_graph *sibling = iterator->siblings;
	if (sibling == NULL) {
		errno = EINVAL;
		return -1;
	}

	struct stat buf;
	
	/* In case this goes through, it means the user manually deleted the 
	 * file but didn't update it with a copy. We need to re-sync. 
	 */
	if (stat(iterator->filename, &buf) < 0) {
		if (errno != ENOENT) /* something went wrong */
			return -1;

		dbg("Stray removal, replacing %s",iterator->filename);

		/* Let's get the old file back from the next sibling in line,
		 * if this one doesn't exist either then something went really
		 * wrong and we need to exit.
		 */
		struct stat buf2;
		if (stat(sibling->filename, &buf2) < 0) 
			return -1;

		if (copy_over(sibling->filename, iterator->filename) < 0)
			return -1;

		iterator->wd = inotify_add_watch(fd, iterator->filename, IN_DELETE_SELF);
		return 0;
	}

	/* First we add the new file to our inotify instance */
	iterator->wd = inotify_add_watch(fd, iterator->filename, IN_DELETE_SELF);
	if (iterator->wd < 0) 
		return -1;

	dbg("New %s, replacing old siblings", iterator->filename);

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
			dbg("Event received for file deletion");
			if (propagate_event(fd, event->wd, root) < 0) {
				return -1;
			}
		}
		i += EVENT_SIZE + event->len;
	}
	return 0;
}

/* This function should be called before creating the graph nodes for
 * the file network. It makes sure the filesystem is consistent between
 * all clients and that all clients are at the most recent sync version.
 * Return -1 in case of failure (for example a tracked file doesn't exit
 * in *ANY* client.
 */
int sync_filesystem() 
{
	int client_num = 0;
	struct br_client_list *client_it = clients;
	while (client_it != NULL) {
		++client_num; /* Count how many clients we need to check */
		client_it = client_it->next; 
	}

	struct br_file_list *file_it = watched;
	while (file_it != NULL) {
		char *siblings[client_num];
		int exists[client_num];
		char *base = file_it->filename;
		client_it = clients;
		memset(siblings, 0, sizeof(char*)*client_num); /* Make sure they are NULL */
		memset(exists, 0, sizeof(int)*client_num);
		int i = 0;
		while (client_it != NULL) {
			/* Obtain the filename and check for existence */
			struct br_client *client = client_it->data;
			char *wholename = NULL;
			int namelength = strlen(base)+strlen(client->chroot);
			++namelength; /* for NULL terminator */
			wholename = malloc(sizeof(char)*namelength);
			if (wholename == NULL)
				return -1;
			if (snprintf(wholename, namelength, "%s%s", client->chroot, base) < 0)
				return -1;
			siblings[i] = wholename;
			struct stat buf;
			if (stat(wholename, &buf) < 0) {
				if (errno != ENOENT)
					return -1;
				/* If errno is ENOENT then the file doesn't 
				 * exist so we don't have to report it on
				 * the array, it's already set to 0.
				 */
				syslog(LOG_NOTICE, "File %s doesn't exist.", wholename);
			}
			else {
				exists[i] = 1;
			}
			++i;
			client_it = client_it->next;
		}

		/* Now we have a list of all the siblings for the current
		 * file_it, we should check all the existing ones and find
		 * the one that has the most recent modification date, then
		 * sync it with the others.
		 * XXX: this policy is a bit nazi, will have to go back to it
		 *	and see if it's possible to do some diff'ing.
		 */
		time_t date = 0;
		char *filename = NULL;

		for (i = 0; i < client_num; i++) {
			if (!exists[i])
				continue;
			struct stat buf;
			if (stat(siblings[i], &buf) < 0) {
				return -1;
			}
			if (buf.st_mtime > date) {
				date = buf.st_mtime;
				filename = siblings[i];
			}
		}
		if (date == 0 || filename == NULL) {
			/* The tracked file doesn't exist in any client */
			errno = ENOENT;
			return -1;
		}
		dbg("%s has the latest modification date, re-syncing",filename);

		/* We do a second pass */
		for (i = 0; i < client_num; i++) {
			if (!exists[i]) {
				if (copy_over(filename, siblings[i]) < 0)
					return -1;
				continue;
			}
			struct stat buf;
			if (stat(siblings[i], &buf) < 0) {
				return -1;
			}
			if (buf.st_mtime < date) {
				if (copy_over(filename, siblings[i]) < 0)
					return -1;
			}
		}

		/* Let's free memory for next file iteration */
		for (i = 0; i < client_num; i++) {
			free(siblings[i]);
		}
		file_it = file_it->next;
	}
	return 0;
}
