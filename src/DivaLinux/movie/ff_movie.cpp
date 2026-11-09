// The movie decoding core: see ff_movie.hpp.

#include "ff_movie.hpp"

std::string MovieDecoder::StatsLine()
{
	char buf[512];
	double wall = playStartMs > 0.0 ? (FFNowMs() - playStartMs) / 1000.0 : 0.0;
	double reduced = reducedSeconds + (reducedQuality && reducedSinceMs > 0.0 ? (FFNowMs() - reducedSinceMs) / 1000.0 : 0.0);
	snprintf(buf, sizeof(buf),
		"%s; %.1f s played, %ld decoded (%.1f fps), decode %.2f ms/frame, %ld converted on the GPU, "
		"%ld on the CPU at %.2f ms/frame, worst lag %.2f s, reduced quality %.1f s, keyframe jumps %ld, "
		"conversions skipped %ld",
		videoInfo.c_str(), wall, framesDecoded, wall > 0.0 ? framesDecoded / wall : 0.0,
		framesDecoded ? decodeMs / framesDecoded : 0.0, framesGpu, framesConverted,
		framesConverted ? convertMs / framesConverted : 0.0,
		maxBehind, reduced, (long)keyframeJumps, (long)framesSkipped);
	return buf;
}

MovieDecoder::~MovieDecoder()
{
	Stop();
	for (auto& f : queue)
	{
		free(f.pixels);
		if (f.yuv)
			ff.av_frame_free(&f.yuv);
	}
	while (fifoCount > 0)
		PopPacket();
	if (pendingVideo)
		ff.av_packet_free(&pendingVideo);
	if (frame) ff.av_frame_free(&frame);
	if (audioFrame) ff.av_frame_free(&audioFrame);
	if (packet) ff.av_packet_free(&packet);
	if (sws) ff.sws_freeContext(sws);
	if (swr) ff.swr_free(&swr);
	free(audioBuffer);
	if (videoCodec) ff.avcodec_free_context(&videoCodec);
	if (audioCodec) ff.avcodec_free_context(&audioCodec);
	if (format) ff.avformat_close_input(&format);
	CloseCustomIO();
}

bool MovieDecoder::Open(const char* utf8Path)
{
	int r;
	if (fileIO)
	{
		// FFmpeg reads through fileIO; the name is only a hint for probing the format
		ioFile = fileIO->open(utf8Path);
		if (!ioFile)
			return Fail("not found (opened through DivaLinux's file layer)", 0);
		const int IO_BUFFER = 256 * 1024;
		uint8_t* buffer = (uint8_t*)ff.av_malloc(IO_BUFFER);
		if (buffer)
			io = ff.avio_alloc_context(buffer, IO_BUFFER, 0, this, &MovieDecoder::IORead, nullptr, &MovieDecoder::IOSeek);
		if (!io)
		{
			ff.av_freep(&buffer);
			return Fail("avio_alloc_context", 0);
		}
		format = ff.avformat_alloc_context();
		if (!format)
			return Fail("avformat_alloc_context", 0);
		format->pb = io;
		format->flags |= AVFMT_FLAG_CUSTOM_IO;
	}
	if ((r = ff.avformat_open_input(&format, utf8Path, nullptr, nullptr)) < 0)
		return Fail("avformat_open_input", r); // frees format; our io is freed with the decoder
	if ((r = ff.avformat_find_stream_info(format, nullptr)) < 0)
		return Fail("avformat_find_stream_info", r);

	const AVCodec* decoder = nullptr;
	videoIndex = ff.av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
	if (videoIndex < 0)
		return Fail("no decodable video stream", videoIndex);
	if (!OpenCodec(decoder, videoIndex, &videoCodec))
		return false;

	if (audioSink)
	{
		const AVCodec* audioDecoder = nullptr;
		audioIndex = ff.av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, videoIndex, &audioDecoder, 0);
		if (audioIndex >= 0 && OpenCodec(audioDecoder, audioIndex, &audioCodec))
			hasAudio = true;
		else
			audioIndex = -1;
	}

	AVStream* stream = format->streams[videoIndex];
	timeBase = av_q2d(stream->time_base);
	startTime = stream->start_time != AV_NOPTS_VALUE ? stream->start_time * timeBase : -1.0;
	if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0)
		fps = av_q2d(stream->avg_frame_rate);
	else if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0)
		fps = av_q2d(stream->r_frame_rate);
	if (fps <= 0.0 || fps > 240.0)
		fps = 30.0;

	if (format->duration > 0)
		duration = format->duration / (double)AV_TIME_BASE;
	else if (stream->duration > 0)
		duration = stream->duration * timeBase;

	frame = ff.av_frame_alloc();
	audioFrame = ff.av_frame_alloc();
	packet = ff.av_packet_alloc();
	if (!frame || !audioFrame || !packet)
		return Fail("out of memory", 0);

	// Preroll: decode until the first video frame is queued
	while (count == 0 && !eof)
	{
		bool progress = DemuxOnce();
		progress |= VideoOnce();
		if (!progress && demuxDone && !eof)
			break; // nothing decodable
	}
	if (count == 0)
	{
		if (error.empty())
			error = "no video frames decoded";
		return false;
	}
	return true;
}

