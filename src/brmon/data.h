#ifndef __BRMON_DATA_H
#define __BRMON_DATA_H

/* configuration file mapping clients to their path */
#define BRCLIENTSCONF "/bedrock/etc/brclients.conf"

/* config data for brmon, includes list of watched files (abs path) */
#define CONFIG "/bedrock/etc/brmon.conf"

#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_SIZE (1024*EVENT_SIZE+16)
#define MAX_LINE 512

#ifdef DEBUG
#define dbg(...) syslog(LOG_DEBUG, __VA_ARGS__)
#else
#define dbg(...)
#endif

#define WATCH_FLAGS (IN_DELETE_SELF | IN_MODIFY)

struct br_client {
	char *name;
	char *chroot;
};

/* Simple list for watched files */
struct br_file_list {
	char *filename;
	struct br_file_list *next;
};

/* Simple list for bedrock clients */
struct br_client_list {
	struct br_client *data;
	struct br_client_list *next;
};

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
	int fd_parent; /* Inotify watching this */
	int wd; /* Inotify watch descriptor */
	char *filename; /* File reference */
	struct br_client *client; /* Chroot'd client identifier */
	struct br_file_graph *next; 
	struct br_file_graph *siblings; 
};

/* All the clients we have currently registered in the daemon */
extern struct br_client_list *clients;

/* The list of files read from the daemon's config */
extern struct br_file_list *watched;

/* Function definitions here */
extern int init_watched();
extern int init_clients();
extern struct br_file_graph* add_graph_nodes(int *fd);
extern int sync_filesystem();
extern int dispatch_inotify(int fd, struct br_file_graph *root);
extern int propagate_event(int fd, int wd, struct br_file_graph *root);

#endif //__BRMON_DATA_H
