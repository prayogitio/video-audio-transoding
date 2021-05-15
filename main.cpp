#include <bits/stdc++.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
}
using namespace std;
#define STREAM_PIX_FMT AV_PIX_FMT_YUV420P

static string output_video_codec = "libx265";
static string output_audio_codec = "aac";
static string codec_priv_key = "x265-params";
static string codec_priv_value = "keyint=60:min-keyint=60:scenecut=0";

struct StreamContext {
	AVFormatContext *avfc;
	AVStream *video_avs;
	AVStream *audio_avs;
	AVCodec *video_avc;
	AVCodec *audio_avc;
	AVCodecContext *video_avcc;
	AVCodecContext *audio_avcc;
	int video_stream_index;
	int audio_stream_index;
	string filename;
};

SwsContext *sws_ctx = NULL;
AVFrame* scaled_frame;
bool is_frame_captured = false;
int captured_frame_in_second = 1;

int open_decoder(AVStream *stream, AVCodec **avc, AVCodecContext **avcc)
{
	*avc = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!(*avc)) {
		av_log(NULL, AV_LOG_ERROR, "Failed to find codec\n");
		return -1;
	}
	*avcc = avcodec_alloc_context3(*avc);
	if (!(*avcc)) {
		av_log(NULL, AV_LOG_ERROR, "Failed to alloc memory for codec context\n");
		return -1;
	}
	if (avcodec_parameters_to_context(*avcc, stream->codecpar)) {
		av_log(NULL, AV_LOG_ERROR, "Failed to fill codec context\n");
		return -1;
	}
	if (avcodec_open2(*avcc, *avc, NULL) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to open decoder\n");
		return -1;
	}

	return 0;
}

int prepare_audio_and_video_decoder(StreamContext *decoder)
{
	for (int i = 0; i < decoder->avfc->nb_streams; ++i) {
		AVStream *stream = decoder->avfc->streams[i];
		if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO && stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
		if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			decoder->video_avs = stream;
			decoder->video_stream_index = i;
			if (open_decoder(stream, &decoder->video_avc, &decoder->video_avcc))
				return -1;
		} else {
			decoder->audio_avs = stream;
			decoder->audio_stream_index = i;
			if (open_decoder(stream, &decoder->audio_avc, &decoder->audio_avcc))
				return -1;
		}
	}

	return 0;
}

int open_input_file(string filename, StreamContext *decoder)
{
	decoder->avfc = avformat_alloc_context();
	if (!decoder->avfc) {
		av_log(NULL, AV_LOG_ERROR, "Failed to alloc memory for format context\n");
		return -1;
	}
	if (avformat_open_input(&decoder->avfc, filename.c_str(), NULL, NULL) != 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to open input file (%s)\n", filename.c_str());
		return -1;
	}
	if (avformat_find_stream_info(decoder->avfc, NULL) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to get stream info\n");
		return -1;
	}
	if (prepare_audio_and_video_decoder(decoder) != 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to prepare audio and video decoder\n");
		return -1;
	}

	return 0;
}

int prepare_video_encoder(StreamContext *encoder, StreamContext *decoder)
{
	AVRational input_framerate = av_guess_frame_rate(decoder->avfc, decoder->video_avs, NULL);
	fprintf(stdout, "Input video frame rate is %d / %d\n", input_framerate.num, input_framerate.den);

	encoder->video_avs = avformat_new_stream(encoder->avfc, NULL);
	encoder->video_avc = avcodec_find_encoder_by_name(output_video_codec.c_str());
	if (!encoder->video_avc) {
		av_log(NULL, AV_LOG_ERROR, "Failed to find codec %s\n", output_video_codec.c_str());
		return -1;
	}
	encoder->video_avcc = avcodec_alloc_context3(encoder->video_avc);
	if (!encoder->video_avcc) {
		av_log(NULL, AV_LOG_ERROR, "Failed to alloc memory for codec context\n");
		return -1;
	}
	av_opt_set(encoder->video_avcc->priv_data, "preset", "fast", 0);
	av_opt_set(encoder->video_avcc->priv_data, codec_priv_key.c_str(), codec_priv_value.c_str(), 0);
	encoder->video_avcc->codec_tag = 0;
	encoder->video_avcc->gop_size = 12;
	encoder->video_avcc->height = decoder->video_avcc->height;
	encoder->video_avcc->width = decoder->video_avcc->width;
	encoder->video_avcc->sample_aspect_ratio = decoder->video_avcc->sample_aspect_ratio;
	encoder->video_avcc->pix_fmt = STREAM_PIX_FMT;
	encoder->video_avcc->bit_rate = 2 * 100 * 1000;
	encoder->video_avcc->rc_buffer_size = 4 * 100 * 1000;
	encoder->video_avcc->rc_max_rate = 2 * 100 * 1000;
	encoder->video_avcc->rc_min_rate = 2.5 * 100 * 1000;
	encoder->video_avcc->framerate = input_framerate;
	encoder->video_avcc->time_base = av_inv_q(input_framerate);
	encoder->video_avs->time_base = encoder->video_avcc->time_base;

	avcodec_parameters_from_context(encoder->video_avs->codecpar, encoder->video_avcc);
	if (avcodec_open2(encoder->video_avcc, encoder->video_avc, NULL) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Could not open encoder\n");
		return -1;
	}

	return 0;
}

