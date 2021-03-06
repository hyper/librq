// librq
// RISP-based queue system

#include "rq.h"
#include <rq-proto.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <pwd.h>
#include <rispbuf.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>


#if (LIBRQ_VERSION != 0x00010910)
	#error "Incorrect rq.h header version."
#endif

#if (RQ_PROTO_VERSION < 0x00010000)
	#error "Need rq-proto.h v1.0 or higher"
#endif


// libevent compatability.   If libevent1.x is used, it doesnt have as much stuff as libevent2.0
#ifdef LIBEVENT_OLD_VER  

	// event_new is a 2.0 function that creates a new event, sets it and then returns the pointer to the new event structure.   1.4 does not have that function, so we wrap an event_set here instead.
	struct event * event_new(struct event_base *evbase, evutil_socket_t sfd, short flags, void (*fn)(int, short, void *), void *arg) {
		struct event *ev;
		assert(evbase && sfd >= 0 && flags != 0 && fn);
		ev = calloc(1, sizeof(*ev));
		assert(ev);
		event_set(ev, sfd, flags, fn, arg);
		event_base_set(evbase, ev);
		return(ev);
	}

	void event_free(struct event *ev) {
		assert(ev);
		event_del(ev);
		free(ev);
	}

	struct event * evsignal_new(struct event_base *evbase, int sig, void (*fn)(int, short, void *), void *arg) {
		struct event *ev;
		ev = event_new(evbase, sig, EV_SIGNAL|EV_PERSIST, fn, arg);
		assert(ev);
		return(ev);
	}


	// pulled in from libevent 2.0.3 (alpha) to add compatibility for older libevents.
	int evutil_parse_sockaddr_port(const char *ip_as_string, struct sockaddr *out, int *outlen) {
		int port;
		char buf[128];
		const char *cp, *addr_part, *port_part;
		int is_ipv6;
		/* recognized formats are:
		* [ipv6]:port
		* ipv6
		* [ipv6]
		* ipv4:port
		* ipv4
		*/

// 		printf("evutil_parse_sockaddr_port: '%s'\n", ip_as_string);
		
		cp = strchr(ip_as_string, ':');
		if (*ip_as_string == '[') {
			int len;
			if (!(cp = strchr(ip_as_string, ']'))) { return -1; }
			len = cp-(ip_as_string + 1);
			if (len > (int)sizeof(buf)-1) { return -1; }
			memcpy(buf, ip_as_string+1, len);
			buf[len] = '\0';
			addr_part = buf;
			if (cp[1] == ':') port_part = cp+2;
			else port_part = NULL;
			is_ipv6 = 1;
		} else if (cp && strchr(cp+1, ':')) {
			is_ipv6 = 1;
			addr_part = ip_as_string;
			port_part = NULL;
		} else if (cp) {
			is_ipv6 = 0;
			if (cp - ip_as_string > (int)sizeof(buf)-1) { return -1; }
			memcpy(buf, ip_as_string, cp-ip_as_string);
			buf[cp-ip_as_string] = '\0';
			addr_part = buf;
			port_part = cp+1;
		} else {
			addr_part = ip_as_string;
			port_part = NULL;
			is_ipv6 = 0;
		}
		
		if (port_part == NULL) { port = 0; } 
		else {
			port = atoi(port_part);
			if (port <= 0 || port > 65535) { return -1; }
		}
		
		if (!addr_part) return -1; /* Should be impossible. */

		struct sockaddr_in sin;
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		sin.sin_port = htons(port);
		if (1 != inet_pton(AF_INET, addr_part, &sin.sin_addr)) return -1;
		if (sizeof(sin) > *outlen) return -1;
		memset(out, 0, *outlen);
		memcpy(out, &sin, sizeof(sin));
		*outlen = sizeof(sin);
		return 0;
	}


#endif
// LIBEVENT_OLD_VER



// pre-declare the handlers otherwise we end up with a precedence loop.
static void rq_read_handler(int fd, short int flags, void *arg);
static void rq_write_handler(int fd, short int flags, void *arg);
static void rq_connect_handler(int fd, short int flags, void *arg);



static void rq_data_init(rq_data_t *data)
{
	assert(data);
	
	data->flags = 0;
	data->mask = 0;
	
	data->id = 0;
	data->qid = 0;
	data->timeout = 0;
	data->priority = 0;

	// we dont allocate the payload object yet, because we need to move it to
	// the message processing it.
	data->payload = NULL;
	data->queue = expbuf_init(NULL, 0);
}

static void rq_data_free(rq_data_t *data)
{
	assert(data);

	if (data->payload) {
		expbuf_clear(data->payload);
		data->payload = expbuf_free(data->payload);
		assert(data->payload == NULL);
	}

	assert(data->queue);
	expbuf_clear(data->queue);
	data->queue = expbuf_free(data->queue);
	assert(data->queue == NULL);
}



void rq_queue_init(rq_queue_t *queue)
{
	assert(queue != NULL);

	queue->queue = NULL;
	queue->qid = 0;
	queue->handler = NULL;
	queue->accepted = NULL;
	queue->dropped = NULL;
	queue->arg = NULL;
}

void rq_queue_free(rq_queue_t *queue)
{
	assert(queue != NULL);
	if (queue->queue != NULL) {
		free(queue->queue);
		queue->queue = NULL;
	}
	queue->qid = 0;
	queue->handler = NULL;
	queue->accepted = NULL;
	queue->dropped = NULL;
	queue->arg = NULL;
}







//-----------------------------------------------------------------------------
// Assuming that the rq structure has been properlly filled out, this function
// will initiate the connection process to a specified IP address.   Since the
// application may be using the event loop for other things, and since we need
// to support primary and secondary controllers, we need to connect in
// non-blocking mode.
//
// This function will ONLY attempt to connect to the connection at the top of
// the list.  To cycle through to a different controller, some other
// functionality will need to move the top conn to the tail.  This would either
// be because the controller sent a CLOSING instruction, or the socket
// connection failed.   This means that if the connection is dropped, that
// functionality should move the current conn to the tail, and the next connect
// attempt would be against the alternate controller.
static void rq_connect(rq_t *rq)
{
	rq_conn_t *conn;
	struct sockaddr saddr;
	int result;
	int len;

	assert(rq != NULL);

	assert(rq->evbase != NULL);
	assert(ll_count(&rq->connlist) > 0);

	// make a note of the first connection in the list.
	conn = ll_get_head(&rq->connlist);

	if (conn->shutdown == 0 && conn->closing == 0 && conn->connect_event == NULL && conn->active == 0) {
	
		assert(conn->hostname != NULL);
		assert(conn->hostname[0] != 0);
		assert(conn->read_event == NULL);
		assert(conn->write_event == NULL);
		assert(conn->connect_event == NULL);
		assert(conn->handle == INVALID_HANDLE);
		
		len = sizeof(saddr);
		if (evutil_parse_sockaddr_port(conn->hostname, &saddr, &len) != 0) {
			// unable to parse the detail.  What do we need to do?
			assert(0);
		}
		else {
			// create the socket, and set to non-blocking mode.
									
			conn->handle = socket(AF_INET,SOCK_STREAM,0);
			assert(conn->handle >= 0);
			evutil_make_socket_nonblocking(conn->handle);

			result = connect(conn->handle, &saddr, sizeof(saddr));
			assert(result < 0);
			assert(errno == EINPROGRESS);
	
			assert(conn->inbuf == NULL);
			assert(conn->outbuf);
			assert(conn->readbuf == NULL);

			assert(conn->data == NULL);
	
			// connect process has been started.  Now we need to create an event so that we know when the connect has completed.
			assert(conn->rq);
			assert(conn->rq->evbase);
			conn->connect_event = event_new(conn->rq->evbase, conn->handle, EV_WRITE, rq_connect_handler, conn);
			assert(conn->connect_event);
			event_add(conn->connect_event, NULL);	// TODO: Should we set a timeout on the connect?
		}
	}
}