void MovieDecoder::Start()
{
	stopping = false;
	demuxThread.Start([](void* p) { ((MovieDecoder*)p)->DemuxMain(); }, this);
	videoThread.Start([](void* p) { ((MovieDecoder*)p)->VideoMain(); }, this);
}

void MovieDecoder::Stop()
{
	{
		FFLockGuard guard(mutex);
		stopping = true;
		cond.WakeAll();
	}
	demuxThread.Join();
	videoThread.Join();
}

const MovieDecoder::Frame* MovieDecoder::FrameFor(double t, bool* changed)
{
	FFLockGuard guard(mutex);
	requestedTime = t;
	if (!consumerStarted)
		playStartMs = FFNowMs();
	consumerStarted = true;
	bool popped = false;
	while (count >= 2 && queue[(head + 1) % CAPACITY].pts <= t)
	{
		head = (head + 1) % CAPACITY;
		count--;
		popped = true;
	}
	if (popped)
		cond.WakeAll();
	if (count == 0)
		return nullptr;
	const Frame* f = &queue[head];
	bool isNew = (f != lastShown) || popped || !shownOnce;
	if (isNew)
		framesShown++;
	if (changed)
		*changed = isNew;
	lastShown = f;
	shownOnce = true;
	return f;
}

bool MovieDecoder::Finished(double t)
{
	FFLockGuard guard(mutex);
	if (!eof)
		return false;
	if (count == 0)
		return true;
	if (count > 1)
		return false;
	const Frame& last = queue[head];
	return t >= last.pts + last.duration;
}

double MovieDecoder::EndTime()
{
	FFLockGuard guard(mutex);
	return lastPts + lastDuration;
}

void MovieDecoder::DemuxMain()
{
	for (;;)
	{
		{
			FFLockGuard guard(mutex);
			if (stopping || demuxDone)
				return;
		}
		if (!DemuxOnce())
		{
			FFLockGuard guard(mutex);
			if (!stopping)
				cond.WaitFor(mutex, 5); // poll for audio consumption / room in the packet queue
		}
	}
}

void MovieDecoder::VideoMain()
{
	for (;;)
	{
		{
			FFLockGuard guard(mutex);
			if (stopping || eof)
				return;
		}
		if (!VideoOnce())
		{
			FFLockGuard guard(mutex);
			if (!stopping && !eof)
				cond.WaitFor(mutex, 5);
		}
	}
}

