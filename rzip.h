/*
   Copyright (C) 2006-2010 Con Kolivas
   Copyright (C) 1998 Andrew Tridgell

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#define LRZIP_MAJOR_VERSION 0
#define LRZIP_MINOR_VERSION 5
#define LRZIP_MINOR_SUBVERSION 51

#define NUM_STREAMS 2
#define STREAM_BUFSIZE (1024 * 1024 * 10)

#include "config.h"

#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <bzlib.h>
#include <zlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/resource.h>
#include <netinet/in.h>

#include <sys/time.h>

#include <sys/mman.h>
#include <sys/syscall.h>

#include <lzo/lzoconf.h>
#include <lzo/lzo1x.h>

/* LZMA C Wrapper */
#include "lzma/C/LzmaLib.h"

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#include <fcntl.h>
#include <sys/stat.h>

#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif
#include <errno.h>
#include <sys/mman.h>

/* needed for CRC routines */
#include "lzma/C/7zCrc.h"

#ifndef uchar
#define uchar unsigned char
#endif

#ifndef int32
#if (SIZEOF_INT == 4)
#define int32 int
#elif (SIZEOF_LONG == 4)
#define int32 long
#elif (SIZEOF_SHORT == 4)
#define int32 short
#endif
#endif

#ifndef int16
#if (SIZEOF_INT == 2)
#define int16 int
#elif (SIZEOF_SHORT == 2)
#define int16 short
#endif
#endif

#ifndef uint32
#define uint32 unsigned int32
#endif

#ifndef uint16
#define uint16 unsigned int16
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b)? (a): (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b)? (a): (b))
#endif

#if !HAVE_STRERROR
extern char *sys_errlist[];
#define strerror(i) sys_errlist[i]
#endif

#ifndef HAVE_ERRNO_DECL
extern int errno;
#endif

#define likely(x)	__builtin_expect(!!(x), 1)
#define unlikely(x)	__builtin_expect(!!(x), 0)

typedef long long int i64;
typedef uint16_t u16;
typedef uint32_t u32;

#ifndef MAP_ANONYMOUS
 #define MAP_ANONYMOUS MAP_ANON
#endif

#if defined(NOTHREAD) || !defined(_SC_NPROCESSORS_ONLN)
 #define PROCESSORS (1)
#else
 #define PROCESSORS (sysconf(_SC_NPROCESSORS_ONLN))
#endif

#ifdef _SC_PAGE_SIZE
 #define PAGE_SIZE (sysconf(_SC_PAGE_SIZE))
#else
 #define PAGE_SIZE (4096)
#endif

void fatal(const char *format, ...);

#ifdef __APPLE__
 #include <sys/sysctl.h>
 #define fmemopen fake_fmemopen
 #define open_memstream fake_open_memstream
 #define memstream_update_buffer fake_open_memstream_update_buffer
 #define mremap fake_mremap
static inline i64 get_ram(void)
{
	int mib[2];
	size_t len;
	i64 *p, ramsize;

	mib[0] = CTL_HW;
	mib[1] = HW_MEMSIZE;
	sysctl(mib, 2, NULL, &len, NULL, 0);
	p = malloc(len);
	sysctl(mib, 2, p, &len, NULL, 0);
	ramsize = *p;

	return ramsize;
}

/* OSX doesn't support unnamed semaphores so we fake the threading by
 * serialising threads and ignore all the semaphore calls.
 */
static inline void init_sem(sem_t *sem __attribute__((unused)))
{
}

static inline void post_sem(sem_t *s __attribute__((unused)))
{
}

static inline void wait_sem(sem_t *s __attribute__((unused)))
{
}

static inline void destroy_sem(sem_t *s __attribute__((unused)))
{
}

static inline void create_pthread(pthread_t  * thread, pthread_attr_t * attr,
	void * (*start_routine)(void *), void *arg)
{
	if (pthread_create(thread, attr, start_routine, arg))
		fatal("pthread_create");
	if (pthread_join(*thread, NULL))
		fatal("pthread_join");
}