//-----------------------------------------------------------------------------
// This function is called only when we lose a connection to the controller.
// Since the connection to the controller has failed in some way, we need to
// move that connection to the tail of the list.
static void rq_conn_closed(rq_conn_t *conn)
{
	int i;
	rq_message_t *msg;
	
	assert(conn);
	assert(conn->rq);

	assert(conn->handle != INVALID_HANDLE);
	close(conn->handle);
	conn->handle = INVALID_HANDLE;

	// free all the buffers.
	assert(conn->readbuf);
	assert(BUF_LENGTH(conn->readbuf) == 0);
	conn->readbuf = expbuf_free(conn->readbuf);
	assert(conn->readbuf == NULL);

	assert(conn->sendbuf);
	assert(BUF_LENGTH(conn->sendbuf) == 0);
	conn->sendbuf = expbuf_free(conn->sendbuf);
	assert(conn->sendbuf == NULL);

	if (conn->inbuf) {
		expbuf_clear(conn->inbuf);
		conn->inbuf = expbuf_free(conn->inbuf);
		assert(conn->inbuf == NULL);
	}
	
	assert(conn->outbuf);
	expbuf_clear(conn->outbuf);

	// cleanup the data structure.
	if (conn->data) {
		rq_data_free(conn->data);
		free(conn->data);
		conn->data = NULL;
	}

	// remove the conn from the connlist.  It should actually be the highest one in the list, and then put it at the tail of the list.
	assert(conn->rq);
	assert(ll_count(&conn->rq->connlist) > 0);
	if (ll_count(&conn->rq->connlist) > 1) {
		ll_remove(&conn->rq->connlist, conn);
		ll_push_tail(&conn->rq->connlist, conn);
	}

	// clear the events
	if (conn->read_event) {
		event_free(conn->read_event);
		conn->read_event = NULL;
	}
	if (conn->write_event) {
		event_free(conn->write_event);
		conn->write_event = NULL;
	}
	assert(conn->connect_event == NULL);

	// timeout all the pending messages, if there are any.
	if (conn->rq->msg_used > 0) {
		for (i=0; i<conn->rq->msg_max; i++) {
			if (conn->rq->msg_list[i]) {
				msg = conn->rq->msg_list[i];
				if (msg->conn == conn) {
					assert(0);
				}
			}
		}
	}
	
	conn->active = 0;
	conn->closing = 0;

	// initiate a connect on the head of the list.
	assert(conn->rq);
	rq_connect(conn->rq);
}

//-----------------------------------------------------------------------------
// this function is used internally to send the data to the connected RQ
// controller.  It will put the data in the outbuffer, and if the outbuffer was previously empty, then we will set the write event.
static void rq_senddata(rq_conn_t *conn, char *data, int length)
{
	assert(conn);
	assert(data);
	assert(length > 0);
	assert(conn->handle != INVALID_HANDLE);

	// add the new data to the buffer.
	assert(conn->outbuf);
	expbuf_add(conn->outbuf, data, length);

	// if we dont already have a write event set, then create one.
	if (conn->write_event == NULL) {
		assert(conn->rq);
		assert(conn->rq->evbase);
		conn->write_event = event_new(conn->rq->evbase, conn->handle, EV_WRITE | EV_PERSIST, rq_write_handler, conn);
		assert(conn->write_event);
		event_add(conn->write_event, NULL);
		
// 		printf("rq_senddata: created WRITE event for socket:%d.\n", conn->handle);
	}
}


//-----------------------------------------------------------------------------
// Send a message to the controller that basically states that this client is
// closing its connection.  Since we are sending a single command we dont need
// to go through the expence of gettng a buffer from the bufpool.
static void rq_send_closing(rq_conn_t *conn)
{
	char buf[1];
	
	buf[0] = RQ_CMD_CLOSING;
	assert(conn);
	rq_senddata(conn, buf, 1);
}



//-----------------------------------------------------------------------------
// Used to tell the library that it needs to start shutting down connections so
// that the application can exit the loop.
void rq_shutdown(rq_t *rq)
{
	rq_conn_t *conn;
	int pending;
	
	assert(rq);

	// go thru the connect list, and tell each one that it is shutting down.
	ll_start(&rq->connlist);
	while ((conn = ll_next(&rq->connlist))) {

		// Because the list entries may move around while being processed, we may
		// need to restart the list from the head again.  Therefore we need to
		// check to see if we have already processed this connection.
		if (conn->shutdown == 0) {
			conn->shutdown ++;
	
			if (conn->handle != INVALID_HANDLE) {
	
				// if we have a handle, but not marked as active, then we must still be connecting.
				if (conn->active == 0) {
					assert(conn->closing == 0);
					
					// need to close the connection, and remove the connect event.
					assert(conn->connect_event);
					event_free(conn->connect_event);
					conn->connect_event = NULL;
					rq_conn_closed(conn);
					assert(conn->closing == 0);

					// closing the connection would have moved the conns around in the
					// list, so we need to reset the 'next'.  This means the loop will
					// restart again, but we wont process the ones that we have already
					// marked as 'shutdown'
					ll_finish(&rq->connlist);
					ll_start(&rq->connlist);
				}
				else {
					assert(conn->active > 0);
					assert(conn->connect_event == NULL);
					assert(conn->read_event);
					
					// send 'closing' message to each connected controller.  This should remove the node from any queues it is consuming.
					rq_send_closing(conn);
	
					assert(conn->closing == 0);
					conn->closing ++;
				
					// we need to wait if we are still processing some messages from a queue being consumed.
					pending = rq->msg_used;

					// close connections if there are no messages waiting to be processed on it.
					if (pending == 0) {
						rq_conn_closed(conn);
						assert(conn->closing == 0);
						
						// closing the connection would have moved the conns around in the
						// list, so we need to reset the 'next'.  This means the loop will
						// restart again, but we wont process the ones that we have already
						// marked as 'shutdown'
						ll_finish(&rq->connlist);
						ll_start(&rq->connlist);
					}
				}
			}
		}
	}
	ll_finish(&rq->connlist);
}