int MovieDecoder::IORead(void* opaque, uint8_t* buffer, int size)
{
	MovieDecoder* self = (MovieDecoder*)opaque;
	int n = self->fileIO->read(self->ioFile, buffer, size);
	return n < 0 ? AVERROR(EIO) : n == 0 ? AVERROR_EOF : n;
}

int64_t MovieDecoder::IOSeek(void* opaque, int64_t offset, int whence)
{
	MovieDecoder* self = (MovieDecoder*)opaque;
	if (whence & AVSEEK_SIZE)
		return self->fileIO->size(self->ioFile);
	int64_t r = self->fileIO->seek(self->ioFile, offset, whence & ~AVSEEK_FORCE);
	return r < 0 ? AVERROR(EIO) : r;
}

void MovieDecoder::CloseCustomIO()
{
	if (io)
	{
		ff.av_freep(&io->buffer); // FFmpeg may have replaced the buffer we gave it
		ff.avio_context_free(&io);
	}
	if (ioFile)
	{
		fileIO->close(ioFile);
		ioFile = nullptr;
	}
}

bool MovieDecoder::Fail(const char* what, int code)
{
	error = what;
	if (code < 0)
		error += ": " + ff.ErrorString(code);
	return false;
}

bool MovieDecoder::OpenCodec(const AVCodec* decoder, int index, AVCodecContext** out)
{
	int r;
	AVCodecContext* c = ff.avcodec_alloc_context3(decoder);
	if (!c)
		return Fail("avcodec_alloc_context3", 0);
	if ((r = ff.avcodec_parameters_to_context(c, format->streams[index]->codecpar)) < 0)
	{
		ff.avcodec_free_context(&c);
		return Fail("avcodec_parameters_to_context", r);
	}
	bool video = format->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
	if (video && decodeThreads > 0)
		ff.av_opt_set(c, "threads", std::to_string(decodeThreads).c_str(), 0);
	else
		ff.av_opt_set(c, "threads", "auto", 0);
	if (video && fastDecode)
	{
		ff.av_opt_set(c, "skip_loop_filter", "all", 0);
		ff.av_opt_set(c, "flags2", "+fast", 0);
	}
	if ((r = ff.avcodec_open2(c, decoder, nullptr)) < 0)
	{
		ff.avcodec_free_context(&c);
		return Fail("avcodec_open2", r);
	}
	if (video)
	{
		const char* pix = ff.av_get_pix_fmt_name ? ff.av_get_pix_fmt_name(c->pix_fmt) : nullptr;
		char buf[256];
		snprintf(buf, sizeof(buf), "%s %dx%d %s, %d thread%s (%s)%s", decoder->name, c->width, c->height,
			pix ? pix : "?", c->thread_count, c->thread_count == 1 ? "" : "s",
			(c->active_thread_type & FF_THREAD_FRAME) ? "frame" : (c->active_thread_type & FF_THREAD_SLICE) ? "slice" : "none",
			fastDecode ? ", fast decode" : "");
		videoInfo = buf;
	}
	*out = c;
	return true;
}

void MovieDecoder::PopPacket()
{
	ff.av_packet_free(&fifo[fifoHead]);
	fifoHead = (fifoHead + 1) % FIFO_MAX;
	fifoCount--;
}

bool MovieDecoder::QueueFull()
{
	FFLockGuard guard(mutex);
	return count >= CAPACITY;
}

bool MovieDecoder::CanDemux()
{
	if (demuxDone)
		return false;
	int packets;
	{
		FFLockGuard guard(mutex);
		packets = fifoCount;
	}
	if (packets >= FIFO_MAX)
		return false;
	if (packets < MIN_VIDEO_PACKETS)
		return true;
	return hasAudio && audioSink->BufferedSeconds() < AUDIO_AHEAD;
}