static inline void join_pthread(pthread_t th __attribute__((unused)), void **thread_return __attribute__((unused)))
{
}
#else /* __APPLE__ */
 #define memstream_update_buffer(A, B, C) (0)
static inline i64 get_ram(void)
{
	i64 ramsize;
	FILE *meminfo;
	char aux[256];
	char *ignore;

	ramsize = (i64)sysconf(_SC_PHYS_PAGES) * PAGE_SIZE;
	if (ramsize > 0)
		return ramsize;

	/* Workaround for uclibc which doesn't properly support sysconf */
	if(!(meminfo = fopen("/proc/meminfo", "r")))
		fatal("fopen\n");

	while(!feof(meminfo) && !fscanf(meminfo, "MemTotal: %Lu kB", &ramsize))
		ignore = fgets(aux, sizeof(aux), meminfo);
	if (fclose(meminfo) == -1)
		fatal("fclose");
	ramsize *= 1000;

	return ramsize;
}

static inline void init_sem(sem_t *sem)
{
	if (unlikely(sem_init(sem, 0, 0)))
		fatal("sem_init\n");
}

static inline void post_sem(sem_t *s)
{
retry:
	if (unlikely((sem_post(s)) == -1)) {
		if (errno == EINTR)
			goto retry;
		fatal("sem_post failed");
	}
}

static inline void wait_sem(sem_t *s)
{
retry:
	if (unlikely((sem_wait(s)) == -1)) {
		if (errno == EINTR)
			goto retry;
		fatal("sem_wait failed");
	}
}

static inline void destroy_sem(sem_t *s)
{
	if (unlikely(sem_destroy(s)))
		fatal("sem_destroy failed\n");
}

static inline void create_pthread(pthread_t  *thread, pthread_attr_t * attr,
	void * (*start_routine)(void *), void *arg)
{
	if (pthread_create(thread, attr, start_routine, arg))
		fatal("pthread_create");
}

static inline void join_pthread(pthread_t th, void **thread_return)
{
	if (pthread_join(th, thread_return))
		fatal("pthread_join");
}
#endif

#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
 #define mremap fake_mremap
#endif

#define FLAG_SHOW_PROGRESS 2
#define FLAG_KEEP_FILES 4
#define FLAG_TEST_ONLY 8
#define FLAG_FORCE_REPLACE 16
#define FLAG_DECOMPRESS 32
#define FLAG_NO_COMPRESS 64
#define FLAG_LZO_COMPRESS 128
#define FLAG_BZIP2_COMPRESS 256
#define FLAG_ZLIB_COMPRESS 512
#define FLAG_ZPAQ_COMPRESS 1024
#define FLAG_VERBOSITY 2048
#define FLAG_VERBOSITY_MAX 4096
#define FLAG_STDIN 8192
#define FLAG_STDOUT 16384
#define FLAG_INFO 32768
#define FLAG_MAXRAM 65536
#define FLAG_UNLIMITED 131072

#define FLAG_VERBOSE (FLAG_VERBOSITY | FLAG_VERBOSITY_MAX)
#define FLAG_NOT_LZMA (FLAG_NO_COMPRESS | FLAG_LZO_COMPRESS | FLAG_BZIP2_COMPRESS | FLAG_ZLIB_COMPRESS | FLAG_ZPAQ_COMPRESS)
#define LZMA_COMPRESS	(!(control.flags & FLAG_NOT_LZMA))