void rq_cleanup(rq_t *rq)
{
	rq_queue_t *q;
	rq_conn_t *conn;
	
	assert(rq != NULL);

	// cleanup the risp object.
	assert(rq->risp != NULL);
	rq->risp = risp_shutdown(rq->risp);
	assert(rq->risp == NULL);

	// free the resources allocated for the connlist.
	while ((conn = ll_pop_head(&rq->connlist))) {
		assert(conn->handle == INVALID_HANDLE);
		assert(conn->active == 0);
		assert(conn->closing == 0);
		assert(conn->shutdown > 0);

		assert(conn->read_event == NULL);
		assert(conn->write_event == NULL);
		assert(conn->connect_event == NULL);

		assert(conn->outbuf);
		conn->outbuf = expbuf_free(conn->outbuf);
		assert(conn->outbuf == NULL);
		
		conn->rq = NULL;
		conn->risp = NULL;
	
		assert(conn->hostname);
		free(conn->hostname);
		conn->hostname = NULL;

		assert(conn->inbuf == NULL);
		assert(conn->readbuf == NULL);
		assert(conn->data == NULL);
	}
	assert(ll_count(&rq->connlist) == 0);
	ll_free(&rq->connlist);

	// cleanup all the queues that we have.
	while ((q = ll_pop_head(&rq->queues))) {
		rq_queue_free(q);
		free(q);
	}

	assert(rq->msg_list);
	assert(rq->msg_used == 0);
	while (rq->msg_max > 0) {
		rq->msg_max --;
		assert(rq->msg_list[rq->msg_max] == NULL);
	}
	free(rq->msg_list);
	rq->msg_list = NULL;


	// cleanup the msgpool
	assert(rq->msg_pool);
	while ((ll_pop_head(rq->msg_pool)));
	ll_free(rq->msg_pool);
	free(rq->msg_pool);
	rq->msg_pool = NULL;
}


void rq_setevbase(rq_t *rq, struct event_base *base)
{
	assert(rq);

	if (base) {
		assert(rq->evbase == NULL);
		rq->evbase = base;
	}
	else {
		assert(rq->evbase);
		rq->evbase = NULL;
	}
}





//-----------------------------------------------------------------------------
// this function is an internal one that is used to read data from the socket.
// It is assumed that we are pretty sure that there is data to be read (or the
// socket has been closed).
static void rq_process_read(rq_conn_t *conn)
{
	int res, empty;
	
	assert(conn);
	assert(conn->rq);
	assert(conn->risp);
	assert(conn->readbuf);
	
	assert(BUF_LENGTH(conn->readbuf) == 0);
	assert(BUF_MAX(conn->readbuf) >= RQ_DEFAULT_BUFFSIZE);

	empty = 0;
	while (empty == 0) {
		assert(BUF_LENGTH(conn->readbuf) == 0);
		assert(conn->handle != INVALID_HANDLE && conn->handle > 0);
		assert(BUF_DATA(conn->readbuf) != NULL  && BUF_MAX(conn->readbuf) > 0);
		
		res = read(conn->handle, BUF_DATA(conn->readbuf), BUF_MAX(conn->readbuf));
		if (res > 0) {
			BUF_LENGTH(conn->readbuf) = res;
			assert(BUF_LENGTH(conn->readbuf) <= BUF_MAX(conn->readbuf));

			// if we pulled out the max we had avail in our buffer, that means we
			// can pull out more at a time, so we should increase our buffer size by
			// RQ_DEFAULT_BUFFSIZE amount.  This will increase the size of the
			// buffer in rather small chunks, which might not be optimal.
			if (res == BUF_MAX(conn->readbuf)) {
				expbuf_shrink(conn->readbuf, RQ_DEFAULT_BUFFSIZE);
				assert(empty == 0);
			}
			else { empty = 1; }
			
			// if there is no data in the in-buffer, then we will process the common buffer by itself.
			if (conn->inbuf == NULL) {
				res = risp_process(conn->risp, conn, BUF_LENGTH(conn->readbuf), (unsigned char *) BUF_DATA(conn->readbuf));
				assert(res <= BUF_LENGTH(conn->readbuf));
				assert(res >= 0);
				if (res > 0) { expbuf_purge(conn->readbuf, res); }

				// if there is data left over, then we need to add it to our in-buffer.
				if (BUF_LENGTH(conn->readbuf) > 0) {
					assert(conn->inbuf == NULL);
					conn->inbuf = expbuf_init(NULL, BUF_LENGTH(conn->readbuf));
					assert(conn->inbuf);
					
					expbuf_add(conn->inbuf, BUF_DATA(conn->readbuf), BUF_LENGTH(conn->readbuf));
					expbuf_clear(conn->readbuf);
				}
			}
			else {
				// we have data left in the in-buffer, so we add the content of the common buffer
				assert(BUF_LENGTH(conn->readbuf) > 0);
				assert(conn->inbuf);
				assert(BUF_LENGTH(conn->inbuf) > 0);
				expbuf_add(conn->inbuf, BUF_DATA(conn->readbuf), BUF_LENGTH(conn->readbuf));
				expbuf_clear(conn->readbuf);
				assert(BUF_LENGTH(conn->readbuf) == 0);
				assert(BUF_LENGTH(conn->inbuf) > 0 && BUF_DATA(conn->inbuf) != NULL);

				// and then process what is there.
				res = risp_process(conn->risp, conn, BUF_LENGTH(conn->inbuf), (unsigned char *) BUF_DATA(conn->inbuf));
				assert(res <= BUF_LENGTH(conn->inbuf));
				assert(res >= 0);
				if (res > 0) { expbuf_purge(conn->inbuf, res); }

				if (BUF_LENGTH(conn->inbuf) == 0) {
					// the in buffer is now empty, so we should free it.
					conn->inbuf = expbuf_free(conn->inbuf);
					assert(conn->inbuf == NULL);
				}
			}
			
			assert(conn->readbuf);
			assert(BUF_LENGTH(conn->readbuf) == 0);
		}
		else {
			assert(empty == 0);
			empty = 1;
			
			if (res == 0) {
				rq_conn_closed(conn);
				assert(conn->readbuf == NULL);
			}
			else {
				assert(res == -1);
				if (errno != EAGAIN && errno != EWOULDBLOCK) {
					rq_conn_closed(conn);
					assert(conn->readbuf == NULL);
				}
				else {
					assert(conn->readbuf);
				}
			}
		}
	}

}




