/**
 * The Shadow Simulator
 *
 * Copyright (c) 2010-2011 Rob Jansen <jansen@cs.umn.edu>
 * Copyright (c) 2006-2009 Tyson Malchow <tyson.malchow@gmail.com>
 *
 * This file is part of Shadow.
 *
 * Shadow is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Shadow is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Shadow.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <mqueue.h>
#include <time.h>
#include <sys/sem.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>

#include "global.h"
#include "pipecloud.h"

pipecloud_tp pipecloud_create(unsigned int attendees, size_t size, unsigned int num_wakeup_channels) {
	pipecloud_tp pc;
	struct mq_attr attrs;
	char mqname[1024];

	pc = malloc(sizeof(*pc));

	if(!pc)
		printfault(EXIT_NOMEM, "pipecloud_create: Out of memory");

	pc->pipeqs = malloc(sizeof(pipecloud_queue_t) * attendees);

	attrs.mq_flags = 0;
	attrs.mq_maxmsg = 10;
	attrs.mq_msgsize = 8192;

	/* fixme error here if the OS cuts this size to something lower */
	pc->max_msg_size = 8192;
	pc->num_pipes = attendees;

	/* TODO - calling getpid here means that if children do not inherit the
	 * pipecloud, they will be unable to connect to the parent's message queues.
	 * ideally, we should pass in a pid to pipecloud_create, and use that instead.
	 *
	 * For now, children inherit the pipecloud so its not an issue.
	 */
	long int mypid = (long)getpid();

	/* Create the MQs */
	for(int i=0; i < attendees; i++) {
		pipecloud_queue_tp myq = &pc->pipeqs[i];

		snprintf(mqname, sizeof(mqname), "/shadow-pid%ld-mq%d", mypid, i);
		size_t name_len = strlen(mqname);
		myq->name = calloc(1, name_len);
		memmove(myq->name, mqname, name_len);

		myq->descriptor = mq_open(myq->name, O_RDWR | O_CREAT, 0777, &attrs);

		if(myq->descriptor == -1){
			perror("mq_open");
			printfault(EXIT_UNKNOWN, "pipecloud_create: Unable to open IPC message queues");
		}

		mq_setattr(myq->descriptor, &attrs, NULL);

		/* the following only works because the mesasge queues are inherited.
		 * unlinking deletes the name-to-queue mapping, so no one else will be
		 * able to connect. the queue will actually be deleted when all the
		 * descriptors are closed.
		 */
		if(mq_unlink(mqname) < 0) {
			perror("mq_unlink");
			printfault(EXIT_UNKNOWN, "pipecloud_create: Error unlinking successfully created message queue");
		}
	}

	pc->localized.id = 0;
	pc->localized.in = list_create();
	pc->localized.waiting_in = 0;

	return pc;
}

void pipecloud_destroy(pipecloud_tp pipecloud) {
	pipecloud_buffer_tp buf;

	/* clear out waiting incoming data */
	while((buf = list_pop_front(pipecloud->localized.in)) != NULL)
		free(buf);

	for(int i=0; i < pipecloud->num_pipes; i++) {
		pipecloud_queue_tp myq = &pipecloud->pipeqs[i];

//		if(pipecloud->localized.id == 0) {
//			if(mq_unlink(myq->name) < 0) {
//				perror("mq_unlink");
//				printfault(EXIT_UNKNOWN, "pipecloud_create: Error unlinking successfully created message queue");
//			}
//		}

		mq_close(myq->descriptor);
	}

	/* process 0 should destroy semaphores and the shm segment */
//	if(pipecloud->localized.id == 0) {
//		/* close pipes */
//		for(i = 0; i < pipecloud->num_wakeup_channels; i++) {
//			close(pipecloud->mboxes[i].wakeup_channel[0]);
//			close(pipecloud->mboxes[i].wakeup_channel[1]);
//		}
//
//		/* destroy all semaphores */
//		for(i = 0; i < pipecloud->num_pipes; i++)
//			ipcsync_destroy(pipecloud->mboxes[i].ipcsync);
//
//		/* mark shm for destruction */
//		shmctl(pipecloud->shm_id, IPC_RMID, NULL);
//	}
//
//	/* detach shm - will cause shm to get destroyed if we are process 0 and all other have deteached already*/
//	shmdt(pipecloud->shm);

	//if(pipecloud->localized.in)
	list_destroy(pipecloud->localized.in);
	free(pipecloud->pipeqs);
	free(pipecloud);
	return ;
}

int pipecloud_get_wakeup_fd(pipecloud_tp pc) {
	if(pc->localized.id < 0)
		return -1;
	pipecloud_queue_tp myq = &pc->pipeqs[pc->localized.id];
	return myq->descriptor;//pc->mboxes[pc->localized.id].wakeup_channel[0];
}

