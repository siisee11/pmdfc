#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/memory.h>
#include <linux/mm_types.h>
#include <linux/random.h>
#include <linux/kthread.h>
#include "tcp.h"

#define THREAD_NUM 4
#define TOTAL_CAPACITY (PAGE_SIZE * 256)
#define ITERATIONS (TOTAL_CAPACITY/PAGE_SIZE/THREAD_NUM)

void*** vpages;
atomic_t failedSearch;
struct completion* comp;
void** return_page;
uint64_t** keys;

struct thread_data{
	int tid;
	struct completion *comp;
};

struct task_struct** write_threads;
struct task_struct** read_threads;

int single_write_message_test(void* arg){
	struct thread_data* my_data = (struct thread_data*)arg;
	int ret, status;
	uint32_t key, index;
	char *test_string;

	test_string = kzalloc(PAGE_SIZE, GFP_KERNEL);
	strcpy(test_string, "hi, dicl");

	key = 4000;
	index = 1;

	ret = pmnet_send_message(PMNET_MSG_PUTPAGE, key, index, 
		test_string, PAGE_SIZE, 0, &status);

	complete(my_data->comp);

	printk("[ PASS ] %s succeeds \n", __func__);

	return 0;
}

int write_message_test(void* arg){
	struct thread_data* my_data = (struct thread_data*)arg;
	int tid = my_data->tid;
	int i, ret;
	int status;
	uint32_t index = 0;
	uint32_t key;

	for(i = 0; i < ITERATIONS; i++){
		key = (uint32_t)keys[tid][i];
		pr_info("write_message: %u\n", key);
		ret = pmnet_send_message(PMNET_MSG_PUTPAGE, key, index, 
			(char *)vpages[tid][i], PAGE_SIZE, 0, &status);
	}

	complete(my_data->comp);

	printk("[ PASS ] %s succeeds \n", __func__);

	return 0;
}


int single_read_message_test(void* arg){
	struct thread_data* my_data = (struct thread_data*)arg;
	int ret, status;
	int result = 0;
	uint32_t key = 4000, index = 1;
	char test_string[PAGE_SIZE] = "hi, dicl";
	void *result_page;

	result_page = (void*)kmalloc(PAGE_SIZE, GFP_KERNEL);

	ret = pmnet_send_recv_message(PMNET_MSG_GETPAGE, key, index, 
			result_page, PAGE_SIZE, 0, &status);

	if(memcmp(result_page, test_string, PAGE_SIZE) != 0){
		printk("[ FAIL ] Searching for key %x\n", key);
		result++;
	}

	if (result == 0)
		printk("[ PASS ] %s succeeds\n", __func__);

	complete(my_data->comp);
	return 0;
}

int read_message_test(void* arg){
	struct thread_data* my_data = (struct thread_data*)arg;
	int ret, i, status;
	int result = 0;
	uint32_t key, index = 0;
	int tid = my_data->tid;
	int nfailed = 0;

	for(i = 0; i < ITERATIONS; i++){
		key = keys[tid][i];
		ret = pmnet_send_recv_message(PMNET_MSG_GETPAGE, key, index, 
				return_page[tid], PAGE_SIZE, 0, &status);

		if(memcmp(return_page[tid], (char *)vpages[tid][i], PAGE_SIZE) != 0){
			printk("failed Searching for key %x\n return: %s\nexpect: %s", key, (char *)return_page[tid], (char *)vpages[tid][i]);
			nfailed++;
		}
	}

	if (nfailed == 0)
		printk("[ PASS ] %s succeeds\n", __func__);
	else
		printk("failedSearch: %d\n", nfailed);

	complete(my_data->comp);
	return 0;
}