static void rq_write_handler(int fd, short int flags, void *arg)
{
	rq_conn_t *conn = (rq_conn_t *) arg;
	int res;

	assert(fd >= 0);
	assert(flags != 0);
	assert(conn);
	assert(conn->rq);
	assert(conn->handle == fd);
	assert(conn->active > 0);
	assert(flags & EV_WRITE);
	assert(conn->write_event);

	assert(conn->outbuf);
	assert(BUF_LENGTH(conn->outbuf) > 0);
	assert(conn->active > 0);
	
// 	printf("rq_write_handler: attempting to send %d bytes for socket: %d\n", BUF_LENGTH(conn->outbuf), fd);
	
	// send the data that is waiting in the outbuffer.
	res = send(conn->handle, BUF_DATA(conn->outbuf), BUF_LENGTH(conn->outbuf), 0);
	if (res > 0) {
		assert(res <= BUF_LENGTH(conn->outbuf));
		expbuf_purge(conn->outbuf, res);
// 		printf("rq_write_handler: sent %d bytes for socket: %d\n", res, fd);
	}
	else if (res == 0 || (res == -1 && errno != EAGAIN && errno != EWOULDBLOCK)) {
// 		printf("rq_write_handler: closing socket %d.\n", fd);
		assert(conn);
		rq_conn_closed(conn);
	}

	// if we dont have any more to send, then we need to remove the WRITE event, and return the outbuffer.
	assert(conn->outbuf);
	if (BUF_LENGTH(conn->outbuf) == 0) {
		// clear the write event
		event_free(conn->write_event);
		conn->write_event = NULL;
	}
}	



//-----------------------------------------------------------------------------
// This internal function is used to actually send a queue consume request 
static void rq_send_consume(rq_conn_t *conn, rq_queue_t *queue)
{
	assert(conn);
	assert(queue);

	assert(queue->queue);
	assert(queue->max >= 0);
	assert(strlen(queue->queue) > 0 && strlen(queue->queue) < 256);

	// get a buffer from the bufpool.
	assert(conn->sendbuf);
	assert(BUF_LENGTH(conn->sendbuf) == 0);
	

	// send consume request to controller.
	addCmd(conn->sendbuf, RQ_CMD_CLEAR);
	if (queue->exclusive != 0)
		addCmd(conn->sendbuf, RQ_CMD_EXCLUSIVE);
	addCmdShortStr(conn->sendbuf, RQ_CMD_QUEUE, strlen(queue->queue), queue->queue);
	addCmdInt(conn->sendbuf, RQ_CMD_MAX, queue->max);
	addCmdShortInt(conn->sendbuf, RQ_CMD_PRIORITY, queue->priority);
	addCmd(conn->sendbuf, RQ_CMD_CONSUME);

	rq_senddata(conn, BUF_DATA(conn->sendbuf), BUF_LENGTH(conn->sendbuf));
	expbuf_clear(conn->sendbuf);
}


static void rq_connect_handler(int fd, short int flags, void *arg)
{
	rq_conn_t *conn = (rq_conn_t *) arg;
	rq_queue_t *q;
	socklen_t foo;
	int error;

	assert(fd >= 0);
	assert(flags != 0);
	assert(conn);
	assert(conn->rq);
	assert(conn->handle == fd);
	assert(flags & EV_WRITE);
	assert(conn->rq->evbase != NULL);

	// remove the connect handler
	assert(conn->connect_event);
	event_free(conn->connect_event);
	conn->connect_event = NULL;

	foo = sizeof(error);
	getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &foo);
	if (error == ECONNREFUSED) {
		// connect failed...  we need to move this conn object from the list and put it at the tail.
	
		assert(conn->active == 0);
		assert(conn->closing == 0);
		assert(conn->connect_event == NULL);
		assert(conn->data == NULL);

		rq_conn_closed(conn);
	}
	else {

		assert(conn->active == 0);
		conn->active ++;

		// now that we have connected, we should get a buffer to handle read data.
		assert(conn->readbuf == NULL);
		conn->readbuf = expbuf_init(NULL, RQ_DEFAULT_BUFFSIZE);
		assert(conn->readbuf);

		// we should also prepare the 'sendbuf' even though it wont be needed until we send something.
		// better to create it now, rather than having to test for it and create it later.
		assert(conn->sendbuf == NULL);
		conn->sendbuf = expbuf_init(NULL, RQ_DEFAULT_BUFFSIZE);
		assert(conn->sendbuf);
		
		// make sure our other buffers are empty, but keep in mind that our outbuf
		// may have something in it by now.
		assert(conn->inbuf == NULL);

		// initialise the data portion of the 'conn' object.
		assert(conn->rq);
		assert(conn->data == NULL);
		conn->data = (rq_data_t *) malloc(sizeof(rq_data_t));
		assert(conn->data);
		rq_data_init(conn->data);

		// apply the regular read handler now.
		assert(conn->read_event == NULL);
		assert(conn->handle > 0);
		conn->read_event = event_new(conn->rq->evbase, conn->handle, EV_READ | EV_PERSIST, rq_read_handler, conn);
		event_add(conn->read_event, NULL);
	
		// if we have data in our out buffer, we need to create the WRITE event.
		assert(conn->outbuf);
		if (BUF_LENGTH(conn->outbuf) > 0) {
			assert(conn->handle != INVALID_HANDLE && conn->handle > 0);
			assert(conn->write_event == NULL);
			assert(conn->rq->evbase);
			conn->write_event = event_new(conn->rq->evbase, conn->handle, EV_WRITE | EV_PERSIST, rq_write_handler, conn);
			event_add(conn->write_event, NULL);
		}
		
		// now that we have an active connection, we need to send our queue requests.
		assert(conn);
		assert(conn->rq);
		ll_start(&conn->rq->queues);
		while ((q = ll_next(&conn->rq->queues))) {
			rq_send_consume(conn, q);
		}
		ll_finish(&conn->rq->queues);
	
		// just in case there is some data there already.
		rq_process_read(conn);
	}
}



static void rq_read_handler(int fd, short int flags, void *arg)
{
	rq_conn_t *conn = (rq_conn_t *) arg;

	assert(fd >= 0);
	assert(flags != 0);
	assert(conn);
	assert(conn->rq);
	assert(conn->active > 0);
	assert(flags & EV_READ);
	
	rq_process_read(conn);
}





//-----------------------------------------------------------------------------
// add a controller to the end of the connection list.   If this is the first
// controller, then we need to attempt to connect to it, and setup the socket
// on the event queue.
void rq_addcontroller(
	rq_t *rq,
	char *host,
	void *connect_handler,
	void *dropped_handler,
	void *arg)
{
	
	// *(connect_handler)(rq_service_t *service, void *arg)
	
	rq_conn_t *conn;
	
	assert(rq != NULL);
	assert(host != NULL);
	assert(host[0] != 0);
	assert((arg != NULL && (connect_handler || dropped_handler)) || (arg == NULL));

	// dont currently have it coded to handle these, so we will fail for now
	// until we find a need to use them.
	assert(connect_handler == NULL);
	assert(dropped_handler == NULL);
	assert(arg == NULL);

	conn = calloc(1, sizeof(rq_conn_t));
	assert(conn);
	
	conn->hostname = strdup(host);

	conn->handle = INVALID_HANDLE;		// socket handle to the connected controller.
	assert(conn->read_event == NULL);
	assert(conn->write_event == NULL);
	assert(conn->connect_event == NULL);

	assert(conn->readbuf == NULL);
	assert(conn->inbuf == NULL);
	conn->outbuf = expbuf_init(NULL, 512);
	assert(conn->outbuf);

	assert(rq->risp);
	conn->risp = rq->risp;

	conn->rq = rq;
	conn->active = 0;
	conn->shutdown = 0;
	conn->closing = 0;
	conn->data = NULL;

	ll_push_tail(&rq->connlist, conn);

	// if this is the only controller we have so far, then we need to attempt the
	// connect (non-blocking)
	if (ll_count(&rq->connlist) == 1) {
		rq_connect(rq);
	}
}