static void pipecloud_add_local_buffer(pipecloud_tp pipecloud, const char* message, size_t bytes) {
	pipecloud_buffer_tp buf = calloc(1, sizeof(*buf) + bytes);

	if(buf == NULL)
		printfault(EXIT_NOMEM, "pipecloud_localize_reads: Out of memory");

	buf->len = bytes;
	buf->offset = 0;

	memcpy(buf->data, message, bytes);

	list_push_back(pipecloud->localized.in, buf);
	pipecloud->localized.waiting_in += bytes;
}

/* should be a non-blocking attempt to read */
static void pipecloud_localize_reads(pipecloud_tp pipecloud) {
	struct timespec ts;
	ssize_t rv = 0;

	ts.tv_sec = 0;
	ts.tv_nsec = 0;

	pipecloud_queue_tp myq = &pipecloud->pipeqs[pipecloud->localized.id];

	struct mq_attr attrs;
	if(mq_getattr(myq->descriptor, &attrs) != 0) {
		perror("mq_getattr");
	}

	/* if there are waiting messages, we better be able to read one.*/
	if(attrs.mq_curmsgs > 0) {
		char* msgbuffer = calloc(1, pipecloud->max_msg_size);
		size_t total_bytes = 0;

		do {
			rv = mq_timedreceive(myq->descriptor, msgbuffer, pipecloud->max_msg_size, NULL, &ts);

			if(rv > 0) {
				size_t bytes = (size_t)rv;
				total_bytes += bytes;
				pipecloud_add_local_buffer(pipecloud, msgbuffer, bytes);
			}

			/* if we're here we should have had at least one message */
			assert(total_bytes > 0);
		} while(rv > 0);

		free(msgbuffer);
	}

	return;
}

void pipecloud_select(pipecloud_tp pipecloud, int block) {
	char* msgbuffer = calloc(1, pipecloud->max_msg_size);
	ssize_t rv;

	pipecloud_queue_tp myq = &pipecloud->pipeqs[pipecloud->localized.id];

	while(pipecloud->localized.waiting_in == 0 && block) {
		rv = mq_receive(myq->descriptor, msgbuffer, pipecloud->max_msg_size, NULL);

		if(rv > 0) {
			pipecloud_add_local_buffer(pipecloud, msgbuffer, (size_t)rv);
		}
	}

	pipecloud_localize_reads(pipecloud);

	free(msgbuffer);

	return;
}

void pipecloud_config_localized(pipecloud_tp pipecloud, unsigned int id) {
	pipecloud->localized.id = id;
}


size_t pipecloud_write(pipecloud_tp pipecloud, unsigned int dest, char * data, size_t data_size) {
	struct timespec ts;
	int rv;

	assert(dest < pipecloud->num_pipes);

	pipecloud_queue_tp destq = &pipecloud->pipeqs[dest];

	ts.tv_sec = PIPECLOUD_TIMEOUT_SEC;
	ts.tv_nsec = PIPECLOUD_TIMEOUT_NSEC;

	rv = mq_timedsend(destq->descriptor, data, data_size, 0, &ts);

	while(rv != 0) {
		/* first, try to pull in any waiting writes that we might have to avoid deadlocks */
		pipecloud_localize_reads(pipecloud);

		/* then, try to send again. */
		rv = mq_timedsend(destq->descriptor, data, data_size, 0, &ts);

		/* if it failed, there better not be space there! */
//		if(rv < 0) {
//			int theerror = errno;
//			struct mq_attr attrs;
//			if(mq_getattr(destq->descriptor, &attrs) != 0) {
//				perror("mq_getattr");
//			}
//			if(attrs.mq_maxmsg > attrs.mq_curmsgs) {
//				/* explode. something happened to our descriptor. did our
//				 * virtual descriptors loop around? */
//				printfault(EXIT_UNKNOWN, "pipecloud_write: fatal descriptor (mq_timedsend error %i) - timeout occurred even though there is space in the message queue!", theerror);
//			}
//		}
	}

	return data_size;
}

int pipecloud_read(pipecloud_tp pipecloud, char * out_buffer, size_t size) {
	size_t amt = 0, offset = 0, buf_avail = 0, rv = 0;
	pipecloud_buffer_tp buf = NULL;

	if(size == 0 || !out_buffer || size > pipecloud->localized.waiting_in)
		return 0;

	while(size) {
		buf = list_get_front(pipecloud->localized.in);

		if(buf != NULL) {
			/* how much to copy...*/
			buf_avail = buf->len - buf->offset;
			amt = size > buf_avail ? buf_avail : size;

			/* copy data */
			memcpy(out_buffer + offset, buf->data + buf->offset, amt);
			size -= amt;
			buf->offset += amt;
			offset += amt;
			rv += amt;
			pipecloud->localized.waiting_in -= amt;

			/* if this buffer has no more data, destroy it */
			if(buf->offset==buf->len) {
				list_pop_front(pipecloud->localized.in);
				free(buf);
			}
		}
	}

	return rv;
}
