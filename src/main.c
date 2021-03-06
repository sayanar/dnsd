#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

#include "common.h"
#include "req_queue.h"
#include "protocol.h"
#include "hhrt.h"
#include "util.h"
#include "black_list.h"

#define WORKER_NUMBER	3
#define RECV_BUFFER_SIZE 1024

// globals
int g_skt;
struct sockaddr_in g_saddr;

void create_shm(const char *name, unsigned size);
void init_shm()
{
	// create shared memory for req queue
    create_shm(SHM_RQ_NAME, sizeof(struct req_queue));

	// create shared memory for hhrt
    create_shm(SHM_HHRT_NAME, sizeof(struct hhrt_table));

	// create shared memory for blist
    create_shm(SHM_BLIST_NAME, sizeof(struct black_list));
}

void create_shm(const char *name, unsigned size)
{
	int shmid, is_new = 0;

	// create shared memory for req queue
	if((shmid = shm_open(name, O_RDWR, 0666)) < 0){
        is_new = 1; // doesn't exist, create new 
        if((shmid = shm_open(name, O_CREAT | O_RDWR, 0666)) < 0){
            perror("shm_open");
            exit(1);
        }
	}
    if(is_new){
        if(ftruncate(shmid, size) < 0){
            perror("ftruncate");
            exit(1);
        }
    }
}

// auxiliary function to get shared memory
void get_shm_by_name(void **p, const char *name, int size)
{
	int shmid;
	if((shmid = shm_open(name, O_RDWR, 0666)) < 0){
		perror("shm_open");
		exit(1);
	}
	if(((*p) = mmap(NULL, size, PROT_WRITE, MAP_SHARED, shmid, 0)) == (void *)(-1)){
		perror("mmap");
		exit(1);
	}
}

void init_socket()
{
	memset((void *)&g_saddr, 0, sizeof(g_saddr));
	g_saddr.sin_family = AF_INET;
	g_saddr.sin_port = htons(DNS_PORT);
	g_saddr.sin_addr.s_addr = INADDR_ANY;

	if((g_skt = socket(AF_INET, SOCK_DGRAM, 0)) == -1){
		perror("socket");
		exit(1);
	}
	
	if(bind(g_skt, (struct sockaddr *)&g_saddr, sizeof(struct sockaddr)) != 0){
		perror("bind");
		exit(1);
	}
}

// auxiliary function for get semaphore
void get_sem_by_name(sem_t **sem, const char *name)
{
	if(((*sem) = sem_open(name, 0)) == SEM_FAILED){
		perror("sem_open");
		exit(1);
	}
}

void init_sem()
{
	sem_t *sem_empty; 
	sem_t *sem_full;
	sem_t *sem_mutex;
	if((sem_empty = sem_open(SEM_EMPTY, O_CREAT, 0666, REQ_QUEUE_SIZE)) == SEM_FAILED){
		perror("sem_open");
		exit(1);
	}
	if((sem_full = sem_open(SEM_FULL, O_CREAT, 0666, 0)) == SEM_FAILED){
		perror("sem_open");
		exit(1);
	}
	if((sem_mutex = sem_open(SEM_MUTEX, O_CREAT, 0666, 1)) == SEM_FAILED){
		perror("sem_open");
		exit(1);
	}
}

int init_req_queue()
{
	struct req_queue *queue;

	get_shm_by_name((void **)&queue, SHM_RQ_NAME, sizeof(struct req_queue));
	queue->in = queue->out = 0;
	return 0;
}

int init_hhrt()
{
	struct hhrt_table *hhrt;

	get_shm_by_name((void **)&hhrt, SHM_HHRT_NAME, sizeof(struct hhrt_table));
	hhrt->pos = 0;
	return 0;
}

int init_blist(const char *filename)
{
	struct black_list *blist;
	FILE *fp = NULL;
	static char domain[NAME_SIZE];

	get_shm_by_name((void **)&blist, SHM_BLIST_NAME, sizeof(struct black_list));
	blist_init(blist);

	if((fp = fopen(filename, "r")) == NULL){
		perror("fopen");
		exit(1);
	}
	while(fscanf(fp, "%s", domain) == 1){
		blist_insert(blist, domain);
	}
	fclose(fp);
	return 0;
}

void *recver_entry(void *arg)
{
	static uint8_t buffer[RECV_BUFFER_SIZE];
	struct sockaddr_in c_addr;
	int tmp, len;
	struct req_queue *queue;
	socklen_t fromlen;
	sem_t *sem_empty, *sem_full;

	// debug
	printf("recver running...\n");

	// init semaphore
	get_sem_by_name(&sem_empty, SEM_EMPTY);
	get_sem_by_name(&sem_full, SEM_FULL);

	// init shared memory
	get_shm_by_name((void **)&queue, SHM_RQ_NAME, sizeof(struct req_queue));

	while(1){
		// recv request
		fromlen = sizeof(struct sockaddr);
		len = recvfrom(g_skt, buffer, RECV_BUFFER_SIZE, 0, (struct sockaddr *)&c_addr, &fromlen);
		
		if((tmp = verify_packet(buffer, len)) < 0){
			printf("verify failed with code: %d\n", tmp);
			continue;
		}

		// put the buffer and client addr into req_queue
		sem_wait(sem_empty);
			en_queue(queue, &c_addr, buffer, len);
		sem_post(sem_full);
	}
    return NULL;
}

