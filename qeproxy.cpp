#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <map>

using namespace std;

#define MAXEVENTS 25
#define MAXLINE 25
#define LISTENQ 10
#define ENDMESSAGE '\n'
#define MAXMESSAGE 4000

typedef struct{
    int cfd; //the socket corresponding to the requesting client
    int tft; //the socket corresponding to the connection to the Web server
    int reqState; //the current state of the request (see Client Request States)
    char buffer[MAXMESSAGE]; //the buffer to read into and write from
    int clientreadsz; //the total number of bytes read from the client
    int servermsz; //the total number of bytes to write to the server
    int serverwrittensz; //the total number of bytes written to the server
    int serverreadsz; //the total number of bytes read from the server
    int clientwrittensz; //the total number of bytes written to the client
}request_info;

map<int, request_info*> RequestMap;

// Imagine network events as a binary signal pulse. 
// Edge triggered epoll only returns when an edge occurs, ie. transitioning from 0 to 1 or 1 to 0. 
// Regardless for how long the state stays on 0 or 1.
//
// Level triggered on the other hand will keep on triggering while the state stays the same.
// Thus you will read some of the data and then the event will trigger again to get more data
// You end up being triggered more to handle the same data
//
// Use level trigger mode when you can't consume all the data in the socket/file descriptor 
// and want epoll to keep triggering while data is available. 
//
//Typically one wants to use edge trigger mode and make sure all data available is read and buffered.
int g_edge_triggered = 1;

int open_listenfd(char *port)
{   
    int listenfd;
    int optval = 1;
    struct sockaddr_in ip4addr;

    ip4addr.sin_family = AF_INET;
    ip4addr.sin_port = htons(atoi(port));
    ip4addr.sin_addr.s_addr = INADDR_ANY;
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) 
    {
        perror("socket error");
        exit(EXIT_FAILURE);
    }
    /* Eliminates "Address already in use" error from bind */
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));

    if (bind(listenfd, (struct sockaddr*)&ip4addr, sizeof(struct sockaddr_in)) < 0) 
    {
        close(listenfd);
        perror("bind error");
        exit(EXIT_FAILURE);
    }
    if (listen(listenfd, 100) < 0) 
    {
        close(listenfd);
        perror("listen error");
        exit(EXIT_FAILURE);
    }

    return listenfd;
}

void modsocket(int epollfd, int connfd){
    // It is OK to use a stack variable here. The struct is simply a way to package arguments
    // The OS copies the struct values so it will be fine
    struct epoll_event event;
    event.data.fd = connfd;
    event.events = EPOLLIN;
    int s;

    if (g_edge_triggered)
    {
        event.events = EPOLLIN | EPOLLET;
    }

    s = epoll_ctl (epollfd, EPOLL_CTL_ADD, connfd, &event);
    if (s == -1)
    {
        perror ("epoll_ctl");
        abort ();
    }
}

int is_complete_request(char *buff)
{
	//printf("buff[%d]=[%c] bytesread = %d\n", n-1, buff[n-1], bytesread);
	//Client is sending hello\n\0 so need to back off 2
	if (buff[strlen(buff) - 1] == ENDMESSAGE)
		return 1;
	else
		return 0;
}

int read_data(int fd, char *buff, int size)
{
	int n = 0, bytesread;
	while ((bytesread = read(fd, &buff[n], size-n)) > 0)
		n += bytesread;
	return n;
}

void echo_data(int fd, char *buff, int size)
{
	printf("Sending: %s\n", buff);
	write(fd, buff, size+1);
}

void handle_response(int fd, char *request, int size)
{
	echo_data(fd, request, size);
    RequestMap[fd]->clientreadz = 0;
}