#define SHOW_PROGRESS	(control.flags & FLAG_SHOW_PROGRESS)
#define KEEP_FILES	(control.flags & FLAG_KEEP_FILES)
#define TEST_ONLY	(control.flags & FLAG_TEST_ONLY)
#define FORCE_REPLACE	(control.flags & FLAG_FORCE_REPLACE)
#define DECOMPRESS	(control.flags & FLAG_DECOMPRESS)
#define NO_COMPRESS	(control.flags & FLAG_NO_COMPRESS)
#define LZO_COMPRESS	(control.flags & FLAG_LZO_COMPRESS)
#define BZIP2_COMPRESS	(control.flags & FLAG_BZIP2_COMPRESS)
#define ZLIB_COMPRESS	(control.flags & FLAG_ZLIB_COMPRESS)
#define ZPAQ_COMPRESS	(control.flags & FLAG_ZPAQ_COMPRESS)
#define VERBOSE		(control.flags & FLAG_VERBOSE)
#define VERBOSITY	(control.flags & FLAG_VERBOSITY)
#define MAX_VERBOSE	(control.flags & FLAG_VERBOSITY_MAX)
#define STDIN		(control.flags & FLAG_STDIN)
#define STDOUT		(control.flags & FLAG_STDOUT)
#define INFO		(control.flags & FLAG_INFO)
#define MAXRAM		(control.flags & FLAG_MAXRAM)
#define UNLIMITED	(control.flags & FLAG_UNLIMITED)

#define BITS32		(sizeof(long) == 4)

#define CTYPE_NONE 3
#define CTYPE_BZIP2 4
#define CTYPE_LZO 5
#define CTYPE_LZMA 6
#define CTYPE_GZIP 7
#define CTYPE_ZPAQ 8

struct rzip_control {
	char *infile;
	char *outname;
	char *outfile;
	char *outdir;
	FILE *msgout; //stream for output messages
	const char *suffix;
	int compression_level;
	unsigned char lzma_properties[5]; // lzma properties, encoded
	double threshold;
	i64 window;
	unsigned long flags;
	i64 ramsize;
	i64 max_chunk;
	i64 max_mmap;
	int threads;
	int nice_val;		// added for consistency
	int major_version;
	int minor_version;
	i64 st_size;
	long page_size;
	int fd_out;
} control;

struct stream {
	i64 last_head;
	uchar *buf;
	i64 buflen;
	i64 bufp;
	int eos;
	long uthread_no;
	long unext_thread;
	long base_thread;
	int total_threads;
};

struct stream_info {
	struct stream *s;
	int num_streams;
	int fd;
	i64 bufsize;
	i64 cur_pos;
	i64 initial_pos;
	i64 total_read;
	long thread_no;
	long next_thread;
	int chunks;
	char chunk_bytes;
};

void sighandler();
i64 runzip_fd(int fd_in, int fd_out, int fd_hist, i64 expected_size);
void rzip_fd(int fd_in, int fd_out);
void *open_stream_out(int f, int n, i64 limit, char cbytes);
void *open_stream_in(int f, int n);
int write_stream(void *ss, int stream, uchar *p, i64 len);
i64 read_stream(void *ss, int stream, uchar *p, i64 len);
int close_stream_out(void *ss);
int close_stream_in(void *ss);
void flush_buffer(struct stream_info *sinfo, int stream);
void read_config(struct rzip_control *s);
ssize_t write_1g(int fd, void *buf, i64 len);
ssize_t read_1g(int fd, void *buf, i64 len);
void zpipe_compress(FILE *in, FILE *out, FILE *msgout, long long int buf_len, int progress, long thread);
void zpipe_decompress(FILE *in, FILE *out, FILE *msgout, long long int buf_len, int progress, long thread);
const i64 two_gig;
void prepare_streamout_threads(void);
void close_streamout_threads(void);

#define print_err(format, args...)	do {\
	fprintf(stderr, format, ##args);	\
} while (0)

#define print_output(format, args...)	do {\
	fprintf(control.msgout, format, ##args);	\
	fflush(control.msgout);	\
} while (0)

#define print_progress(format, args...)	do {\
	if (SHOW_PROGRESS)	\
		print_output(format, ##args);	\
} while (0)

#define print_verbose(format, args...)	do {\
	if (VERBOSE)	\
		print_output(format, ##args);	\
} while (0)

#define print_maxverbose(format, args...)	do {\
	if (MAX_VERBOSE)	\
		print_output(format, ##args);	\
} while (0)
