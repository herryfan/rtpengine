#include "mix.h"
#include <glib.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/channel_layout.h>
#include <inttypes.h>
#include <libavresample/avresample.h>
#include <libavutil/opt.h>
#include "types.h"
#include "log.h"
#include "output.h"


#define NUM_INPUTS 4


struct mix_s {
	// format params
	int clockrate;
	int channels;

	AVFilterGraph *graph;
	AVFilterContext *src_ctxs[NUM_INPUTS];
	uint64_t pts_offs[NUM_INPUTS]; // initialized at first input seen
	uint64_t in_pts[NUM_INPUTS]; // running counter of next expected adjusted pts
	AVFilterContext *amix_ctx;
	AVFilterContext *sink_ctx;
	unsigned int next_idx;
	AVFrame *sink_frame;

	AVAudioResampleContext *avresample;
	AVFrame *swr_frame;
	int swr_buffers;
	uint64_t out_pts; // starting at zero

	AVFrame *silence_frame;
};


static void mix_shutdown(mix_t *mix) {
	if (mix->amix_ctx)
		avfilter_free(mix->amix_ctx);
	mix->amix_ctx = NULL;

	if (mix->sink_ctx)
		avfilter_free(mix->sink_ctx);
	mix->sink_ctx = NULL;

	for (int i = 0; i < NUM_INPUTS; i++) {
		if (mix->src_ctxs[i])
			avfilter_free(mix->src_ctxs[i]);
		mix->src_ctxs[i] = NULL;
	}

	avresample_free(&mix->avresample);
	avfilter_graph_free(&mix->graph);
}


void mix_destroy(mix_t *mix) {
	mix_shutdown(mix);
	av_frame_free(&mix->sink_frame);
	av_frame_free(&mix->swr_frame);
	av_frame_free(&mix->silence_frame);
	g_slice_free1(sizeof(*mix), mix);
}


unsigned int mix_get_index(mix_t *mix) {
	return mix->next_idx++;
}


int mix_config(mix_t *mix, unsigned int clockrate, unsigned int channels) {
	const char *err;
	char args[512];

	// anything to do?
	if (G_UNLIKELY(mix->clockrate != clockrate))
		goto format_mismatch;
	if (G_UNLIKELY(mix->channels != channels))
		goto format_mismatch;

	// all good
	return 0;

format_mismatch:
	mix_shutdown(mix);

	// copy params
	mix->clockrate = clockrate;
	mix->channels = channels;

	// filter graph
	err = "failed to alloc filter graph";
	mix->graph = avfilter_graph_alloc();
	if (!mix->graph)
		goto err;

	// amix
	err = "no amix filter available";
	AVFilter *flt = avfilter_get_by_name("amix");
	if (!flt)
		goto err;

	snprintf(args, sizeof(args), "inputs=%lu", (unsigned long) NUM_INPUTS);
	err = "failed to create amix filter context";
	if (avfilter_graph_create_filter(&mix->amix_ctx, flt, NULL, args, NULL, mix->graph))
		goto err;

	// inputs
	err = "no abuffer filter available";
	flt = avfilter_get_by_name("abuffer");
	if (!flt)
		goto err;

	for (int i = 0; i < NUM_INPUTS; i++) {
		dbg("init input ctx %i", i);

		snprintf(args, sizeof(args), "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:"
				"channel_layout=0x%" PRIx64,
				1, mix->clockrate, mix->clockrate,
				av_get_sample_fmt_name(AV_SAMPLE_FMT_S16),
				av_get_default_channel_layout(mix->channels));

		err = "failed to create abuffer filter context";
		if (avfilter_graph_create_filter(&mix->src_ctxs[i], flt, NULL, args, NULL, mix->graph))
			goto err;

		err = "failed to link abuffer to amix";
		if (avfilter_link(mix->src_ctxs[i], 0, mix->amix_ctx, i))
			goto err;
	}

	// sink
	err = "no abuffersink filter available";
	flt = avfilter_get_by_name("abuffersink");
	if (!flt)
		goto err;

	err = "failed to create abuffersink filter context";
	if (avfilter_graph_create_filter(&mix->sink_ctx, flt, NULL, NULL, NULL, mix->graph))
		goto err;

	err = "failed to link amix to abuffersink";
	if (avfilter_link(mix->amix_ctx, 0, mix->sink_ctx, 0))
		goto err;

	// finish up
	err = "failed to configure filter chain";
	if (avfilter_graph_config(mix->graph, NULL))
		goto err;

	return 0;

err:
	mix_shutdown(mix);
	ilog(LOG_ERR, "Failed to initialize mixer: %s", err);
	return -1;
}


