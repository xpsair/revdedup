/**
 * @file 	restore.c
 * @brief	Implements restoration of images without reverse deduplication
 * @author	Ng Chun Ho
 */

#include <revdedup.h>
#include <queue.h>
#include <datatable.h>
#include "minilzo.h"

/**
 * Definition of data info for all threads
 */
typedef struct {
	int ifd;					/*!< Direct recipe file descriptor */
	int ofd;					/*!< File descriptor for sending image data */
	Direct * dir;				/*!< Direct recipe entries */
	uint64_t size;				/*!< Size of direct recipe */
	uint64_t cnt;				/*!< Number of direct recipe entries */
	volatile uint64_t cur;		/*!< Direct entry currently processed */
	pthread_spinlock_t lock;	/*!< Lock for modifying cur */
} DataInfo;

IMEntry * ien;
SMEntry * sen;
CMEntry * cen;
BMEntry * ben;

/** Data Info for all threads */
DataInfo dinfo;
/** Data Table for holding decompressed data that will be later sending out */
DataTable * dt;
/** Queue for holding memory blocks for decompression */
Queue * dq;

/**
 * Segment prefetching routine
 * @param ptr		Queue that holds either segment or bucket number
 */
void * prefetch(void * ptr) {
	Queue * q = (Queue *) ptr;
	uint64_t sid, lsid = 0, bid = 0;
	uint32_t pos, len;
	char buf[128];
	while ((sid = (uint64_t) Dequeue(q)) != 0) {
#ifdef PREFETCH_WHOLE_BUCKET
        bid = sen[sid].bucket;
        sprintf(buf, DATA_DIR "bucket/%08lx", bid);
        int fd = open(buf, O_RDONLY);
        posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
        close(fd);
	}
#else
		if (bid == 0) {
			bid = sen[sid].bucket;
			pos = sen[sid].pos;
			len = sen[sid].len;
			continue;
		}
		if (sen[sid].bucket == bid && pos + len == sen[sid].pos) {
			len += sen[sid].len;
			continue;
		}

		sprintf(buf, DATA_DIR "bucket/%08lx", bid);
		int fd = open(buf, O_RDONLY);
		posix_fadvise(fd, pos, len, POSIX_FADV_WILLNEED);
		close(fd);

		bid = sen[sid].bucket;
		pos = sen[sid].pos;
		len = sen[sid].len;

	}
	sprintf(buf, DATA_DIR "bucket/%08lx", bid);
	int fd = open(buf, O_RDONLY);
	posix_fadvise(fd, pos, len, POSIX_FADV_WILLNEED);
	close(fd);
#endif
	return NULL;
}

/**
 * Decompression routine
 * @param ptr		useless
 */
void * decompress(void * ptr) {
	uint8_t * cdata = MMAP_MM(MAX_SEG_SIZE);
	SMEntry * en;
	DataEntry * den;
	uint64_t size, cur;
	int32_t fd;
	char buf[128];

	while (1) {
		pthread_spin_lock(&dinfo.lock);
		cur = dinfo.cur++;
		pthread_spin_unlock(&dinfo.lock);
		if (unlikely(cur >= dinfo.cnt)) {
			break;
		}
		/// If it is locked, then other threads have decompressed this segment
		if (pthread_spin_trylock(&dt->en[dinfo.dir[cur].id].lock)) {
			continue;
		}
		en = &sen[dinfo.dir[cur].id];
		den = &dt->en[dinfo.dir[cur].id];
		den->data = Dequeue(dq);
		sprintf(buf, DATA_DIR "bucket/%08lx", en->bucket);
		fd = open(buf, O_RDONLY);
#ifdef DISABLE_COMPRESSION
		assert(pread(fd, den->data, en->len, en->pos) == en->len);
		den->size = en->len;
#else
		if (sen->compressed) {
			assert(pread(fd, cdata, en->len, en->pos) == en->len);
			lzo1x_decompress(cdata, en->len, den->data, &size, NULL);
			den->size = size;
		} else {
			assert(pread(fd, den->data, en->len, en->pos) == en->len);
			den->size = en->len;
		}
#endif
		close(fd);
		pthread_mutex_unlock(&den->mutex);
	}

	munmap(cdata, MAX_SEG_SIZE);
	return NULL;
}

/**
 * Routine for sending out data
 * @param ptr	useless
 */
