/*
 * zerofree - a tool to zero free blocks in an ext2 filesystem
 *
 * Copyright (C) 2004-2012 R M Yorston
 *
 * This file may be redistributed under the terms of the GNU General Public
 * License, version 2.
 *
 * Changes:
 *
 * 2015-07-02  Multithread support.   Patch from Natale Galioto.
 * 2010-10-17  Allow non-zero fill value.   Patch from Jacob Nevins.
 * 2007-08-12  Allow use on filesystems mounted read-only.   Patch from
 *             Jan Krämer.
 */

#include <ext2fs/ext2fs.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define USAGE "usage: %s [-n] [-v] [-f fillval] [-t max_threads] filesystem\n"

pthread_mutex_t mutex;
pthread_cond_t job_enqueued;
pthread_cond_t job_accepted;
pthread_cond_t job_done;

volatile int quit = 0;
volatile int do_job = 0;
volatile unsigned long job_block_num; 
unsigned char *empty;
int blocksize = 0;
volatile int runningThreads = 0;
volatile unsigned int freeBlocks, modifiedBlocks;
volatile double percent;
volatile int old_percent;
unsigned int fillval = 0;
int verbose = 0;
int dryrun = 0;

void *worker_thread(void *arg)
{
    int i, ret;
    ext2_filsys fs = (ext2_filsys)arg;
	unsigned char *buf = (unsigned char *)malloc(fs->blocksize);
    
    pthread_mutex_lock(&mutex);
    while (1)
    {
        // Mutex hold
        while (!do_job)
            pthread_cond_wait(&job_enqueued, &mutex);
        if (quit)
            break;
        int blk = job_block_num;
        runningThreads++;
        do_job = 0;
        pthread_cond_signal(&job_accepted);
        pthread_mutex_unlock(&mutex);

        // Do job 
		if ( ext2fs_test_block_bitmap(fs->block_map, blk) ) {
            goto again;
		}

        pthread_mutex_lock(&mutex);
		++freeBlocks;
		percent = 100.0 * (double)freeBlocks/
					(double)fs->super->s_free_blocks_count;
        pthread_mutex_unlock(&mutex);

		if ( verbose && (int)(percent*10) != old_percent ) {
            pthread_mutex_lock(&mutex);
			fprintf(stderr, "\r%4.1f%%", percent);
            pthread_mutex_unlock(&mutex);
			old_percent = (int)(percent*10);
		}

		ret = io_channel_read_blk(fs->io, blk, 1, buf);
		if ( ret ) {
			fprintf(stderr, "error while reading block\n");
			goto finish;
		}

		for ( i=0; i < fs->blocksize; ++i ) {
			if ( buf[i] != fillval ) {
				break;
			}
		}

		if ( i == fs->blocksize ) {
            goto again;
		}

        pthread_mutex_lock(&mutex);
		++modifiedBlocks;
        pthread_mutex_unlock(&mutex);

		if ( !dryrun ) {
			ret = io_channel_write_blk(fs->io, blk, 1, empty);
			if ( ret ) {
				fprintf(stderr, "error while writing block\n");
                goto finish;
			}
		}
again:
        pthread_mutex_lock(&mutex);
        runningThreads--;
        pthread_cond_signal(&job_done);
    }

finish:
    runningThreads--;
    pthread_mutex_unlock(&mutex);
	return NULL;
}