//-----------------------------------------------------------------------------
// Send a request to the controller indicating a desire to consume a particular
// queue.  We will add queue information to our RQ structure.  If we are
// already connected to a controller, then the queue request will be sent
// straight away.  If not, then the request will be made as soon as a
// connection is made.  
void rq_consume(
	rq_t *rq,
	char *queue,
	int max,
	int priority,
	int exclusive,
	void (*handler)(rq_message_t *msg, void *arg),
	void (*accepted)(char *queue, queue_id_t qid, void *arg),
	void (*dropped)(char *queue, queue_id_t qid, void *arg),
	void *arg)
{
	int found;
	rq_queue_t *q;
	rq_conn_t *conn;
	
	assert(rq);
	assert(queue);
	assert(strlen(queue) < 256);
	assert(max >= 0);
	assert(priority == RQ_PRIORITY_NONE || priority == RQ_PRIORITY_LOW || priority == RQ_PRIORITY_NORMAL || priority == RQ_PRIORITY_HIGH);
	assert(handler);

	// check that we are connected to a controller.
	assert(ll_count(&rq->connlist) > 0);

	// check that we are not already consuming this queue.
	found = 0;
	ll_start(&rq->queues);
	q = ll_next(&rq->queues);
	while (q && found == 0) {
		if (strcmp(q->queue, queue) == 0) {
			// the queue is already in our list...
			found ++;
		}
		else {
			q = ll_next(&rq->queues);
		}
	}
	ll_finish(&rq->queues);

	if (found == 0) {
		q = (rq_queue_t *) malloc(sizeof(rq_queue_t));
		assert(q != NULL);

		rq_queue_init(q);
		q->queue = strdup(queue);
		q->handler = handler;
		q->accepted = accepted;
		q->dropped = dropped;
		q->arg = arg;
		q->exclusive = exclusive;
		q->max = max;
		q->priority = priority;

		ll_push_tail(&rq->queues, q);

		// check to see if the top connection is active.  If so, send the consume request.
		conn = ll_get_head(&rq->connlist);
		assert(conn);
		if (conn->active > 0 && conn->closing == 0) {
			rq_send_consume(conn, q);
		}
	}
}










static void cmdClear(void *ptr)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;
	assert(conn);
	assert(conn->data);
	
	conn->data->mask = 0;
	conn->data->flags = 0;
	
	conn->data->id = 0;
	conn->data->qid = 0;
	conn->data->timeout = 0;
	conn->data->priority = 0;

	expbuf_clear(conn->data->queue);

	if (conn->data->payload) {
		expbuf_clear(conn->data->payload);
	}
}


static void cmdPing(void *ptr)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;
	char buf;
	
	assert(conn);

	// since we only need to send one char, its a bit silly to get an expanding buffer for it... so we'll do this one slightly different.
	buf = RQ_CMD_PONG;

	rq_senddata(conn, &buf, 1);
}


static void cmdPong(void *ptr)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;
	assert(conn);

	// not sure what to do with this yet.  Havent built in the code to actually handle things if we dont get the pong back.
	assert(0);
}





static void cmdConsuming(void *ptr)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;
	rq_queue_t *q;
	char *queue;
	int qid;
	
	
	assert(conn);
	assert(conn->data);

	if (BIT_TEST(conn->data->mask, RQ_DATA_MASK_QUEUEID)  && BIT_TEST(conn->data->mask, RQ_DATA_MASK_QUEUE)) {
		queue = expbuf_string(conn->data->queue);
		qid = conn->data->qid;

		assert(queue);
		assert(qid > 0);
		assert(ll_count(&conn->rq->queues) > 0);
		
		ll_start(&conn->rq->queues);
		q = ll_next(&conn->rq->queues);
		while (q) {
			assert(q->queue);
			if (strcmp(q->queue, queue) == 0) {
				assert(q->qid == 0);
				q->qid = qid;
				
				// if we have an 'accepted' handler, then we need to call that too.
				if (q->accepted) {
					q->accepted(queue, qid, q->arg);
				}
				
				q = NULL;
			}
			else {
				q = ll_next(&conn->rq->queues);
			}
		}
		ll_finish(&conn->rq->queues);
	}
	else {
		// Not enough data.
		assert(0);
	}
}

static void cmdRequest(void *ptr)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;
	msg_id_t msgid;
	queue_id_t qid = 0;
	char *qname = NULL;
	rq_queue_t *tmp, *queue;
	rq_message_t *msg;
	
	assert(conn);
	assert(conn->data);
	
	if (BIT_TEST(conn->data->mask, RQ_DATA_MASK_ID) && BIT_TEST(conn->data->mask, RQ_DATA_MASK_PAYLOAD) && (BIT_TEST(conn->data->mask, RQ_DATA_MASK_QUEUEID) || BIT_TEST(conn->data->mask, RQ_DATA_MASK_QUEUE))) {

		// get message ID
		msgid = conn->data->id;
		assert(msgid >= 0);

		// get queue Id or queue name.
		if (BIT_TEST(conn->data->mask, RQ_DATA_MASK_QUEUEID))
			qid = conn->data->qid;
		if (BIT_TEST(conn->data->mask, RQ_DATA_MASK_QUEUE))
			qname = expbuf_string(conn->data->queue);
		assert((qname == NULL && qid > 0) || (qname && qid == 0));

		// find the queue to handle this request.
		// TODO: use a function call to do this.
		queue = NULL;
		ll_start(&conn->rq->queues);
		tmp = ll_next(&conn->rq->queues);
		while (tmp) {
			assert(tmp->qid > 0);
			assert(tmp->queue);
			if (qid == tmp->qid || strcmp(qname, tmp->queue) == 0) {
				queue = tmp;
				tmp = NULL;
			}
			else {
				tmp = ll_next(&conn->rq->queues);
			}
		}
		ll_finish(&conn->rq->queues);

		if (queue == NULL) {
			// we dont seem to be consuming that queue...
			assert(conn->sendbuf);
			assert(BUF_LENGTH(conn->sendbuf) == 0);
			addCmd(conn->sendbuf, RQ_CMD_CLEAR);
			addCmdLargeInt(conn->sendbuf, RQ_CMD_ID, (short int)msgid);
			addCmd(conn->sendbuf, RQ_CMD_UNDELIVERED);
			rq_senddata(conn, BUF_DATA(conn->sendbuf), BUF_LENGTH(conn->sendbuf));
			expbuf_clear(conn->sendbuf);
		}
		else {
			// send a delivery message back to the controller.
			assert(conn->sendbuf);
			assert(BUF_LENGTH(conn->sendbuf) == 0);
			addCmd(conn->sendbuf, RQ_CMD_CLEAR);
			addCmdLargeInt(conn->sendbuf, RQ_CMD_ID, (short int)msgid);
			addCmd(conn->sendbuf, RQ_CMD_DELIVERED);
			rq_senddata(conn, BUF_DATA(conn->sendbuf), BUF_LENGTH(conn->sendbuf));
			expbuf_clear(conn->sendbuf);

			// get a new message object from the pool.
			msg = rq_msg_new(conn->rq, conn);
			assert(msg);
			assert(msg->id >= 0);
			assert(msg->src_id == -1);
			assert(msg->state == rq_msgstate_new);

			// fill out the message details, and add it to the head of the messages list.
			assert(conn->data);
			assert(queue && msgid >= 0);
			msg->src_id = msgid;
			if (BIT_TEST(conn->data->flags, RQ_DATA_FLAG_NOREPLY)) {
				msg->noreply = 1;
			}

			// move the payload buffer to the message.
			assert(msg->data == NULL);
			assert(conn->data);
			assert(conn->data->payload);
			msg->data = conn->data->payload;
			conn->data->payload = NULL;

			msg->state = rq_msgstate_delivering;
			queue->handler(msg, queue->arg);

			// if the message was NOREPLY, then we dont need to reply, and we can clear the message.
			if (msg->noreply == 1) {
				rq_msg_clear(msg);
				msg = NULL;
			}
			else if (msg->state == rq_msgstate_replied) {
				// we already have replied to this message.  Dont need to add it to
				// the out-process, as that would already have been done.  So all we
				// need to do is clear the message and return it to the pool.
				rq_msg_clear(msg);
				msg = NULL;
			}
			else {
				// we called the handled, but it hasn't replied yet.  We will need to
				// wait until it calls rq_reply, which can clean up this message
				// object.
				msg->state = rq_msgstate_delivered;
			}			
		}
	}
	else {
		// we dont have the required data to handle a request.
		// TODO: This should be handled better.
		assert(0);
	}
}