int main(void){
	int i;
	ktime_t start, end;
	uint64_t elapsed;
	struct thread_data** args = (struct thread_data**)kmalloc(sizeof(struct thread_data*)*THREAD_NUM, GFP_KERNEL);

	for(i = 0; i < THREAD_NUM; i++){
		args[i] = (struct thread_data*)kmalloc(sizeof(struct thread_data), GFP_KERNEL);
		args[i]->tid = i;
		args[i]->comp = &comp[i];
	}

	pr_info("Start running write thread functions...\n");
	start = ktime_get();
	for(i=0; i<THREAD_NUM; i++){
		write_threads[i] = kthread_create((void*)&write_message_test, (void*)args[i], "page_writer");
//		write_threads[i] = kthread_create((void*)&single_write_message_test, (void*)args[i], "page_writer");
		wake_up_process(write_threads[i]);
	}

	for(i=0; i<THREAD_NUM; i++){
		wait_for_completion(&comp[i]);
	}
	end = ktime_get();
	elapsed = ((u64)ktime_to_ns(ktime_sub(end, start)) / 1000);
	pr_info("[ PASS ] complete write thread functions: time( %llu ) usec", elapsed);
	
	ssleep(3);

	for(i=0; i<THREAD_NUM; i++){
		reinit_completion(&comp[i]);
		args[i]->comp = &comp[i];
	}

	pr_info("Start running read thread functions...\n");
	start = ktime_get();

	for(i=0; i<THREAD_NUM; i++){
		read_threads[i] = kthread_create((void*)&read_message_test, (void*)args[i], "page_reader");
//		read_threads[i] = kthread_create((void*)&single_read_message_test, (void*)args[i], "page_reader");
		wake_up_process(read_threads[i]);
	}

	for(i=0; i<THREAD_NUM; i++){
		wait_for_completion(&comp[i]);
	}

	end = ktime_get();
	elapsed = ((u64)ktime_to_ns(ktime_sub(end, start)) / 1000);
	pr_info("[ PASS ] complete read thread functions: time( %llu ) usec", elapsed);

	for(i=0; i<THREAD_NUM; i++){
		kfree(args[i]);
	}
	kfree(args);

	return 0;
}

int init_pages(void){
	int i, j;
	uint64_t key = 0;
	u64 rand;
	char str[8];

	write_threads = (struct task_struct**)kmalloc(sizeof(struct task_struct*)*THREAD_NUM, GFP_KERNEL);
	if(!write_threads){
		printk(KERN_ALERT "write_threads allocation failed\n");
		goto ALLOC_ERR;
	}

	read_threads = (struct task_struct**)kmalloc(sizeof(struct task_struct*)*THREAD_NUM, GFP_KERNEL);
	if(!read_threads){
		printk(KERN_ALERT "read_threads allocation failed\n");
		goto ALLOC_ERR;
	}

	return_page = (void**)kmalloc(sizeof(void*)*THREAD_NUM, GFP_KERNEL);
	if(!return_page){
		printk(KERN_ALERT "return page allocation failed\n");
		goto ALLOC_ERR;
	}

	for(i=0; i<THREAD_NUM; i++){
		return_page[i] = (void*)kmalloc(PAGE_SIZE, GFP_KERNEL);
		if(!return_page[i]){
			printk(KERN_ALERT "return page[%d] allocation failed\n", i);
			goto ALLOC_ERR;
		}
	}

	vpages = (void***)kmalloc(sizeof(void**)*THREAD_NUM, GFP_KERNEL);
	if(!vpages){
		printk(KERN_ALERT "vpages allocation failed\n");
		goto ALLOC_ERR;
	}
	for(i=0; i<THREAD_NUM; i++){
		vpages[i] = (void**)vmalloc(sizeof(void*)*ITERATIONS);
		if(!vpages[i]){
			printk(KERN_ALERT "vpages[%d] allocation failed\n", i);
			goto ALLOC_ERR;
		}
		for(j=0; j<ITERATIONS; j++){
			vpages[i][j] = (void*)kmalloc(sizeof(char)*PAGE_SIZE, GFP_KERNEL);
			if(!vpages[i][j]){
				printk(KERN_ALERT "vpages[%d][%d] allocation failed\n", i, j);
				goto ALLOC_ERR;
			}
			sprintf(str, "%lld", key++ );
			strcpy(vpages[i][j], str);
		}
	}

	keys = (uint64_t**)kmalloc(sizeof(uint64_t*)*THREAD_NUM, GFP_KERNEL);
	key = 0;
	if(!keys){
		printk(KERN_ALERT "keys allocation failed\n");
		goto ALLOC_ERR;
	}
	for(i=0; i<THREAD_NUM; i++){
		keys[i] = (uint64_t*)kmalloc(sizeof(uint64_t)*ITERATIONS, GFP_KERNEL);
		for(j=0; j<ITERATIONS; j++){
			keys[i][j] = key++;
		}
	}

	comp = (struct completion*)kmalloc(sizeof(struct completion)*THREAD_NUM, GFP_KERNEL);
	if(!comp){
		printk(KERN_ALERT "Completion allocation failed\n");
		goto ALLOC_ERR;
	}

	for(i=0; i<THREAD_NUM; i++){
		init_completion(&comp[i]);
	}

	atomic_set(&failedSearch, 0);

	printk(KERN_INFO "[ PASS ] pmdfc initialization");
	return 0;


ALLOC_ERR:
	if(write_threads) 
		kfree(write_threads);
	if(read_threads) 
		kfree(read_threads);
	if(comp) 
		kfree(comp);

	for(i=0; i<THREAD_NUM; i++){
		if(return_page[i]) kfree(return_page[i]);
		for(j=0; j<ITERATIONS; j++){
			if(vpages[i][j]) kfree(vpages[i][j]);
		}
		if(vpages[i]) vfree(vpages[i]);
	}

	if(return_page) 
		kfree(return_page);
	if(vpages) 
		kfree(vpages);
	return 1;
}