int prepare_audio_encoder(StreamContext *encoder, StreamContext *decoder)
{
	int sample_rate = decoder->audio_avcc->sample_rate;
	fprintf(stdout, "Input audio sample rate is %d\n", sample_rate);

	encoder->audio_avs = avformat_new_stream(encoder->avfc, NULL);
	encoder->audio_avc = avcodec_find_encoder_by_name(output_audio_codec.c_str());	
	if (!encoder->audio_avc) {
		av_log(NULL, AV_LOG_ERROR, "Failed to find codec (%s)\n", output_audio_codec.c_str());
		return -1;
	}
	encoder->audio_avcc = avcodec_alloc_context3(encoder->audio_avc);
	if (!encoder->audio_avcc) {
		av_log(NULL, AV_LOG_ERROR, "Failed to alloc memory for codec context\n");
		return -1;
	}

	int OUTPUT_CHANNELS = 2;
	int OUTPUT_BIT_RATE = 196000;
	encoder->audio_avcc->channels = OUTPUT_CHANNELS;
	encoder->audio_avcc->channel_layout = av_get_default_channel_layout(OUTPUT_CHANNELS);
	encoder->audio_avcc->sample_rate = sample_rate;
	encoder->audio_avcc->sample_fmt = encoder->audio_avc->sample_fmts[0];
	encoder->audio_avcc->bit_rate = OUTPUT_BIT_RATE;
	encoder->audio_avcc->time_base = (AVRational){1, sample_rate};
	encoder->audio_avcc->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
	encoder->audio_avs->time_base = encoder->audio_avcc->time_base;

	if (avcodec_open2(encoder->audio_avcc, encoder->audio_avc, NULL) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to open audio encoder\n");
		return -1;
	}
	avcodec_parameters_from_context(encoder->audio_avs->codecpar, encoder->audio_avcc);

	return 0;
}

int open_output_file(string filename, StreamContext *decoder, StreamContext *encoder)
{
	avformat_alloc_output_context2(&encoder->avfc, NULL, NULL, filename.c_str());
	if (!encoder->avfc) {
		av_log(NULL, AV_LOG_ERROR, "Failed to alloc memory for output context\n");
		return -1;
	}
	if (prepare_video_encoder(encoder, decoder) != 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to open video encoder\n");
		return -1;
	};
	if (prepare_audio_encoder(encoder, decoder) != 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to open audio encoder\n");
		return -1;
	};

	if (encoder->avfc->oformat->flags & AVFMT_GLOBALHEADER)
		encoder->video_avcc->flags |=  AV_CODEC_FLAG_GLOBAL_HEADER;
	if (!(encoder->avfc->oformat->flags & AVFMT_NOFILE)) {
		if (avio_open(&encoder->avfc->pb, encoder->filename.c_str(), AVIO_FLAG_WRITE) < 0) {
			av_log(NULL, AV_LOG_ERROR, "Failed to open output file (%s)\n", encoder->filename.c_str());
			return -1;
		}
	}
	if (avformat_write_header(encoder->avfc, NULL) < 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to write header to output file\n");
		return -1;
	}

	return 0;
}

int do_encode(StreamContext *decoder, StreamContext *encoder, AVFrame *input_frame, bool is_audio)
{
	AVPacket *output_packet = av_packet_alloc();
	if (!output_packet) {
		av_log(NULL, AV_LOG_ERROR, "Failed to allocate memory for output packet\n");
		return -1;
	}

	AVCodecContext *avcc = is_audio ? encoder->audio_avcc : encoder->video_avcc;
	AVStream *src_stream = is_audio ? decoder->audio_avs : decoder->video_avs;
	AVStream *dst_stream = is_audio ? encoder->audio_avs : encoder->video_avs;
	int response = avcodec_send_frame(avcc, input_frame);
	while (response >= 0) {
		response = avcodec_receive_packet(avcc, output_packet);
		if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
			break;
		} else if (response < 0) {
			av_log(NULL, AV_LOG_ERROR, "Failed receiving packet from encoder\n");
			return -1;
		}

		output_packet->stream_index = is_audio ? decoder->audio_stream_index : decoder->video_stream_index;
		av_packet_rescale_ts(output_packet, src_stream->time_base, dst_stream->time_base);

		response = av_interleaved_write_frame(encoder->avfc, output_packet);
		if (response != 0) {
			av_log(NULL, AV_LOG_ERROR, "Failed to write packet to output file\n");
			return -1;
		}
	}
	av_packet_unref(output_packet);
	av_packet_free(&output_packet);

	return 0;
}