//-----------------------------------------------------------------------------
// The controller will return a DELIVERED command when a message has been
// delivered to the consumer within the timeout period.  We will just mark it
// as delivered.  The delivery messages are more useful to the controllers the
// message passes through than the node that made the request.
static void cmdDelivered(void *ptr)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;
	msg_id_t id;
	rq_message_t *msg;
	
	assert(conn);
	assert(conn->data);
	
	if (BIT_TEST(conn->data->mask, RQ_DATA_MASK_ID)) {

		id = conn->data->id;
		assert(id >= 0);

		// make sure that the message exists.
		assert(conn->rq);
		assert(conn->rq->msg_list);
		assert(id >= 0 && id < conn->rq->msg_max);
		assert(conn->rq->msg_list[id]);
		msg = conn->rq->msg_list[id];

		// make sure that it was a SENT message, and not a consumed one.
		assert(msg->conn == NULL);
		assert(msg->state == rq_msgstate_new);
		msg->state = rq_msgstate_delivered;
	}
	else {
		// we received a DELIVERED command, but we didn't have the required data also.
		assert(0);
	}
}

//-----------------------------------------------------------------------------
// When the reply to a request comes in, we need to match it with the request
// that was sent, and then using that information call the callback functions
// with the payload.
static void cmdReply(void *ptr)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;
	msg_id_t msgid;
	rq_message_t *msg;
	
	assert(conn);
	assert(conn->data);
	
	if (BIT_TEST(conn->data->mask, RQ_DATA_MASK_ID) && BIT_TEST(conn->data->mask, RQ_DATA_MASK_PAYLOAD)) {

		// get message ID
		msgid = conn->data->id;
		assert(msgid >= 0);

		// make sure this msgid is potentially legit.
		assert(conn->rq);
		assert(conn->rq->msg_list);
		assert(conn->rq->msg_used > 0);
		assert(conn->rq->msg_max > 0);
		assert(msgid < conn->rq->msg_max);
		assert(msgid != conn->rq->msg_next);
		assert(conn->rq->msg_list[msgid]);

		msg = conn->rq->msg_list[msgid];
		assert(msg->id == msgid);
		assert(msg->src_id == -1);
		assert(msg->conn == NULL);
		assert(msg->state == rq_msgstate_delivered);

		// replace the data buffer in the message, with the data buffer received with the reply.
		assert(msg->data);
		assert(conn->data->payload);

		expbuf_clear(msg->data);
		msg->data = expbuf_free(msg->data);
		assert(msg->data == NULL);
		msg->data = conn->data->payload;
		conn->data->payload = NULL;

		// if we have a reply handler, then we should call it, with the payload information.
		if (msg->reply_handler) {
			msg->reply_handler(msg);
		}

		// clear the message, retrun it to the pool.
		rq_msg_clear(msg);
		msg = NULL;
	}
	else {
		// we dont have the required data to handle a request.
		// TODO: This should be handled better.
		assert(0);
	}
}



static void cmdBroadcast(void *ptr)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;
	assert(conn);
	assert(conn->data);

	assert(0);
}
	
// set the noreply flag.
static void cmdNoreply(void *ptr)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;

	assert(conn);
	assert(conn->data);
	BIT_SET(conn->data->flags, RQ_DATA_FLAG_NOREPLY);	
}

//-----------------------------------------------------------------------------
// By receiving the closing command from the controller, we should not get any
// more queue requests to consume.  Also as soon as there are no more messages
// waiting for replies, the controller will drop the connection.  Therefore,
// when this command is processed, we need to initiate a connection to an
// alternative controller that can receive requests.
static void cmdClosing(void *ptr)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;

	assert(conn);
	assert(conn->data);

	// mark the connectiong as 'closing'.
	assert(conn->closing == 0);
	conn->closing ++;

	// initialise the connection to the alternate controller.
	assert(conn->rq);
	rq_connect(conn->rq);
}
	
static void cmdServerFull(void *ptr)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;

	assert(conn);
	assert(conn->data);

	assert(0);
}
	
static void cmdID(void *ptr, risp_int_t value)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;
	
	assert(conn);
	assert(value >= 0 && value <= 0xffff);
	assert(conn->data);
	
	conn->data->id = value;
	BIT_SET(conn->data->mask, RQ_DATA_MASK_ID);
}
	
static void cmdQueueID(void *ptr, risp_int_t value)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;
	
	assert(conn);
	assert(value > 0 && value <= 0xffff);
	assert(conn->data);
	
	conn->data->qid = value;
	BIT_SET(conn->data->mask, RQ_DATA_MASK_QUEUEID);

}
	
static void cmdTimeout(void *ptr, risp_int_t value)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;
	
	assert(conn);
	assert(value > 0 && value <= 0xffff);
	assert(conn->data);
	
	conn->data->timeout = value;
	BIT_SET(conn->data->mask, RQ_DATA_MASK_TIMEOUT);
}
	
