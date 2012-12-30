#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <sys/inotify.h>
#include <string.h>
#include <errno.h>

/* local include */
#include "data.h"


/* This function initializes the watched list reading from the config file
 * for each filename we want to watch.
 * Returns -1 in case of error.
 */
int init_watched() 
{
	FILE *fp = fopen(CONFIG, "r");
	char line[MAX_LINE];
	char result[MAX_LINE];
	if (fp == NULL)
		return -1;


	if (watched != NULL) {
		/* Something else has tampered with the structure */
		errno = EINVAL;
		return -1;
	}

	struct br_file_list *file_it = watched = malloc(sizeof(*watched));
	if (watched == NULL) {
		return -1;
	}
	
	while (fgets(line, MAX_LINE, fp) != NULL) {
		if( line[0] == '#' || line[0] == ' ' || line[0] == '\n')
			continue;
		int res = sscanf(line,"%s", result);
		//if (res == 0)
		//	continue;
		char *entry = malloc(sizeof(char)*strlen(result)+1);
		if (entry == NULL)
			return -1;
		snprintf(entry, strlen(result)+1, "%s", result);
		dbg("Got %s to watch", entry);
		file_it->next = NULL;
		file_it->filename = entry;
		file_it->next = malloc(sizeof(file_it));
		file_it = file_it->next;
	}

	/* Second pass */
	struct br_file_list *tmp = watched;
	if (file_it == watched) {
		errno = ECANCELED; /* No need to do anything */
		return -1;
	}
	while (tmp->next != file_it)
		tmp = tmp->next;
	free(tmp->next);
	tmp->next = NULL;
	fclose(fp);
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
				temp->data->chroot = malloc(sizeof(char)*strlen(value)+1);
				snprintf(temp->data->chroot, strlen(value)+1, "%s", value);

				dbg("Found %s client at %s", temp->data->name, temp->data->chroot);
				
				/* clean some stuff for next iteration */
				in_client = 0;
				memset(section,0,7);
				memset(path,0,5);
				memset(value,0,strlen(value)*sizeof(char)); 
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
			temp->data->name = malloc(sizeof(target)*strlen(value)+1);
			snprintf(temp->data->name, strlen(value)+1, "%s", value);
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
			graph_it->fd_parent = *fd;
			graph_it->wd = inotify_add_watch(*fd, wholename, IN_DELETE_SELF);
			if (graph_it->wd < 0){
				return NULL;
			}
			graph_it->filename = wholename;
			syslog(LOG_DEBUG,"%s with %d and %d", wholename, *fd, graph_it->wd);
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