int do_transcode(StreamContext *decoder, StreamContext *encoder, AVPacket *input_packet, AVFrame *input_frame, bool is_audio) {
	AVCodecContext *avcc = is_audio ? decoder->audio_avcc : decoder->video_avcc;
	AVStream *stream = decoder->avfc->streams[input_packet->stream_index];
	int response = avcodec_send_packet(avcc, input_packet);
	if (response < 0) {
		av_log(NULL, AV_LOG_ERROR, "Error sending packet to decoder\n");
		return -1;
	}

	while (response >= 0) {
		response = avcodec_receive_frame(avcc, input_frame);
		if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
			break;
		} else if (response < 0) {
			av_log(NULL, AV_LOG_ERROR, "Error receiving frame from decoder\n");
			return -1;
		}
		if (response >= 0) {
			fprintf(stdout, "%s frame pts is %" PRId64 "\n", is_audio ? "Audio" : "Video", input_frame->pts);
			fprintf(stdout, "%s timestamp of this frame is %lf\n", is_audio ? "Audio" : "Video", input_frame->pts * av_q2d(stream->time_base));
			if (!is_audio && !is_frame_captured && captured_frame_in_second == (int) input_frame->pts * av_q2d(stream->time_base)) {
				sws_scale(sws_ctx, input_frame->data, input_frame->linesize, 0, decoder->video_avcc->height, scaled_frame->data, scaled_frame->linesize);
				is_frame_captured = true;
			}
			if (do_encode(decoder, encoder, input_frame, is_audio))
				return -1;
		}
		av_frame_unref(input_frame);
	}

	return 0;
}

void save_captured_image()
{
	int ret;
    AVFrame *frame;
    AVPacket *pkt;

    AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        printf("Codec not found\n");
        return;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        printf("Could not allocate video codec context\n");
        return;
    }

    codec_ctx->bit_rate = 400000;
    codec_ctx->width = scaled_frame->width;
    codec_ctx->height = scaled_frame->height;
    codec_ctx->time_base= (AVRational){1,25};
    codec_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        printf("Could not open codec\n");
        return;
    }

    pkt = av_packet_alloc();
    pkt->data = NULL;
    pkt->size = 0;

	ret = avcodec_send_frame(codec_ctx, scaled_frame);
    if (ret < 0) {
        printf("Error encoding frame\n");
        return;
    }

	ret = avcodec_receive_packet(codec_ctx, pkt);
	if (ret < 0) {
        printf("Error receive encoded frame\n");
        return;
    }

	char jpeg_file_name[256] = "";
	sprintf(jpeg_file_name, "image-%06d.jpg", captured_frame_in_second);
	printf("Storing encoded frame to jpg file (%s)\n", jpeg_file_name);
	FILE* jpeg_file = fopen(jpeg_file_name, "wb");
	fwrite(pkt->data, 1, pkt->size, jpeg_file);
	fclose(jpeg_file);

	av_packet_unref(pkt);
	av_packet_free(&pkt);
	avcodec_free_context(&codec_ctx); codec_ctx = NULL;
}