static void cmdPriority(void *ptr, risp_int_t value)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;

	assert(conn);
	assert(value > 0 && value <= 0xffff);
	assert(conn->data);
	
	conn->data->priority = value;
	BIT_SET(conn->data->mask, RQ_DATA_MASK_PRIORITY);
}
	
static void cmdPayload(void *ptr, risp_length_t length, risp_data_t *data)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;
	
	assert(conn && length > 0 && data);

	assert(conn->data);
	assert(conn->data->payload == NULL);
	if (conn->data->payload == NULL) {
		assert(conn->rq);
		conn->data->payload = expbuf_init(NULL, length);
		assert(conn->data->payload);
	}
 	expbuf_set(conn->data->payload, data, length);
	BIT_SET(conn->data->mask, RQ_DATA_MASK_PAYLOAD);
// 	fprintf(stderr, "Payload. len=%d\n", length);
}


static void cmdQueue(void *ptr, risp_length_t length, risp_data_t *data)
{
	rq_conn_t *conn = (rq_conn_t *) ptr;
	
 	assert(conn && length > 0 && data);

 	assert(conn->data);
 	assert(conn->data->queue);
 	expbuf_set(conn->data->queue, data, length);
 	BIT_SET(conn->data->mask, RQ_DATA_MASK_QUEUE);
}



// Initialise an RQ structure.  
void rq_init(rq_t *rq)
{
	assert(rq);

	rq->evbase = NULL;

	// setup the risp processor.
	rq->risp = risp_init(NULL);
	risp_add_command(rq->risp, RQ_CMD_CLEAR,        &cmdClear);
	risp_add_command(rq->risp, RQ_CMD_PING,         &cmdPing);
	risp_add_command(rq->risp, RQ_CMD_PONG,         &cmdPong);
	risp_add_command(rq->risp, RQ_CMD_REQUEST,      &cmdRequest);
	risp_add_command(rq->risp, RQ_CMD_REPLY,        &cmdReply);
	risp_add_command(rq->risp, RQ_CMD_DELIVERED,    &cmdDelivered);
	risp_add_command(rq->risp, RQ_CMD_BROADCAST,    &cmdBroadcast);
	risp_add_command(rq->risp, RQ_CMD_NOREPLY,      &cmdNoreply);
	risp_add_command(rq->risp, RQ_CMD_CLOSING,      &cmdClosing);
	risp_add_command(rq->risp, RQ_CMD_CONSUMING,    &cmdConsuming);
	risp_add_command(rq->risp, RQ_CMD_SERVER_FULL,  &cmdServerFull);
	risp_add_command(rq->risp, RQ_CMD_ID,           &cmdID);
	risp_add_command(rq->risp, RQ_CMD_QUEUEID,      &cmdQueueID);
	risp_add_command(rq->risp, RQ_CMD_TIMEOUT,      &cmdTimeout);
	risp_add_command(rq->risp, RQ_CMD_PRIORITY,     &cmdPriority);
	risp_add_command(rq->risp, RQ_CMD_QUEUE,        &cmdQueue);
	risp_add_command(rq->risp, RQ_CMD_PAYLOAD,      &cmdPayload);

	ll_init(&rq->connlist);
	ll_init(&rq->queues);

	// create an array of DEFAULT_MSG_ARRAY items;
	assert(DEFAULT_MSG_ARRAY > 0);
	rq->msg_list = (void **) malloc(sizeof(void *) * DEFAULT_MSG_ARRAY);
	assert(rq->msg_list);
	for (rq->msg_max = 0; rq->msg_max < DEFAULT_MSG_ARRAY; rq->msg_max ++) {
		rq->msg_list[rq->msg_max] = NULL;
	}
	assert(rq->msg_max == DEFAULT_MSG_ARRAY);
	rq->msg_used = 0;
	rq->msg_next = 0;

	rq->msg_pool = ll_init(NULL);
	assert(rq->msg_pool);
}




//-----------------------------------------------------------------------------
// Return a new message struct.  Will get one from the mempool if there are any
// available, otherwise it will create a new entry.  Cannot assume that
// anything left in the mempool has any valid data, so will initialise it as if
// it was a fresh allocation.  The message will need to be applied to the
// message list.  Even incoming messages need to be placed on the list in case
// it receives cancel commands for it.
rq_message_t * rq_msg_new(rq_t *rq, rq_conn_t *conn)
{
	rq_message_t *msg;
	int i;

	// need to get a message struct from the mempool.
	assert(rq);
	assert(rq->msg_pool);
	msg = ll_pop_head(rq->msg_pool);
	if (msg == NULL) {
		// there wasnt any messages available in the pool, so we need to create one.
		msg = (rq_message_t *) malloc(sizeof(rq_message_t));
	}

	assert(msg);
	msg->rq = rq;
	msg->queue = NULL;
	msg->id = -1;
	msg->src_id = -1;
	msg->broadcast = 0;
	msg->noreply = 0;
	msg->state = rq_msgstate_new;
	msg->conn = conn;
	msg->reply_handler = NULL;
	msg->fail_handler = NULL;
	msg->arg = NULL;

	// if we are supplied with a 'conn' it means we know which connection the
	// message came from, which means it is already fully formed, and we wont
	// need a data buffer, because the data is already in a buffer which we will
	// be assigned.   If conn is null, it means that we are building a message
	// and will therefore need a data buffer.
	if (conn) { msg->data = NULL; }
	else { msg->data = expbuf_init(NULL, 0); }

	// add the message to the message list.
	assert(rq->msg_list);
	assert(rq->msg_max > 0);
	assert(rq->msg_used >= 0 && rq->msg_used <= rq->msg_max);
	if (rq->msg_used < rq->msg_max) {
		// there has to be at least one empty slot in the list.
		if (rq->msg_next >= 0) {
			assert(rq->msg_next < rq->msg_max);
			assert(rq->msg_list[rq->msg_next] == NULL);
			rq->msg_list[rq->msg_next] = msg;
			msg->id = rq->msg_next;
			rq->msg_next = -1;
		}
		else {
			// we need to go through the list, to find the empty slot.
			for (i=0; i < rq->msg_max; i++) {
				if (rq->msg_list[i] == NULL) {
					rq->msg_list[i] = msg;
					msg->id = i;
					i = rq->msg_max;
				}
			}
		}
	}
	else {
		// the list is full, we need to expand it.
		rq->msg_list = (void **) realloc(rq->msg_list, sizeof(void *) * (rq->msg_max + 1));
		assert(rq->msg_list);
		rq->msg_list[rq->msg_max] = msg;
		msg->id = rq->msg_max;
		rq->msg_max ++;
	}

	assert(rq->msg_used >= 0);
	rq->msg_used ++;
	assert(rq->msg_used > 0 && rq->msg_used <= rq->msg_max);

	assert(msg);
	return(msg);
}