void * send(void * ptr) {
	uint8_t * zero = MMAP_MM(ZERO_SIZE);
	SMEntry * en;
	DataEntry * den;
	uint64_t i, j;
	for (i = 0; i < dinfo.cnt; i++) {
		en = &sen[dinfo.dir[i].id];
		den = &dt->en[dinfo.dir[i].id];

		/// Ensure that the segment is fully decompressed
		pthread_mutex_lock(&den->mutex);
		pthread_mutex_unlock(&den->mutex);

		for (j = 0; j < en->chunks; j++) {
			CMEntry * cptr = &cen[en->cid + j];
			if (cptr->len == 0) {
				assert(write(dinfo.ofd, zero, ZERO_SIZE));
				continue;
			}
			assert(write(dinfo.ofd, den->data + cptr->pos, cptr->len) == cptr->len);
		}

		if (--den->cnt == 0) {
			Enqueue(dq, den->data);
		}
	}
	munmap(zero, ZERO_SIZE);
	return NULL;
}

int main(int argc, char * argv[]) {
	if (argc != 4) {
		fprintf(stderr, "Usage: %s instance version file\n", argv[0]);
		return 0;
	}
	char buf[128];
	uint32_t i;
	int32_t fd;


	uint32_t ins = atoi(argv[1]);
	uint32_t ver = atoi(argv[2]);
	/// Check whether the image is reversely deduplicated
	sprintf(buf, DATA_DIR "image/i%u-%u", ins, ver);
	if (access(buf, F_OK) == 0) {
		fprintf(stderr, "This version is get revdeduped, try using restoreo\n");
		return 0;
	}

	/// Retrieving Metadata
	fd = open(DATA_DIR "ilog", O_RDONLY);
	ien = MMAP_FD_RO(fd, INST_MAX(sizeof(IMEntry)));
	close(fd);

	fd = open(DATA_DIR "slog", O_RDONLY);
	sen = MMAP_FD_RO(fd, MAX_ENTRIES(sizeof(SMEntry)));
	close(fd);

	fd = open(DATA_DIR "clog", O_RDONLY);
	cen = MMAP_FD_RO(fd, MAX_ENTRIES(sizeof(CMEntry)));
	close(fd);

	fd = open(DATA_DIR "blog", O_RDONLY);
	ben = MMAP_FD_RO(fd, MAX_ENTRIES(sizeof(BMEntry)));
	close(fd);

	/// Setup DataInfo
	sprintf(buf, DATA_DIR "image/%u-%u", ins, ver);
	dinfo.ifd = open(buf, O_RDONLY);
	dinfo.ofd = creat(argv[3], 0644);
	dinfo.size = lseek(dinfo.ifd, 0, SEEK_END);
	dinfo.dir = MMAP_FD_RO(dinfo.ifd, dinfo.size);
	dinfo.cnt = dinfo.size / sizeof(Direct);
	dinfo.cur = 0;
	pthread_spin_init(&dinfo.lock, PTHREAD_PROCESS_SHARED);

	/// Setup memory buffer for decompression
	void * data = MMAP_MM(LONGQUEUE_LENGTH * MAX_SEG_SIZE);
	dq = LongQueue();
	for (i = 0; i < LONGQUEUE_LENGTH; i++) {
		Enqueue(dq, data + i * MAX_SEG_SIZE);
	}

	/// Setup DataTable and prefetch
	dt = NewDataTable(((SegmentLog *)sen)->segID + 1);
	Queue * pfq = SuperQueue();
	pthread_t pft;
	pthread_create(&pft, NULL, prefetch, pfq);
	for (i = 0; i < dinfo.cnt; i++) {
		DataEntry * den = &dt->en[dinfo.dir[i].id];
		if (++den->cnt == 1) {
			pthread_spin_init(&den->lock, PTHREAD_PROCESS_SHARED);
			pthread_mutex_lock(&den->mutex);
			Enqueue(pfq, (void *) dinfo.dir[i].id);
		}
	}
	Enqueue(pfq, NULL);

	/// Setup decompress and send
	pthread_t dct[DPS_CNT];
	pthread_t sdt;
	for (i = 0; i < DPS_CNT; i++) {
		pthread_create(dct + i, NULL, decompress, NULL);
	}
	pthread_create(&sdt, NULL, send, NULL);

	/// wait for the outstanding threads
	for (i = 0; i < DPS_CNT; i++) {
		pthread_join(dct[i], NULL);
	}
	pthread_join(sdt, NULL);
	pthread_cancel(pft);

	DelQueue(pfq);
	DelQueue(dq);
	munmap(data, LONGQUEUE_LENGTH * MAX_SEG_SIZE);

	DelDataTable(dt);

	munmap(ien, INST_MAX(sizeof(IMEntry)));
	munmap(sen, MAX_ENTRIES(sizeof(SMEntry)));
	munmap(cen, MAX_ENTRIES(sizeof(CMEntry)));
	munmap(ben, MAX_ENTRIES(sizeof(BMEntry)));
	munmap(dinfo.dir, dinfo.size);
	close(dinfo.ifd);
	close(dinfo.ofd);
	return 0;
}