int main(int argc, char const *argv[])
{
	if (argc < 3) {
		av_log(NULL, AV_LOG_ERROR, "Usage: %s <input filename> <output filename>\n", argv[0]);
		return -1;
	}

	if (argc == 4) {
		captured_frame_in_second = atoi(argv[3]);
	}

	StreamContext *decoder = new StreamContext();
	decoder->filename = argv[1];
	StreamContext *encoder = new StreamContext();
	encoder->filename = argv[2];

	av_register_all();

	// Open source file and open codec for decoding
	if (open_input_file(argv[1], decoder) != 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to open input file (%s)\n", decoder->filename.c_str());
		return -1;
	}

	av_dump_format(decoder->avfc, 0, decoder->filename.c_str(), 0);

	// Open destination file and open codec for encoding
	if (open_output_file(argv[2], decoder, encoder) != 0) {
		av_log(NULL, AV_LOG_ERROR, "Failed to open output file (%s)\n", encoder->filename.c_str());
		return -1;
	}

	// Initialize sws_ctx for scaling frame
	sws_ctx = sws_getContext(
		decoder->video_avcc->width,
		decoder->video_avcc->height,
		decoder->video_avcc->pix_fmt,
		encoder->video_avcc->width,
		encoder->video_avcc->height,
		AV_PIX_FMT_YUVJ420P,
		SWS_BILINEAR,
		nullptr,
		nullptr,
		nullptr
	);

	scaled_frame = av_frame_alloc();
	scaled_frame->format = AV_PIX_FMT_YUVJ420P;
	scaled_frame->width = encoder->video_avcc->width;
	scaled_frame->height = encoder->video_avcc->height;

	av_image_alloc(
		scaled_frame->data,
		scaled_frame->linesize,
		scaled_frame->width,
		scaled_frame->height,
		(AVPixelFormat) scaled_frame->format,
		32
	);

	// Transcode video and audio
	AVFrame *input_frame = av_frame_alloc();
	if (!input_frame) {
		av_log(NULL, AV_LOG_ERROR, "Failed to allocate memory for frame\n");
		return -1;
	}

	AVPacket *input_packet = av_packet_alloc();
	if (!input_packet) {
		av_log(NULL, AV_LOG_ERROR, "Failed to allocate memory for packet\n");
		return -1;
	}

	while (av_read_frame(decoder->avfc, input_packet) >= 0) {
		AVStream *stream = decoder->avfc->streams[input_packet->stream_index];
		if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO && stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
		if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			if (do_transcode(decoder, encoder, input_packet, input_frame, false) != 0) {
				av_log(NULL, AV_LOG_ERROR, "Failed when transcoding video\n");
				return -1;
			}
		} else {
			if (do_transcode(decoder, encoder, input_packet, input_frame, true) != 0) {
				av_log(NULL, AV_LOG_ERROR, "Failed when transcoding audio\n");
				return -1;
			}
		}
	}
	fprintf(stdout, "Decoder video stream time_base is %d / %d\n", decoder->avfc->streams[decoder->video_stream_index]->time_base.num, decoder->avfc->streams[decoder->video_stream_index]->time_base.den);
	fprintf(stdout, "Encoder video stream time_base is %d / %d\n", encoder->avfc->streams[decoder->video_stream_index]->time_base.num, encoder->avfc->streams[decoder->video_stream_index]->time_base.den);
	fprintf(stdout, "Decoder audio stream time_base is %d / %d\n", decoder->avfc->streams[decoder->audio_stream_index]->time_base.num, decoder->avfc->streams[decoder->audio_stream_index]->time_base.den);
	fprintf(stdout, "Encoder audio stream time_base is %d / %d\n", encoder->avfc->streams[decoder->audio_stream_index]->time_base.num, encoder->avfc->streams[decoder->audio_stream_index]->time_base.den);
	fprintf(stdout, "Video duration is %" PRId64 "\n", decoder->avfc->streams[decoder->video_stream_index]->duration);
	fprintf(stdout, "Video length is %lf\n", decoder->avfc->streams[decoder->video_stream_index]->duration * av_q2d(decoder->avfc->streams[decoder->video_stream_index]->time_base));
	fprintf(stdout, "Audio duration is %" PRId64 "\n", decoder->avfc->streams[decoder->audio_stream_index]->duration);
	fprintf(stdout, "Audio length is %lf\n", decoder->avfc->streams[decoder->audio_stream_index]->duration * av_q2d(decoder->avfc->streams[decoder->audio_stream_index]->time_base));

	av_write_trailer(encoder->avfc);

	// Save captured image
	int64_t video_duration = decoder->avfc->streams[decoder->video_stream_index]->duration;
	if (video_duration > captured_frame_in_second && scaled_frame->format == AV_PIX_FMT_YUVJ420P) {
		save_captured_image();
	}

	// Free memory
	if (input_frame != NULL) {
		av_frame_free(&input_frame);
		input_frame = NULL;
	}
	if (scaled_frame != NULL) {
		av_frame_free(&scaled_frame);
		scaled_frame = NULL;
	}
	if (input_packet != NULL) {
		av_packet_free(&input_packet);
		input_packet = NULL;
	}

	avformat_close_input(&decoder->avfc);

	avformat_free_context(decoder->avfc); decoder->avfc = NULL;
	avformat_free_context(encoder->avfc); encoder->avfc = NULL;

	avcodec_free_context(&decoder->video_avcc); decoder->video_avcc = NULL;
	avcodec_free_context(&decoder->audio_avcc); decoder->audio_avcc = NULL;

	free(decoder); decoder = NULL;
	free(encoder); encoder = NULL;

	sws_freeContext(sws_ctx);

	return 0;
}