void show_test_info(void){

	pr_info("+------------ PMDFC SERVER TEST INFO ------------+\n");
	pr_info("| NUMBER OF THREAD: %d  \t\t\t\t|\n", THREAD_NUM);
	pr_info("| TOTAL CAPACITY  : %ld \t\t\t|\n", TOTAL_CAPACITY);
	pr_info("| ITERATIONS      : %ld \t\t\t\t|\n", ITERATIONS);
	pr_info("+------------------------------------------------+\n");

	return;
}


static int __init init_test_module(void){
	int ret = 0;

	show_test_info();
	ssleep(3);

	ret = init_pages();
	if(ret){
		printk(KERN_ALERT "module initialization failed\n");
		return -1;
	}
	ssleep(3);

	ret = main();
	if(ret){
		printk(KERN_ALERT "module main function failed\n");
		return -1;
	}
	printk(KERN_INFO "run all the module functions\n");

	return 0;
}


static void __exit exit_test_module(void){
	int i, j;
	pr_info("[%s]: cleaning up resources", __func__);

	if(read_threads){
		kfree(read_threads);
		pr_info("[%s]: freed read threads", __func__);
	}
	if(write_threads){
		kfree(write_threads);
		pr_info("[%s]: freed write threads", __func__);
	}

	if(comp){
		kfree(comp);
		pr_info("[%s]: freed completion", __func__);
	}
	if(vpages){
		for(i=0; i<THREAD_NUM; i++){
			for(j=0; j<ITERATIONS; j++)
				if(vpages[i][j]) kfree(vpages[i][j]);
			if(vpages[i]) vfree(vpages[i]);
		}
		kfree(vpages);
		pr_info("[%s]: freed vpages", __func__);
	}
	if(return_page){
		for(i=0; i<THREAD_NUM; i++)
			if(return_page[i]) kfree(return_page[i]);
		kfree(return_page);
		pr_info("[%s]: freed return pages", __func__);
	}

	pr_info("[%s]: exiting test module", __func__);
}

module_init(init_test_module);
module_exit(exit_test_module);

MODULE_AUTHOR("Jaeyoun");
MODULE_LICENSE("GPL");
