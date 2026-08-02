/* smoke_libav.c — 最小 libav 静态链接冒烟测试
 *
 * 目的（需求）：
 *   1. 验证 avformat/avcodec/avutil/swscale/swresample 五个静态库可用 MSVC 直接链接；
 *   2. 验证 GPL 软编全量配置下 libx264/libx265/libvpx/libopus 编码器与常用 muxer/demuxer/decoder 均已注册；
 *   3. 做一次真实往返：libx264 编码 -> mp4 封装 -> 解封装 -> h264 解码，证明整条导出链路可用。
 *
 * 用法：由 smoke-libav.ps1 在 VS x64 环境编译并运行；也可手动：
 *   cl /nologo /W3 /O2 /I <prefix>\include smoke_libav.c /Fe:smoke_libav.exe /link /LIBPATH:<prefix>\lib avformat.lib avcodec.lib avutil.lib swscale.lib swresample.lib x264.lib x265.lib vpx.lib opus.lib zlib.lib ws2_32.lib bcrypt.lib secur32.lib avrt.lib user32.lib ole32.lib ucrt.lib vcruntime.lib
 */
#include <stdio.h>
#include <string.h>

#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define CHECK(expr, msg) do { if (!(expr)) { printf("FAIL: %s\n", msg); return 1; } } while (0)

static int check_codec(const char *name, int is_encoder) {
    const AVCodec *c = is_encoder ? avcodec_find_encoder_by_name(name)
                                  : avcodec_find_decoder_by_name(name);
    printf("  %-14s %s\n", name, c ? "OK" : "MISSING");
    return c != NULL;
}

static int check_format(const char *short_name, const char *is_muxer) {
    int found = 0;
    if (is_muxer) {
        found = av_guess_format(short_name, NULL, NULL) != NULL;
    } else {
        found = av_find_input_format(short_name) != NULL;
    }
    printf("  %-18s %s\n", short_name, found ? "OK" : "MISSING");
    return found;
}

static int test_swscale(void) {
    printf("== swscale RGB24 -> YUV420P ==\n");
    struct SwsContext *s = sws_getContext(16, 16, AV_PIX_FMT_RGB24,
                                          16, 16, AV_PIX_FMT_YUV420P,
                                          SWS_BILINEAR, NULL, NULL, NULL);
    CHECK(s, "sws_getContext");
    uint8_t *src_data[4] = {0}; int src_linesize[4] = {0};
    uint8_t *dst_data[4] = {0}; int dst_linesize[4] = {0};
    CHECK(av_image_alloc(src_data, src_linesize, 16, 16, AV_PIX_FMT_RGB24, 1) >= 0, "av_image_alloc rgb");
    CHECK(av_image_alloc(dst_data, dst_linesize, 16, 16, AV_PIX_FMT_YUV420P, 1) >= 0, "av_image_alloc yuv");
    memset(src_data[0], 0x80, src_linesize[0] * 16);
    int ret = sws_scale(s, (const uint8_t *const *)src_data, src_linesize, 0, 16, dst_data, dst_linesize);
    sws_freeContext(s);
    av_freep(&src_data[0]);
    av_freep(&dst_data[0]);
    CHECK(ret == 16, "sws_scale row count");
    printf("  sws_scale OK (%d rows)\n", ret);
    return 0;
}

static int test_swresample(void) {
    printf("== swresample 48kHz stereo fltp -> 44.1kHz mono s16 ==\n");
    SwrContext *swr = NULL;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    CHECK(swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_S16, 44100,
                              &stereo, AV_SAMPLE_FMT_FLTP, 48000, 0, NULL) >= 0,
          "swr_alloc_set_opts2");
    CHECK(swr_init(swr) >= 0, "swr_init");
    const int n = 4800;
    uint8_t *in[2] = {0}; int in_linesize = 0;
    uint8_t *out[1] = {0}; int out_linesize = 0;
    CHECK(av_samples_alloc(in, &in_linesize, 2, n, AV_SAMPLE_FMT_FLTP, 0) >= 0, "alloc in");
    CHECK(av_samples_alloc(out, &out_linesize, 1, n, AV_SAMPLE_FMT_S16, 0) >= 0, "alloc out");
    int got = swr_convert(swr, out, n, (const uint8_t **)in, n);
    swr_free(&swr);
    av_freep(&in[0]);
    av_freep(&out[0]);
    CHECK(got > 0, "swr_convert output");
    printf("  swr_convert OK (%d samples)\n", got);
    return 0;
}

