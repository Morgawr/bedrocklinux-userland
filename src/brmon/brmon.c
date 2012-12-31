/* 
 * This is brmon.c, part of the Bedrock daemon project (brmon). 
 * This file contains the main body of the application which 
 * has the purpose of sync'ing special files on the Bedroc
 * Linux system between each individual client.
 *
 * Copyright (C) 2012 Federico 'Morgawr' Pareschi <morgawr@gmail.com> 
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/select.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
/* Local includes for all data structures and functions */
#include "data.h"

struct br_file_list *watched;
struct br_client_list *clients;

int main(int argc, const char *argv[])
{
	/* TODO: Use getopt to check params */
	/* TODO: Check if we were called from root */

	pid_t pid, sid;
	int exit_status = 0;
	struct br_file_graph *file_network = NULL; /* all our file instances */
	int null_fd = 0;
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

	openlog("brmon", LOG_PID, LOG_DAEMON);
	syslog(LOG_INFO, "Daemon started.");

	sid = setsid();
	if (sid < 0) {
		exit_status = EXIT_FAILURE;
		syslog(LOG_ERR, "%s", strerror(errno));
		goto just_exit;
	}

	/* Redirect standard streams to /dev/null */
	null_fd = open("/dev/null", O_WRONLY);
	dup2(null_fd, STDIN_FILENO);
	dup2(null_fd, STDOUT_FILENO);
	dup2(null_fd, STDERR_FILENO);

	/* we need to setup all the inotify instances, 
	 * but first we need to read from the config file
	 * and discover all the clients available
	 */

	dbg("Initializing watched list");
	/* Setup the watched list reading from config file */
	if (init_watched() < 0) {
		syslog(LOG_ERR, "%s. Unable to read config file.", strerror(errno));
		goto just_exit;
	}

	dbg("Initializing client list");
	/* Populate the clients structure */
	if (init_clients() < 0) {
		syslog(LOG_ERR, "%s. Unable to read clients.", strerror(errno));
		goto exit_free;
	}

	dbg("Initializing inotify list");
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

	dbg("Syncing filesystem");
	/* Check if all the sibling files exist, if they do not then
	 * sync them all together using the most recent stat'd modification
	 */
	if (sync_filesystem() < 0) {
		syslog(LOG_ERR, "%s. Unable to sync initialized siblings.", strerror(errno));
		goto exit_free;
	}

	dbg("Adding nodes to graph");
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

	dbg("Entered main loop");
	/* Enter infinite loop for daemon */
	for (;;) {
		fd_set fds_copy;
		memcpy(&fds_copy, &fds, sizeof(fd_set));
		int nfds = inotify_instances[inotify_count-1]+1;
		int retval = select(nfds, &fds_copy, NULL, NULL, NULL);
		
		if (retval < 0) { /* error! */
			if (errno == EINTR) {
				dbg("Received interrupt signal");
				continue; /* just kidding, only a signal :D */
			}
			syslog(LOG_ERR, "%s. Unable to select on inotify descriptors.", strerror(errno));
			goto exit_inotify;
		}
		
		if (retval == 0) /* this should never happen, just to be safe */
			continue;

		for (int i = 0; i < inotify_count; i++) {
			if (FD_ISSET(inotify_instances[i], &fds_copy)) {
				dbg("Found event for %d",inotify_instances[i]);
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
		inotify_rm_watch(tmp->fd_parent, tmp->wd);
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
	close(null_fd);
	exit(exit_status);
}

