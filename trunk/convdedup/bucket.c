/*
 * bucket.c
 *
 *  Created on: May 29, 2013
 *      Author: chng
 */

#include "bucket.h"
#include "fingerprint.h"
#include "index.h"

static BucketService service;

static Bucket * NewBucket(uint64_t cid) {
	char buf[64];
	Bucket * b = malloc(sizeof(Bucket));
	b->id = ++service._log->bucketID;
	b->cid = cid;
	b->chunks = 0;
	b->size = 0;

	sprintf(buf, DATA_DIR "bucket/%08lx", b->id);
	b->fd = open(buf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(b->fd != -1);
	return b;
}

static void SaveBucket(Bucket * b) {
	ssize_t remain = (BLOCK_SIZE - (b->size % BLOCK_SIZE)) % BLOCK_SIZE;
	assert(write(b->fd, service._padding, remain) == remain);
	close(b->fd);

	service._en[b->id].cid = b->cid;
	service._en[b->id].chunks = b->chunks;
	service._en[b->id].size = b->size + remain;
	service._en[b->id].psize = 0;
	service._en[b->id].rsize = 0;
	free(b);
}

static Bucket * BucketInsert(Bucket * b, Chunk * ch) {
	if (b == NULL) {
		b = NewBucket(ch->id);
	}
	if (b->size + ch->clen > BUCKET_SIZE) {
		SaveBucket(b);
		b = NewBucket(ch->id);
	}
	ch->pos = b->size;
	GetIndexService()->setChunk(ch, b->id);
	assert(write(b->fd, ch->cdata, ch->clen) == ch->clen);
	b->chunks++;
	b->size += ch->clen;

	return b;
}


static void * process(void * ptr) {
	Bucket * b = NULL;
	Chunk * ch;
	int turn = 0;
	while ((ch = (Chunk *) Dequeue(service._iq)) != NULL) {
		if (ch->unique) {
			b = BucketInsert(b, ch);
		}
		Enqueue(service._oq, ch);
	}
	if (b != NULL) {
		SaveBucket(b);
	}
	Enqueue(service._oq, NULL);
	return NULL;
}

static int start(Queue * iq, Queue * oq) {
	int ret, fd;
	service._iq = iq;
	service._oq = oq;

	/* Load Bucket Log */
	fd = open(DATA_DIR "blog", O_RDWR | O_CREAT, 0644);
	assert(!ftruncate(fd, MAX_ENTRIES * sizeof(BMEntry)));
	service._en = MMAP_FD(fd, MAX_ENTRIES * sizeof(BMEntry));
	service._log = (BucketLog *) service._en;
	close(fd);

	memset(service._padding, 0, BLOCK_SIZE);

	ret = pthread_create(&service._tid, NULL, process, NULL);
	return ret;
}

static int stop() {
	int ret;
	ret = pthread_join(service._tid, NULL);
	munmap(service._en, MAX_ENTRIES * sizeof(BMEntry));
	return ret;
}

static BucketService service = {
		.start = start,
		.stop = stop,
};

BucketService* GetBucketService() {
	return &service;
}