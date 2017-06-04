#include "hls-media.h"
#include "hls-param.h"
#include "hls-h264.h"
#include "mpeg-ts.h"
#include "mpeg-ps.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define N_TS_PACKET 188
#define N_TS_FILESIZE (100 * 1024 * 1024) // 100M

#define VMAX(a, b) ((a) > (b) ? (a) : (b))

struct hls_media_t
{
	void* ts;
	uint8_t* ptr;
	size_t bytes;
	size_t capacity;
	size_t maxsize; // max bytes per ts file

	int64_t duration;	// user setting segment duration
	int64_t dts_last;	// last packet dts
	int64_t dts;		// segment first dts
	int64_t pts;		// segment first pts

	int audio_only_flag;// don't have video stream in segment

	hls_media_handler handler;
	void* param;
};

static void* hls_ts_alloc(void* param, size_t bytes)
{
	struct hls_media_t* hls;
	hls = (struct hls_media_t*)param;
	assert(188 == bytes);
	assert(hls->capacity >= hls->bytes);
	if (hls->capacity - hls->bytes < bytes)
	{
		void* p = realloc(hls->ptr, hls->capacity + bytes + N_TS_PACKET * 10 * 1024);
		if (NULL == p)
			return NULL;
		hls->ptr = p;
		hls->capacity += bytes + N_TS_PACKET * 10 * 1024;
	}
	return hls->ptr + hls->bytes;
}

static void hls_ts_free(void* param, void* packet)
{
	struct hls_media_t* hls;
	hls = (struct hls_media_t*)param;
	assert(packet == hls->ptr + hls->bytes - 188);
	assert(hls->ptr <= (uint8_t*)packet && hls->ptr + hls->capacity > (uint8_t*)packet);
}

static void hls_ts_write(void* param, const void* packet, size_t bytes)
{
	struct hls_media_t* hls;
	hls = (struct hls_media_t*)param;
	assert(188 == bytes);
	assert(hls->ptr <= (uint8_t*)packet && hls->ptr + hls->capacity > (uint8_t*)packet);
	hls->bytes += bytes; // update packet length
}

static void* hls_ts_create(struct hls_media_t* hls)
{
	struct mpeg_ts_func_t handler;
	handler.alloc = hls_ts_alloc;
	handler.write = hls_ts_write;
	handler.free = hls_ts_free;
	return mpeg_ts_create(&handler, hls);
}

void* hls_media_create(int64_t duration, hls_media_handler handler, void* param)
{
	struct hls_media_t* hls;
	hls = (struct hls_media_t*)malloc(sizeof(*hls));
	if (NULL == hls)
		return NULL;

	memset(hls, 0, sizeof(struct hls_media_t));
	hls->ts = hls_ts_create(hls);
	if (NULL == hls->ts)
	{
		free(hls);
		return NULL;
	}

	hls->maxsize = N_TS_FILESIZE;
	hls->dts = hls->pts = PTS_NO_VALUE;
	hls->dts_last = PTS_NO_VALUE;
	hls->duration = duration;
	hls->handler = handler;
	hls->param = param;
	return hls;
}

void hls_media_destroy(void* p)
{
	struct hls_media_t* hls;
	hls = (struct hls_media_t*)p;

	if (hls->ts)
		mpeg_ts_destroy(hls->ts);

	if (hls->ptr)
	{
		assert(hls->capacity > 0);
		free(hls->ptr);
	}

	free(hls);
}

static inline int hls_media_keyframe(int avtype, const void* data, size_t bytes)
{
	// TODO: check sps/pps???
	return STREAM_VIDEO_H264 == avtype && h264_idr((const uint8_t*)data, bytes);  // IDR-frame or audio only stream
}

int hls_media_input(void* p, int avtype, const void* data, size_t bytes, int64_t pts, int64_t dts, int force_new_segment)
{
	int segment;
	int64_t duration;
	struct hls_media_t* hls;
	hls = (struct hls_media_t*)p;

	assert(dts < hls->dts_last + hls->duration || PTS_NO_VALUE == hls->dts_last);

	// PTS/DTS rewind
	if (dts + hls->duration < hls->dts_last)
		force_new_segment = 1;

	// IDR frame
	// 1. check segment duration
	// 2. new segment per keyframe
	// 3. check segment file size
	if ((dts - hls->dts >= hls->duration || 0 == hls->duration)
		&& (hls_media_keyframe(avtype, data, bytes) || hls->bytes >= hls->maxsize) )
	{
		segment = 1;
	}
	else if (hls->audio_only_flag && dts - hls->dts >= hls->duration)
	{
		// audio only file
		segment = 1;
	}
	else
	{
		segment = 0;
	}

	if (0 == hls->bytes || segment || force_new_segment)
	{
		if (hls->bytes > 0)
		{
			duration = ((force_new_segment || dts > hls->dts_last + 100) ? hls->dts_last : dts) - hls->dts;
			hls->handler(hls->param, hls->ptr, hls->bytes, hls->pts, hls->dts, duration);

			// reset mpeg ts generator
			mpeg_ts_reset(hls->ts);
		}

		// new segment
		hls->pts = pts;
		hls->dts = dts;
		hls->bytes = 0;
		hls->audio_only_flag = 1;
	}

	if (STREAM_VIDEO_H264 == avtype && hls->audio_only_flag)
		hls->audio_only_flag = 0; // clear audio only flag

	hls->dts_last = dts;
	return mpeg_ts_write(hls->ts, avtype, pts * 90, dts * 90, data, bytes);
}