static int test_h264_roundtrip(const char *tmp_path) {
    printf("== libx264 encode -> mp4 mux -> demux -> h264 decode ==\n");
    const AVCodec *enc = avcodec_find_encoder_by_name("libx264");
    const AVCodec *dec = avcodec_find_decoder_by_name("h264");
    CHECK(enc && dec, "libx264/h264 codec not found");

    /* ---- encode 2 帧 ---- */
    AVCodecContext *e = avcodec_alloc_context3(enc);
    CHECK(e, "alloc libx264 ctx");
    e->width = 64; e->height = 64;
    e->time_base = (AVRational){1, 25};
    e->framerate = (AVRational){25, 1};
    e->pix_fmt = AV_PIX_FMT_YUV420P;
    e->bit_rate = 200000;
    e->gop_size = 12;
    e->max_b_frames = 0;
    e->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; /* mp4 需要 extradata 进 header */
    CHECK(avcodec_open2(e, enc, NULL) >= 0, "open libx264");

    AVFrame *fr = av_frame_alloc();
    CHECK(fr, "alloc frame");
    fr->format = e->pix_fmt; fr->width = e->width; fr->height = e->height;
    CHECK(av_frame_get_buffer(fr, 32) >= 0, "frame buffer");

    AVPacket *pkt = av_packet_alloc();
    AVPacket packets[8];
    int np = 0;
    for (int i = 0; i < 2 && np < 8; i++) {
        for (int y = 0; y < e->height; y++)
            memset(fr->data[0] + y * fr->linesize[0], (uint8_t)(i * 40 + y), e->width);
        memset(fr->data[1], 128, fr->linesize[1] * (e->height / 2));
        memset(fr->data[2], 128, fr->linesize[2] * (e->height / 2));
        fr->pts = i;
        CHECK(avcodec_send_frame(e, fr) == 0, "send_frame");
        while (avcodec_receive_packet(e, pkt) == 0 && np < 8)
            av_packet_move_ref(&packets[np++], pkt);
    }
    avcodec_send_frame(e, NULL); /* flush */
    while (avcodec_receive_packet(e, pkt) == 0 && np < 8)
        av_packet_move_ref(&packets[np++], pkt);
    av_packet_free(&pkt);
    av_frame_free(&fr);
    printf("  encoded packets: %d\n", np);
    CHECK(np > 0, "no packets encoded");

    /* ---- mux 到 mp4 ---- */
    AVFormatContext *mux = NULL;
    CHECK(avformat_alloc_output_context2(&mux, NULL, "mp4", tmp_path) >= 0, "alloc mp4 muxer");
    AVStream *vs = avformat_new_stream(mux, NULL);
    CHECK(vs, "new stream");
    vs->time_base = e->time_base;
    CHECK(avcodec_parameters_from_context(vs->codecpar, e) >= 0, "copy codecpar");
    CHECK(avio_open(&mux->pb, tmp_path, AVIO_FLAG_WRITE) >= 0, "avio_open");
    CHECK(avformat_write_header(mux, NULL) >= 0, "write_header");
    for (int i = 0; i < np; i++) {
        packets[i].stream_index = 0;
        CHECK(av_interleaved_write_frame(mux, &packets[i]) >= 0, "write_frame");
    }
    CHECK(av_write_trailer(mux) >= 0, "write_trailer");
    avio_closep(&mux->pb);
    avformat_free_context(mux);
    avcodec_free_context(&e);

    /* ---- demux + decode ---- */
    AVFormatContext *dem = NULL;
    CHECK(avformat_open_input(&dem, tmp_path, NULL, NULL) >= 0, "open mp4");
    CHECK(avformat_find_stream_info(dem, NULL) >= 0, "find_stream_info");
    int vi = av_find_best_stream(dem, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    CHECK(vi >= 0, "video stream");
    AVCodecContext *d = avcodec_alloc_context3(dec);
    CHECK(d, "alloc h264 ctx");
    CHECK(avcodec_parameters_to_context(d, dem->streams[vi]->codecpar) >= 0, "params to ctx");
    CHECK(avcodec_open2(d, dec, NULL) >= 0, "open h264 decoder");
    int got = 0;
    AVPacket *p = av_packet_alloc();
    AVFrame *out = av_frame_alloc();
    while (av_read_frame(dem, p) >= 0) {
        if (p->stream_index == vi && avcodec_send_packet(d, p) == 0)
            while (avcodec_receive_frame(d, out) == 0) got++;
        av_packet_unref(p);
    }
    avcodec_send_packet(d, NULL);
    while (avcodec_receive_frame(d, out) == 0) got++;
    printf("  decoded frames: %d\n", got);
    av_packet_free(&p);
    av_frame_free(&out);
    avcodec_free_context(&d);
    avformat_close_input(&dem);
    CHECK(got > 0, "no frames decoded");
    return 0;
}

int main(int argc, char **argv) {
    const char *tmp = argc > 1 ? argv[1] : "pb-smoke.mp4";
    int ok = 1;

    printf("== Playback libav smoke test ==\n");
    printf("av_version_info : %s\n", av_version_info());
    printf("avcodec config  : %s\n", avcodec_configuration());

    printf("encoders:\n");
    const char *encs[] = {"libx264", "libx265", "libvpx", "libvpx-vp9", "libopus", "aac", NULL};
    for (int i = 0; encs[i]; i++) ok &= check_codec(encs[i], 1);
    printf("decoders:\n");
    const char *decs[] = {"h264", "hevc", "vp9", "vp8", "opus", "aac", "mp3", NULL};
    for (int i = 0; decs[i]; i++) ok &= check_codec(decs[i], 0);
    printf("muxers:\n");
    const char *muxs[] = {"mp4", "mov", "matroska", "webm", "wav", "gif", NULL};
    for (int i = 0; muxs[i]; i++) ok &= check_format(muxs[i], "mux");
    printf("demuxers:\n");
    const char *demuxs[] = {"mov", "matroska", "wav", "image2", NULL};
    for (int i = 0; demuxs[i]; i++) ok &= check_format(demuxs[i], "demux");

    ok &= test_swscale() == 0;
    ok &= test_swresample() == 0;
    ok &= test_h264_roundtrip(tmp) == 0;

    printf("\nRESULT: %s\n", ok ? "ALL OK" : "FAILED");
    return ok ? 0 : 1;
}