bool MovieDecoder::DemuxOnce()
{
	if (!CanDemux())
		return false;

	if (ff.av_read_frame(format, packet) < 0)
	{
		if (audioCodec)
		{
			ff.avcodec_send_packet(audioCodec, nullptr);
			ReceiveAudio();
		}
		if (audioSink && hasAudio)
			audioSink->EndOfStream();
		FFLockGuard guard(mutex);
		demuxDone = true;
		cond.WakeAll();
		return true;
	}

	if (packet->stream_index == videoIndex)
	{
		AVPacket* copy = ff.av_packet_alloc();
		if (copy)
		{
			ff.av_packet_move_ref(copy, packet); // keeps the reference-counted data
			FFLockGuard guard(mutex);
			fifo[(fifoHead + fifoCount) % FIFO_MAX] = copy;
			fifoCount++;
			cond.WakeAll();
		}
		else
			ff.av_packet_unref(packet);
	}
	else
	{
		if (packet->stream_index == audioIndex)
		{
			if (ff.avcodec_send_packet(audioCodec, packet) == AVERROR(EAGAIN))
			{
				ReceiveAudio();
				ff.avcodec_send_packet(audioCodec, packet);
			}
			ReceiveAudio();
		}
		ff.av_packet_unref(packet);
	}
	return true;
}

double MovieDecoder::PacketSeconds(const AVPacket* p)
{
	int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
	if (ts == AV_NOPTS_VALUE)
		return -1.0;
	return ts * timeBase - (startTime > 0.0 ? startTime : 0.0);
}

void MovieDecoder::CatchUp()
{
	double now, behind;
	{
		FFLockGuard guard(mutex);
		if (!consumerStarted || lastPts < 0.0)
			return;
		now = requestedTime;
		behind = now - lastPts;
	}
	if (behind > maxBehind)
		maxBehind = behind;

	bool skip = behind > 0.15;
	if (skip != reducedQuality)
	{
		reducedQuality = skip;
		if (skip)
			reducedSinceMs = FFNowMs();
		else if (reducedSinceMs > 0.0)
			reducedSeconds += (FFNowMs() - reducedSinceMs) / 1000.0;
		ff.av_opt_set(videoCodec, "skip_frame", skip ? "noref" : "default", 0);
		ff.av_opt_set(videoCodec, "skip_loop_filter", skip ? "all" : (fastDecode ? "all" : "default"), 0);
	}

	if (behind < 0.5)
		return;

	// Find the newest queued keyframe that is not in the future
	FFLockGuard guard(mutex);
	int target = -1;
	for (int i = 0; i < fifoCount; i++)
	{
		const AVPacket* p = fifo[(fifoHead + i) % FIFO_MAX];
		if (!(p->flags & AV_PKT_FLAG_KEY))
			continue;
		double t = PacketSeconds(p);
		if (t >= 0.0 && t <= now + 0.1)
			target = i;
	}
	if (target <= 0)
		return;
	for (int i = 0; i < target; i++)
	{
		ff.av_packet_free(&fifo[fifoHead]);
		fifoHead = (fifoHead + 1) % FIFO_MAX;
		fifoCount--;
	}
	if (pendingVideo)
		ff.av_packet_free(&pendingVideo);
	ff.avcodec_flush_buffers(videoCodec);
	keyframeJumps++;
	cond.WakeAll();
}