void *worker_entry(void *arg)
{
	static struct req_wrapper wrapper;
	static struct hhrt_item hh_req;
	static struct sockaddr_in ns_addr;
	int hh_id, old_id;
	struct req_queue *queue;
	struct hhrt_table *hhrt;
	struct black_list *blist;
	sem_t *sem_empty, *sem_full, *sem_mutex;
	int worker_number = *((int *)arg);
	static uint8_t buffer[UDP_MSG_SIZE];
	static char domain[NAME_SIZE];
	int len;

	// debug
	printf("worker #%d running...\n", worker_number);

	// init NS addr
	memset((void *)&ns_addr, 0, sizeof(ns_addr));
	ns_addr.sin_family = AF_INET;
	ns_addr.sin_port = htons(DNS_PORT);
	inet_pton(AF_INET, HIT_NS_SERVER, &ns_addr.sin_addr.s_addr);

	// init semaphore
	get_sem_by_name(&sem_empty, SEM_EMPTY);
	get_sem_by_name(&sem_full, SEM_FULL);
	get_sem_by_name(&sem_mutex, SEM_MUTEX);

	// init shared memory
	get_shm_by_name((void **)&queue, SHM_RQ_NAME, sizeof(struct req_queue));
	get_shm_by_name((void **)&hhrt, SHM_HHRT_NAME, sizeof(struct hhrt_table));
	get_shm_by_name((void **)&blist, SHM_BLIST_NAME, sizeof(struct black_list));

	while(1){
		sem_wait(sem_full);
			sem_wait(sem_mutex);
				de_queue(queue, &wrapper);		
				hh_id = gen_hhrt_id(hhrt);
			sem_post(sem_mutex);
		sem_post(sem_empty);

		if(msg_is_req(wrapper.buffer, wrapper.len)){
			// msg is a request
			old_id = get_msg_id(wrapper.buffer, wrapper.len);
			get_msg_domain(wrapper.buffer, wrapper.len, domain);
			// debug
			printf("request for: %s, id=%d\n", domain, old_id);
			// lookup black list
			if(blist_lookup(blist, domain) >= 0){
				// debug
				printf("%s BLOCKED!\n", domain);
				len = make_error_resp(buffer, UDP_MSG_SIZE);
				set_msg_id(buffer, len, old_id);

				if(sendto(g_skt, buffer, len, 0, (struct sockaddr *)(&wrapper.clnt_addr), sizeof(struct sockaddr)) != len){
					perror("sendto");
				}
				continue;
			}

			insert_hhrt(hhrt, hh_id, old_id, &(wrapper.clnt_addr));
			set_msg_id(wrapper.buffer, wrapper.len, hh_id);
			// send to NS
			if(sendto(g_skt, wrapper.buffer, wrapper.len, 0, (struct sockaddr *)&ns_addr, sizeof(struct sockaddr)) != wrapper.len){
				perror("sendto");
				printf("to ns wrapper.len = %d\n", wrapper.len);
				continue;
			}
		} else {
			// msg is a response
			hh_id = get_msg_id(wrapper.buffer, wrapper.len);
			lookup_hhrt(hhrt, hh_id, &hh_req);
			set_msg_id(wrapper.buffer, wrapper.len, hh_req.old_id);

			// debug
			printf("response from NS, old_id = %d, new_id = %d\n", hh_req.old_id, hh_id);
			// send to client
			if(sendto(g_skt, wrapper.buffer, wrapper.len, 0, (struct sockaddr *)&(hh_req.clnt_addr), sizeof(struct sockaddr)) != wrapper.len){
				perror("sendto");
				printf("to clnt wrapper.len = %d\n", wrapper.len);
				continue;
			}
		}
	}
    return NULL;
}

pthread_t recv_thread;
pthread_t worker_threads[WORKER_NUMBER];

void clean_up(int sig)
{
    if(sig == SIGINT){
        printf("bye.\n");
    }
}

int main()
{
	int worker_numbers[WORKER_NUMBER];
	int i;

	init_socket();
	init_shm();
	init_sem();
	init_req_queue();
	init_hhrt();
	init_blist("black.list");
	
	pthread_create(&recv_thread, NULL, recver_entry, NULL);
	for(i = 0; i < WORKER_NUMBER; i ++){
		worker_numbers[i] = i;
		pthread_create(&worker_threads[i], NULL, worker_entry, &worker_numbers[i]);
	}

	pthread_join(recv_thread, NULL);
	for(i = 0; i < WORKER_NUMBER; i ++){
		pthread_join(worker_threads[i], NULL);
	}
	return 0;
}