int main(int argc, char **argv)
{
	errcode_t ret;
	int flags;
	int superblock = 0;
	int open_flags = EXT2_FLAG_RW;
    ext2_filsys fs = NULL;
	unsigned long blk;
	int i, c;
    int max_threads = 1;
    pthread_t *threads;

	while ( (c=getopt(argc, argv, "nvf:t:")) != -1 ) {
		switch (c) {
		case 'n' :
			dryrun = 1;
			break;
		case 'v' :
			verbose = 1;
			break;
		case 'f' :
			{
				char *endptr;
				fillval = strtol(optarg, &endptr, 0);
				if ( !*optarg || *endptr ) {
					fprintf(stderr, "%s: invalid argument to -f\n", argv[0]);
					return 1;
				} else if ( fillval > 0xFFu ) {
					fprintf(stderr, "%s: fill value must be 0-255\n", argv[0]);
					return 1;
				}
			}
			break;
        case 't' :
            {
				char *endptr;
				max_threads = strtol(optarg, &endptr, 0);
				if ( !*optarg || *endptr ) {
					fprintf(stderr, "%s: invalid argument to -t\n", argv[0]);
					return 1;
				} else if ( max_threads < 1 || max_threads > 1024*1024 ) {
					fprintf(stderr, "%s: thread count must be 1-256\n", argv[0]);
					return 1;
				}
            }
            break;
		default :
			fprintf(stderr, USAGE, argv[0]);
			return 1;
		}
	}

	if ( argc != optind+1 ) {
		fprintf(stderr, USAGE, argv[0]);
		return 1;
	}

    printf("Checking if filesystem is mounted...\n");
	ret = ext2fs_check_if_mounted(argv[optind], &flags);
	if ( ret ) {
		fprintf(stderr, "%s: failed to determine filesystem mount state  %s\n",
					argv[0], argv[optind]);
		return 1;
	}

	if ( (flags & EXT2_MF_MOUNTED) && !(flags & EXT2_MF_READONLY) ) {
		fprintf(stderr, "%s: filesystem %s is mounted rw\n",
					argv[0], argv[optind]);
		return 1;
	}

    printf("Opening filesystem...\n");
    ret = ext2fs_open(argv[optind], open_flags, superblock, blocksize,
							unix_io_manager, &fs);
	if ( ret ) {
		fprintf(stderr, "%s: failed to open filesystem %s\n",
					argv[0], argv[optind]);
		return 1;
	}

	empty = (unsigned char *)malloc(fs->blocksize);
	if ( empty == NULL) {
		fprintf(stderr, "%s: out of memory (surely not?)\n", argv[0]);
		return 1;
	}

	memset(empty, fillval, fs->blocksize);

    printf("Reading inode bitmap...\n");
    ret = ext2fs_read_inode_bitmap(fs);
	if ( ret ) {
		fprintf(stderr, "%s: error while reading inode bitmap\n", argv[0]);
		return 1;
	}

    printf("Starting %d threads...\n", max_threads);
    pthread_t *threads = (pthread_t*)malloc(max_threads * sizeof(pthread_t));
    pthread_cond_init(&job_enqueued, NULL);
    pthread_cond_init(&job_accepted, NULL);
    pthread_mutex_init(&mutex, NULL);
    for (i = 0; i < max_threads; i++)
        pthread_create(&threads[i], NULL, worker_thread, fs);

    printf("Reading block bitmap...\n");
	ret = ext2fs_read_block_bitmap(fs);
	if ( ret ) {
		fprintf(stderr, "%s: error while reading block bitmap\n", argv[0]);
		return 1;
	}

	freeBlocks = modifiedBlocks = 0;
	percent = 0.0;
	old_percent = -1;

    printf("Processing %d blocks...\n", fs->super->s_blocks_count);
	for ( blk=fs->super->s_first_data_block; blk < fs->super->s_blocks_count; blk++ ) {
        pthread_mutex_lock(&mutex);
        while (runningThreads >= max_threads)
            pthread_cond_wait(&job_done, &mutex);
        job_block_num = blk;
        do_job = 1;
        pthread_cond_signal(&job_enqueued);
        while (do_job)
            pthread_cond_wait(&job_accepted, &mutex);
        pthread_mutex_unlock(&mutex);
	}
    pthread_mutex_lock(&mutex);
    quit = 1;
    pthread_cond_broadcast(&job_enqueued);
    pthread_mutex_unlock(&mutex);
    for (i = 0; i < max_threads; i++) {
        if (threads[i] == (pthread_t)0)
            continue;
        pthread_join(threads[i], NULL);
    }

	if ( verbose ) {
		printf("\r%u/%u/%u\n", modifiedBlocks, freeBlocks, fs->super->s_blocks_count);
	}

	ret = ext2fs_close(fs);
	if ( ret ) {
		fprintf(stderr, "%s: error while closing filesystem\n", argv[0]);
		return 1;
	}

	return 0;
}