bool MovieDecoder::VideoOnce()
{
	bool progress = false;
	if (!flushSent)
		CatchUp();
	for (;;)
	{
		if (!pendingVideo)
		{
			FFLockGuard guard(mutex);
			if (fifoCount == 0)
				break;
			pendingVideo = fifo[fifoHead];
			fifo[fifoHead] = nullptr;
			fifoHead = (fifoHead + 1) % FIFO_MAX;
			fifoCount--;
			cond.WakeAll(); // room for the demuxer
		}
		double t0 = FFNowMs();
		int sent = ff.avcodec_send_packet(videoCodec, pendingVideo);
		decodeMs += FFNowMs() - t0;
		if (sent == AVERROR(EAGAIN))
			break; // decoder output must be drained first
		ff.av_packet_free(&pendingVideo);
		progress = true;
		if (ReceiveVideo())
			break; // a frame came out: let the queue logic run
	}

	bool finishedInput;
	{
		FFLockGuard guard(mutex);
		finishedInput = demuxDone && fifoCount == 0 && !pendingVideo;
	}
	if (finishedInput && !flushSent)
	{
		if (ff.avcodec_send_packet(videoCodec, nullptr) != AVERROR(EAGAIN))
			flushSent = true;
		progress = true;
	}

	if (ReceiveVideo())
		progress = true;

	if (finishedInput && videoFlushed)
	{
		FFLockGuard guard(mutex);
		eof = true;
		cond.WakeAll();
		progress = true;
	}
	return progress;
}

bool MovieDecoder::ReceiveVideo()
{
	bool received = false;
	while (!videoFlushed)
	{
		if (QueueFull())
			break; // leave remaining frames in the decoder for later
		double t0 = FFNowMs();
		int r = ff.avcodec_receive_frame(videoCodec, frame);
		decodeMs += FFNowMs() - t0;
		if (r == AVERROR(EAGAIN))
			break;
		if (r == AVERROR_EOF)
		{
			videoFlushed = true;
			break;
		}
		if (r < 0)
			continue; // corrupt data: skip it
		received = true;
		framesDecoded++;
#ifdef FFMOVIE_TEST_DECODE_DELAY_US
		usleep(FFMOVIE_TEST_DECODE_DELAY_US); // test hook: simulate a slow CPU
#endif
		QueueVideoFrame();
		ff.av_frame_unref(frame);
	}
	return received;
}

void MovieDecoder::QueueVideoFrame()
{
	if (frame->width <= 0 || frame->height <= 0 || !frame->data[0])
		return;
	if (width == 0)
	{
		width = frame->width;
		height = frame->height;
		if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0)
			pixelAspect = av_q2d(frame->sample_aspect_ratio);
	}

	// Timestamp in seconds from the start of the movie
	double pts;
	if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
	{
		pts = frame->best_effort_timestamp * timeBase;
		if (startTime < 0.0)
			startTime = pts;
		pts -= startTime;
	}
	else
		pts = lastPts < 0.0 ? 0.0 : lastPts + lastDuration;
	if (pts < 0.0)
		pts = 0.0;
	double frameDuration = 1.0 / fps;

	// If decoding has fallen behind playback, skip converting frames that are
	// already late (the consumer would drop them anyway).
	{
		FFLockGuard guard(mutex);
		// count includes the frame on screen; only skip when another newer frame is already waiting
		bool late = consumerStarted && count >= 2 && pts + 2 * frameDuration < requestedTime;
		lastPts = pts;
		lastDuration = frameDuration;
		if (late)
		{
			framesSkipped++;
			return;
		}
	}

	// Colour matrix: honour the stream's tags, defaulting to BT.709 for HD
	int colorspace = frame->colorspace;
	if (colorspace == AVCOL_SPC_UNSPECIFIED)
		colorspace = height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
	int srcFullRange = frame->color_range == AVCOL_RANGE_JPEG ||
		frame->format == AV_PIX_FMT_YUVJ420P || frame->format == AV_PIX_FMT_YUVJ422P || frame->format == AV_PIX_FMT_YUVJ444P;

	// GPU path: hand the planes over as they are (8-bit planar formats, same size as the texture)
	int cw = 0, ch = 0;
	switch (frame->format)
	{
	case AV_PIX_FMT_YUV420P: case AV_PIX_FMT_YUVJ420P: cw = (width + 1) / 2; ch = (height + 1) / 2; break;
	case AV_PIX_FMT_YUV422P: case AV_PIX_FMT_YUVJ422P: cw = (width + 1) / 2; ch = height; break;
	case AV_PIX_FMT_YUV444P: case AV_PIX_FMT_YUVJ444P: cw = width; ch = height; break;
	default: break;
	}
	if (gpuConvert && cw && frame->width == width && frame->height == height)
	{
		Frame* slot;
		{
			FFLockGuard guard(mutex);
			slot = &queue[(head + count) % CAPACITY];
		}
		if (!slot->yuv)
			slot->yuv = ff.av_frame_alloc();
		if (slot->yuv)
		{
			ff.av_frame_unref(slot->yuv);
			ff.av_frame_move_ref(slot->yuv, frame); // keeps the decoder's buffer, no copy
			slot->isYuv = true;
			slot->bt709 = colorspace == AVCOL_SPC_BT709;
			slot->fullRange = srcFullRange != 0;
			slot->chromaWidth = cw;
			slot->chromaHeight = ch;
			slot->pts = pts;
			slot->duration = frameDuration;
			framesGpu++;
			FFLockGuard guard(mutex);
			count++;
			return;
		}
	}

	sws = ff.sws_getCachedContext(sws, frame->width, frame->height, (AVPixelFormat)frame->format,
		width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!sws)
		return;
	const int* coefficients = ff.sws_getCoefficients(colorspace == AVCOL_SPC_BT709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
	ff.sws_setColorspaceDetails(sws, coefficients, srcFullRange, coefficients, 1, 0, 1 << 16, 1 << 16);

	Frame* slot;
	{
		FFLockGuard guard(mutex);
		slot = &queue[(head + count) % CAPACITY];
	}
	if (slot->yuv)
		ff.av_frame_unref(slot->yuv); // the slot held planes for the GPU before
	slot->isYuv = false;
	if (!slot->pixels)
	{
		slot->pixels = (uint8_t*)malloc((size_t)width * height * 4);
		if (!slot->pixels)
			return;
	}

	uint8_t* dst[4] = { slot->pixels, nullptr, nullptr, nullptr };
	int dstStride[4] = { width * 4, 0, 0, 0 };
	double t0 = FFNowMs();
	ff.sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
	convertMs += FFNowMs() - t0;
	framesConverted++;
	slot->pts = pts;
	slot->duration = frameDuration;

	FFLockGuard guard(mutex);
	count++;
}