mix_t *mix_new() {
	mix_t *mix = g_slice_alloc0(sizeof(*mix));
	mix->clockrate = -1;
	mix->channels = -1;
	mix->sink_frame = av_frame_alloc();

	for (int i = 0; i < NUM_INPUTS; i++)
		mix->pts_offs[i] = (uint64_t) -1LL;

	return mix;
}


static AVFrame *mix_resample_frame(mix_t *mix, AVFrame *frame) {
	const char *err;

	if (frame->format == AV_SAMPLE_FMT_S16)
		return frame;

	if (!mix->avresample) {
		mix->avresample = avresample_alloc_context();
		err = "failed to alloc resample context";
		if (!mix->avresample)
			goto err;

		av_opt_set_int(mix->avresample, "in_channel_layout",
				av_get_default_channel_layout(mix->channels), 0);
		av_opt_set_int(mix->avresample, "in_sample_fmt",
				frame->format, 0);
		av_opt_set_int(mix->avresample, "in_sample_rate",
				mix->clockrate, 0);
		av_opt_set_int(mix->avresample, "out_channel_layout",
				av_get_default_channel_layout(mix->channels), 0);
		av_opt_set_int(mix->avresample, "out_sample_fmt",
				AV_SAMPLE_FMT_S16, 0);
		av_opt_set_int(mix->avresample, "out_sample_rate",
				mix->clockrate, 0);
		// av_opt_set_int(dec->avresample, "internal_sample_fmt", AV_SAMPLE_FMT_FLTP, 0); // ?

		err = "failed to init resample context";
		if (avresample_open(mix->avresample) < 0)
			goto err;
	}

	// get a large enough buffer for resampled audio - this should be enough so we don't
	// have to loop
	int dst_samples = avresample_available(mix->avresample) +
		av_rescale_rnd(avresample_get_delay(mix->avresample) + frame->nb_samples,
				mix->clockrate, mix->clockrate, AV_ROUND_UP);
	if (!mix->swr_frame || mix->swr_buffers < dst_samples) {
		av_frame_free(&mix->swr_frame);
		mix->swr_frame = av_frame_alloc();
		err = "failed to alloc resampling frame";
		if (!mix->swr_frame)
			goto err;
		av_frame_copy_props(mix->swr_frame, frame);
		mix->swr_frame->format = frame->format;
		mix->swr_frame->channel_layout = frame->channel_layout;
		mix->swr_frame->nb_samples = dst_samples;
		mix->swr_frame->sample_rate = mix->clockrate;
		err = "failed to get resample buffers";
		if (av_frame_get_buffer(mix->swr_frame, 0) < 0)
			goto err;
		mix->swr_buffers = dst_samples;
	}

	mix->swr_frame->nb_samples = dst_samples;
	int ret_samples = avresample_convert(mix->avresample, mix->swr_frame->extended_data,
				mix->swr_frame->linesize[0], dst_samples,
				frame->extended_data,
				frame->linesize[0], frame->nb_samples);
	err = "failed to resample audio";
	if (ret_samples < 0)
		goto err;

	mix->swr_frame->nb_samples = ret_samples;
	mix->swr_frame->pts = av_rescale(frame->pts, mix->clockrate, mix->clockrate);
	return mix->swr_frame;

err:
	ilog(LOG_ERR, "Error resampling: %s", err);
	return NULL;
}