//-----------------------------------------------------------------------------
// clean up the resources used by the message so that it can be used again.  We
// will return the data buffer to the bufpool so that it can be used for
// payloads of future messages.  We will also return the message to the message
// pool.
void rq_msg_clear(rq_message_t *msg)
{
	assert(msg);

	// remove the message from the msg_list.
	assert(msg->rq);
	assert(msg->rq->msg_list);
	assert(msg->rq->msg_used > 0);
	assert(msg->id >= 0);
	assert(msg->id < msg->rq->msg_max);
	assert(msg->rq->msg_list[msg->id] == msg);
	msg->rq->msg_list[msg->id] = NULL;
	msg->rq->msg_next = msg->id;
	msg->rq->msg_used--;
	
	msg->id = -1;
	msg->broadcast = 0;
	msg->noreply = 0;
	msg->queue = NULL;
	msg->state = rq_msgstate_new;

	// clear the buffer, if we have one allocated.
	if (msg->data) {
		expbuf_clear(msg->data);
		msg->data = expbuf_free(msg->data);
		assert(msg->data == NULL);
	}
	
	// return the message to the msgpool.
	assert(msg->rq);
	assert(msg->rq->msg_pool);
	ll_push_head(msg->rq->msg_pool, msg);
}


void rq_msg_setqueue(rq_message_t *msg, const char *queue)
{
	assert(msg != NULL);
	assert(queue != NULL);
	assert(msg->queue == NULL);

	msg->queue = queue;
}


void rq_msg_setbroadcast(rq_message_t *msg)
{
	assert(msg != NULL);
	assert(msg->broadcast == 0);

	msg->broadcast = 1;
}

void rq_msg_setnoreply(rq_message_t *msg)
{
	assert(msg != NULL);
	assert(msg->noreply == 0);

	msg->noreply = 1;
}


//-----------------------------------------------------------------------------
// This function copies the data that is presented, into an expanding buffer
// that it controls.  It should be assumed that the 'data' field is empty when
// this function is called.
void rq_msg_setdata(rq_message_t *msg, int length, char *data)
{
	assert(msg);
	assert(length > 0);
	assert(data);
	assert(BUF_LENGTH(msg->data) == 0);
	
	expbuf_set(msg->data, data, length);
}



//-----------------------------------------------------------------------------
// send a message to the controller.   We dont need to worry about the
// mechanics of the actual send, that will be done through the rq_senddata
// function.
void rq_send(
	rq_message_t *msg,
	void (*reply_handler)(rq_message_t *reply),
	void (*fail_handler)(rq_message_t *msg),
	void *arg)
{
	rq_conn_t *conn;
	
	assert(msg);
	assert(msg->data);
	assert(BUF_LENGTH(msg->data) > 0);
	assert(msg->rq);
	assert(msg->id >= 0);
	assert(msg->conn == NULL);
	assert(msg->queue);
	assert(msg->src_id == -1);
	assert(msg->state == rq_msgstate_new);

	// if we are given an 'arg', then we should have at least one handler.
	assert((arg == NULL) || (arg && (reply_handler || fail_handler)));
	msg->reply_handler = reply_handler;
	msg->fail_handler = fail_handler;
	msg->arg = arg;

	// find an active connection to a controller, and send it.
	// otherwise, if we dont have any active connections, then we keep it in the
	// messages list, and send it out when we finally get a connection.
	conn = ll_get_head(&msg->rq->connlist);
	if (conn && conn->active > 0 && conn->closing == 0) {

		// get a buffer from the bufpool.
		assert(conn->sendbuf);
		assert(BUF_LENGTH(conn->sendbuf) == 0);
	
		// send consume request to controller.
		addCmd(conn->sendbuf, RQ_CMD_CLEAR);
		addCmdLargeInt(conn->sendbuf, RQ_CMD_ID, msg->id);
		addCmdShortStr(conn->sendbuf, RQ_CMD_QUEUE, strlen(msg->queue), (char *) msg->queue);
		addCmdLargeStr(conn->sendbuf, RQ_CMD_PAYLOAD, BUF_LENGTH(msg->data), BUF_DATA(msg->data));

		if (msg->noreply > 0) { addCmd(conn->sendbuf, RQ_CMD_NOREPLY); }
		if (msg->broadcast > 0) { addCmd(conn->sendbuf, RQ_CMD_BROADCAST); }
		else { addCmd(conn->sendbuf, RQ_CMD_REQUEST); }
	
		rq_senddata(conn, BUF_DATA(conn->sendbuf), BUF_LENGTH(conn->sendbuf));
		
		// return the buffer to the bufpool.
		expbuf_clear(conn->sendbuf);
	}
	else {
		// We need to put the message in a linked list so that we do send it in the right order.
		assert(0);
	}
}


//-----------------------------------------------------------------------------
// This function is used to send a reply for a request.  The data being sent
// back should be placed in the data buffer.   Reply needs to be sent on the
// same connection that it arrived on.   The data returned in the reply can
// actually be empty.  Once the reply is sent, there is absolutely no reason
// to keep it around.  If the connection got dropped, there is nothing we can
// do about it.  Therefore the reply is transient.
//
//	Note: Originally the reply functionality re-used the 'data' field which
//	contained the original payload.  However, it because apparent that we
//	couldn't re-use that buffer while we are in the middle of processing it.
//	Although it was assumed that by the time we needed to reply, the data
//	would have finished processing, it wasn't very safe.
void rq_reply(rq_message_t *msg, int length, char *data)
{
	assert(msg);
	assert((length == 0 && data == NULL) || (length > 0 && data));
	
	assert(msg->rq);
	assert(msg->conn);

	assert(msg->id >= 0);
	assert(msg->src_id >= 0);
	assert(msg->broadcast == 0);
	assert(msg->noreply == 0);
	assert(msg->queue == NULL);
	assert(msg->state == rq_msgstate_delivering || msg->state == rq_msgstate_delivered);

	assert(msg->data);

	// get the send buffer from rq.
	assert(msg->conn);
	assert(msg->conn->sendbuf);
	assert(BUF_LENGTH(msg->conn->sendbuf) == 0);
	addCmd(msg->conn->sendbuf, RQ_CMD_CLEAR);
	addCmdLargeInt(msg->conn->sendbuf, RQ_CMD_ID, (short int) msg->src_id);
	if (length > 0) {
		assert(data);
		addCmdLargeStr(msg->conn->sendbuf, RQ_CMD_PAYLOAD, length, data);
	}
	addCmd(msg->conn->sendbuf, RQ_CMD_REPLY);
	rq_senddata(msg->conn, BUF_DATA(msg->conn->sendbuf), BUF_LENGTH(msg->conn->sendbuf));
	expbuf_clear(msg->conn->sendbuf);

	// if this reply is being sent after the message was delivered to the handler,
	if (msg->state == rq_msgstate_delivered) {
		// then we need to clean up the message, because there is nothing else that will.
		rq_msg_clear(msg);
	}
	else {
		// then we must be replying straight away and we can let the code that called the handler clean it up.
		msg->state = rq_msgstate_replied;
	}
}