void MovieDecoder::ReceiveAudio()
{
	for (;;)
	{
		int r = ff.avcodec_receive_frame(audioCodec, audioFrame);
		if (r < 0)
			return;
		WriteAudioFrame();
		ff.av_frame_unref(audioFrame);
	}
}

void MovieDecoder::WriteAudioFrame()
{
	if (!swr)
	{
		AVChannelLayout outLayout;
		ff.av_channel_layout_default(&outLayout, AudioSink::CHANNELS);
		int r = ff.swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, AudioSink::SAMPLE_RATE,
			&audioFrame->ch_layout, (AVSampleFormat)audioFrame->format, audioFrame->sample_rate, 0, nullptr);
		ff.av_channel_layout_uninit(&outLayout);
		if (r < 0 || ff.swr_init(swr) < 0)
		{
			ff.swr_free(&swr);
			audioIndex = -1; // give up on audio
			return;
		}
	}

	int outFrames = ff.swr_get_out_samples(swr, audioFrame->nb_samples);
	if (outFrames <= 0)
		return;
	if (outFrames > audioBufferFrames)
	{
		free(audioBuffer);
		audioBuffer = (int16_t*)malloc((size_t)outFrames * AudioSink::CHANNELS * sizeof(int16_t));
		audioBufferFrames = audioBuffer ? outFrames : 0;
		if (!audioBuffer)
			return;
	}
	uint8_t* out[1] = { (uint8_t*)audioBuffer };
	int converted = ff.swr_convert(swr, out, outFrames, (const uint8_t**)audioFrame->extended_data, audioFrame->nb_samples);
	if (converted > 0)
		audioSink->Write(audioBuffer, converted);
}