void handle_client_request(struct epoll_event *ev)
{
	char buff[MAXLINE];
	int n, bytesread;

      /* We have data on the fd waiting to be read. Read and
	 display it. If edge triggered, we must read whatever data is available
	 completely, as we are running in edge-triggered mode
	 and won't get a notification again for the same data. 
	 */
    request_info* ri = RequestMap[ev->data.fd];
	printf("Handling a client request\n");

	int done = 0;
	for(int i = 0; !done; i++)
	{
		int n = read_data(ev->data.fd, &(ri->buffer[ri->clientreadsz]), MAXLINE-1);
		ri->clientreadsz += n;
        if (!g_edge_triggered)
		{
			printf("Done & not edge_triggered\n");
			done = 1;
		}

		if (n == 0) // received 0 bytes
		{
			done = 1;

			if (i == 0) /* if we get 0 bytes the first time through, the socket has been closed */
			{
                ////CLEAN SPACE FROM MAP??
				/* Closing the descriptor will make epoll remove it
				from the set of descriptors which are monitored. */
				close (ev->data.fd);
				printf("Closed file descriptor %d\n", ev->data.fd);
			}
		}
		else 
		{
			ri->buffer[ri->clientreadsz] = '\0';
			printf("Read: [%s]\n",  ri->buffer);

			// Concatenate the data to a value in a map with the socket id as the key
			// concatval(ev->data.fd, buff)

			// If we find the end of message marker, respond
			if(is_complete_request(ri->buffer))
			{
				handle_response(ev->data.fd, ri->buffer, ri->clientreadsz);
			}

		}
	}
}

void handle_new_connection(int epollfd, struct epoll_event *ev)
{
	struct sockaddr_in in_addr;
	socklen_t addr_size = sizeof(in_addr);
	char hbuf[MAXLINE], sbuf[MAXLINE];

	int connfd = accept(ev->data.fd, (struct sockaddr *)(&in_addr), &addr_size);

	/* get the client's IP addr and port num */
	int s = getnameinfo ((struct sockaddr *)&in_addr, addr_size,
                                   hbuf, sizeof hbuf,
                                   sbuf, sizeof sbuf,
                                   NI_NUMERICHOST | NI_NUMERICSERV);
	if (s == 0)
	{
	    printf("Accepted connection on descriptor %d (host=%s, port=%s)\n", connfd, hbuf, sbuf);
	}

	/* Make the incoming socket non-blocking and add it to the list of fds to monitor. */
	int flags = fcntl (connfd, F_GETFL, 0);
	flags |= O_NONBLOCK;
	fcntl (connfd, F_SETFL, flags);

    modsocket(epollfd, connfd);


    request_info *ri = (request_info *) calloc(1, sizeof (request_info));
    ri->cfd = connfd;
    ri->clientreadsz = 0;
    RequestMap[connfd] = ri;
}



int main(int argc, char **argv)
{
    int listenfd, connfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr; /* Enough room for any addr */
    char client_hostname[MAXLINE], client_port[MAXLINE];
    
    struct epoll_event event, *events;
    
    int epollfd = epoll_create(LISTENQ);
    if(epollfd < 0)
    {
        printf("Unable to create epoll fd\n");
        exit(1);
    }

    if (argc < 2)
    {
        printf("Usage: %s portnumber\n", argv[0]);
        exit(1);
    }

    listenfd = open_listenfd(argv[1]);
    if (listenfd < 0)
    {
        printf("Unable to open listen socket on port %s\n", argv[1]);
        exit(1);
    }
    event.data.fd = listenfd;
    event.events = EPOLLIN | EPOLLET;  // just interested in read's events so using edge triggered mode
    int s = epoll_ctl (epollfd, EPOLL_CTL_ADD, listenfd, &event); // Add server socket FD to epoll's watched list
    if (s == -1)
    {
        perror ("epoll_ctl");
        abort ();
    }

    /* Events buffer used by epoll_wait to list triggered events */
    events = (struct epoll_event*) calloc (MAXEVENTS, sizeof(event));



    while (1)
    {
    	struct epoll_event rev;

    	int rc = epoll_wait (epollfd, events, MAXEVENTS, -1);  // Block until some events happen, no timeout
        for (int i = 0; i < rc; i++)
        {
        
            /* Error handling */
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN)))
            {
                /* An error has occured on this fd, or the socket is not
                 ready for reading (why were we notified then?) */
                fprintf (stderr, "epoll error\n");
                close (events[i].data.fd);  // Closing the fd removes from the epoll monitored list
                continue;
            }
            else if (events[i].data.fd == listenfd)
            {
                /* We have a notification on the listening socket, which
                means one or more incoming connections. */
                handle_new_connection(epollfd, &events[i]);
            }
            else if (events[i].events & EPOLLIN)
            {
                handle_client_request(&events[i]);
            }
            else
            {
                printf("Got some other event\n");
            }
        }
    }
}