static void mix_silence_fill_idx_upto(mix_t *mix, unsigned int idx, uint64_t upto) {
	unsigned int silence_samples = mix->clockrate / 100;

	while (mix->in_pts[idx] < upto) {
		if (G_UNLIKELY(!mix->silence_frame)) {
			mix->silence_frame = av_frame_alloc();
			mix->silence_frame->format = AV_SAMPLE_FMT_S16;
			mix->silence_frame->channel_layout =
				av_get_default_channel_layout(mix->channels);
			mix->silence_frame->nb_samples = silence_samples;
			mix->silence_frame->sample_rate = mix->clockrate;
			if (av_frame_get_buffer(mix->silence_frame, 0) < 0) {
				ilog(LOG_ERR, "Failed to get silence frame buffers");
				return;
			}
			memset(mix->silence_frame->extended_data[0], 0, mix->silence_frame->linesize[0]);
		}

		dbg("pushing silence frame into stream %i (%lli < %llu)", idx,
				(long long unsigned) mix->in_pts[idx],
				(long long unsigned) upto);

		mix->silence_frame->pts = mix->in_pts[idx];
		mix->silence_frame->nb_samples = MIN(silence_samples, upto - mix->in_pts[idx]);
		mix->in_pts[idx] += mix->silence_frame->nb_samples;

		if (av_buffersrc_write_frame(mix->src_ctxs[idx], mix->silence_frame))
			ilog(LOG_WARN, "Failed to write silence frame to buffer");
	}
}


static void mix_silence_fill(mix_t *mix) {
	if (mix->out_pts < mix->clockrate)
		return;

	for (int i = 0; i < NUM_INPUTS; i++) {
		// check the pts of each input and give them max 1 second of delay.
		// if they fall behind too much, fill input with silence. otherwise
		// output stalls and won't produce media
		mix_silence_fill_idx_upto(mix, i, mix->out_pts - mix->clockrate);
	}
}


// frees the frame passed to it
int mix_add(mix_t *mix, AVFrame *frame, unsigned int idx, output_t *output) {
	const char *err;

	err = "index out of range";
	if (idx >= NUM_INPUTS)
		goto err;

	err = "mixer not initialized";
	if (!mix->src_ctxs[idx])
		goto err;

	dbg("stream %i pts_off %llu in pts %llu in frame pts %llu samples %u mix out pts %llu", 
			idx,
			(unsigned long long) mix->pts_offs[idx],
			(unsigned long long) mix->in_pts[idx],
			(unsigned long long) frame->pts,
			frame->nb_samples,
			(unsigned long long) mix->out_pts);

	// adjust for media started late
	if (G_UNLIKELY(mix->pts_offs[idx] == (uint64_t) -1LL))
		mix->pts_offs[idx] = mix->out_pts - frame->pts;
	frame->pts += mix->pts_offs[idx];

	// fill missing time
	mix_silence_fill_idx_upto(mix, idx, frame->pts);

	uint64_t next_pts = frame->pts + frame->nb_samples;

	err = "failed to add frame to mixer";
	if (av_buffersrc_add_frame(mix->src_ctxs[idx], frame))
		goto err;

	// update running counters
	if (next_pts > mix->out_pts)
		mix->out_pts = next_pts;
	if (next_pts > mix->in_pts[idx])
		mix->in_pts[idx] = next_pts;

	av_frame_free(&frame);

	mix_silence_fill(mix);

	while (1) {
		int ret = av_buffersink_get_frame(mix->sink_ctx, mix->sink_frame);
		err = "failed to get frame from mixer";
		if (ret < 0) {
			if (ret == AVERROR(EAGAIN))
				break;
			else
				goto err;
		}
		frame = mix_resample_frame(mix, mix->sink_frame);

		ret = output_add(output, frame);

		av_frame_unref(mix->sink_frame);

		if (ret)
			return -1;
	}

	return 0;

err:
	ilog(LOG_ERR, "Failed to add frame to mixer: %s", err);
	av_frame_free(&frame);
	return -1;
}
