#include "client.hpp"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__ANDROID__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#endif

using namespace godot;

#if defined(__ANDROID__)
struct HwLoop {
    AMediaExtractor *extractor = nullptr;
    AMediaCodec *codec = nullptr;
    std::thread thread;
    std::mutex mutex;
    std::vector<uint8_t> y, u, v;
    int width = 0;
    int height = 0;
    std::atomic<bool> run{false};
    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    std::atomic<int> serial{0};
    int shown = 0;
    std::atomic<double> position{0};
    double duration = 0;
    bool loop = true;
    int64_t first_pts = 0;
    int64_t epoch_pts = 0;
    std::chrono::steady_clock::time_point epoch_wall;

    static void copy_frame(HwLoop *hw, const uint8_t *src, AMediaFormat *format) {
        int32_t width = 0, height = 0, stride = 0, slice = 0, color = 21;
        int32_t crop_left = 0, crop_top = 0, crop_right = 0, crop_bottom = 0;
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_STRIDE, &stride);
        AMediaFormat_getInt32(format, "slice-height", &slice);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color);
        if (AMediaFormat_getInt32(format, "crop-left", &crop_left)
                && AMediaFormat_getInt32(format, "crop-right", &crop_right))
            width = crop_right - crop_left + 1;
        if (AMediaFormat_getInt32(format, "crop-top", &crop_top)
                && AMediaFormat_getInt32(format, "crop-bottom", &crop_bottom))
            height = crop_bottom - crop_top + 1;
        if (width <= 0 || height <= 0 || !src) return;
        if (stride < width) stride = width;
        if (slice < height) slice = height;
        if (stride <= 0 || slice <= 0) return;
        const int chroma_w = width / 2;
        const int chroma_h = height / 2;
        std::vector<uint8_t> y(width * height), u(chroma_w * chroma_h), v(chroma_w * chroma_h);
        for (int row = 0; row < height; ++row)
            memcpy(y.data() + row * width, src + (crop_top + row) * stride + crop_left, width);
        const uint8_t *chroma = src + stride * slice;
        // Android vendors commonly expose YUV420 as planar (19), semi-planar
        // (21), or the flexible format (0x7f420888). The latter is usually
        // backed by the same two-plane NV12/NV21 layout when using ByteBuffer
        // output. Treat it as semi-planar so the hardware decoder remains
        // usable across devices instead of dropping every decoded frame.
        const bool planar = color == 19;
        const bool semiplanar = color == 21 || color == 0x7f420888 || !planar;
        for (int row = 0; row < chroma_h; ++row) {
            for (int col = 0; col < chroma_w; ++col) {
                if (planar) {
                    u[row * chroma_w + col] = chroma[(crop_top / 2 + row) * (stride / 2) + crop_left / 2 + col];
                    v[row * chroma_w + col] = chroma[stride * slice / 4 + (crop_top / 2 + row) * (stride / 2) + crop_left / 2 + col];
                } else if (semiplanar) {
                    const uint8_t *pair = chroma + (crop_top / 2 + row) * stride + crop_left + col * 2;
                    u[row * chroma_w + col] = pair[0];
                    v[row * chroma_w + col] = pair[1];
                }
            }
        }
        std::lock_guard<std::mutex> lock(hw->mutex);
        hw->width = width;
        hw->height = height;
        hw->y.swap(y);
        hw->u.swap(u);
        hw->v.swap(v);
        hw->serial.fetch_add(1);
    }

    static void pump(HwLoop *hw) {
        AMediaFormat *output_format = nullptr;
        bool input_done = false;
        while (hw->run.load()) {
            ssize_t in = input_done ? -1 : AMediaCodec_dequeueInputBuffer(hw->codec, 8000);
            if (in >= 0) {
                size_t capacity = 0;
                uint8_t *buffer = AMediaCodec_getInputBuffer(hw->codec, in, &capacity);
                const ssize_t sample = buffer ? AMediaExtractor_readSampleData(hw->extractor, buffer, capacity) : -1;
                if (sample < 0) {
                    AMediaCodec_queueInputBuffer(hw->codec, in, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    input_done = true;
                } else {
                    const int64_t time = AMediaExtractor_getSampleTime(hw->extractor);
                    AMediaCodec_queueInputBuffer(hw->codec, in, 0, sample, time, 0);
                    AMediaExtractor_advance(hw->extractor);
                }
            }
            AMediaCodecBufferInfo info;
            const ssize_t out = AMediaCodec_dequeueOutputBuffer(hw->codec, &info, 8000);
            if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                if (output_format) AMediaFormat_delete(output_format);
                output_format = AMediaCodec_getOutputFormat(hw->codec);
                UtilityFunctions::print("MediaCodec output format: ",
                    String::utf8(AMediaFormat_toString(output_format)));
                continue;
            }
            if (out < 0) continue;
            if (hw->serial.load() == 0) hw->first_pts = info.presentationTimeUs;
            if (hw->started.load() && info.presentationTimeUs >= hw->epoch_pts) {
                const auto due = hw->epoch_wall + std::chrono::microseconds(info.presentationTimeUs - hw->epoch_pts);
                while (hw->run.load() && std::chrono::steady_clock::now() < due)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            hw->position.store(double(info.presentationTimeUs) / 1000000.0);
            size_t size = 0;
            uint8_t *data = AMediaCodec_getOutputBuffer(hw->codec, out, &size);
            if (data && output_format && info.size > 0)
                copy_frame(hw, data + info.offset, output_format);
            else if (info.size > 0)
                UtilityFunctions::push_error("MediaCodec returned an unusable output buffer size=",
                    String::num_int64(info.size), " capacity=", String::num_int64(size));
            AMediaCodec_releaseOutputBuffer(hw->codec, out, false);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
                if (hw->loop) {
                    AMediaCodec_flush(hw->codec);
                    AMediaExtractor_seekTo(hw->extractor, 0, AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC);
                    hw->epoch_pts = 0;
                    hw->epoch_wall = std::chrono::steady_clock::now();
                    input_done = false;
                    continue;
                }
                hw->done.store(true);
                hw->run.store(false);
                break;
            }
            if (hw->serial.load() > 0 && !hw->started.load())
                while (hw->run.load() && !hw->started.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (output_format) AMediaFormat_delete(output_format);
    }
};

static HwLoop *start_hw_loop(const std::string &path, bool repeat) {
    auto *hw = new HwLoop();
    hw->extractor = AMediaExtractor_new();
    const int fd = open(path.c_str(), O_RDONLY);
    struct stat source_stat{};
    const media_status_t source_status = hw->extractor && fd >= 0 && fstat(fd, &source_stat) == 0
        ? AMediaExtractor_setDataSourceFd(hw->extractor, fd, 0, source_stat.st_size)
        : AMEDIA_ERROR_UNKNOWN;
    if (fd >= 0) close(fd);
    if (source_status != AMEDIA_OK) {
        UtilityFunctions::push_error("MediaCodec could not open video source: ", String::utf8(path.c_str()),
            " status=", String::num_int64(source_status));
        if (hw->extractor) AMediaExtractor_delete(hw->extractor);
        delete hw;
        return nullptr;
    }
    int track = -1;
    AMediaFormat *format = nullptr;
    const int tracks = AMediaExtractor_getTrackCount(hw->extractor);
    for (int i = 0; i < tracks; ++i) {
        AMediaFormat *candidate = AMediaExtractor_getTrackFormat(hw->extractor, i);
        const char *mime = nullptr;
        if (candidate && AMediaFormat_getString(candidate, AMEDIAFORMAT_KEY_MIME, &mime) && mime
                && strncmp(mime, "video/", 6) == 0) {
            track = i;
            format = candidate;
            break;
        }
        if (candidate) AMediaFormat_delete(candidate);
    }
    if (track < 0 || !format || AMediaExtractor_selectTrack(hw->extractor, track) != AMEDIA_OK) {
        if (format) AMediaFormat_delete(format);
        AMediaExtractor_delete(hw->extractor);
        delete hw;
        return nullptr;
    }
    const char *mime_ptr = nullptr;
    AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime_ptr);
    const std::string mime = mime_ptr ? mime_ptr : "";
    int64_t duration_us = 0;
    AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &duration_us);
    hw->duration = double(duration_us) / 1000000.0;
    hw->loop = repeat;
    hw->codec = mime.empty() ? nullptr : AMediaCodec_createDecoderByType(mime.c_str());
    media_status_t configured = hw->codec
        ? AMediaCodec_configure(hw->codec, format, nullptr, nullptr, 0) : AMEDIA_ERROR_UNKNOWN;
    AMediaFormat_delete(format);
    const media_status_t started = hw->codec && configured == AMEDIA_OK
        ? AMediaCodec_start(hw->codec) : AMEDIA_ERROR_UNKNOWN;
    if (!hw->codec || configured != AMEDIA_OK || started != AMEDIA_OK) {
        UtilityFunctions::push_error("MediaCodec could not initialize decoder mime=",
            String::utf8(mime.c_str()), " configure=", String::num_int64(configured),
            " start=", String::num_int64(started));
        if (hw->codec) AMediaCodec_delete(hw->codec);
        AMediaExtractor_delete(hw->extractor);
        delete hw;
        return nullptr;
    }
    hw->run.store(true);
    hw->thread = std::thread(HwLoop::pump, hw);
    for (int i = 0; i < 500 && hw->serial.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (hw->serial.load() == 0) {
        UtilityFunctions::push_error("MediaCodec timed out waiting for the first decoded frame");
        hw->run.store(false);
        if (hw->thread.joinable()) hw->thread.join();
        AMediaCodec_stop(hw->codec);
        AMediaCodec_delete(hw->codec);
        AMediaExtractor_delete(hw->extractor);
        delete hw;
        return nullptr;
    }
    return hw;
}

bool ClientVideo::start_hw(const char *path, bool repeat) {
    stop_hw();
    HwLoop *created = path ? start_hw_loop(path, repeat) : nullptr;
    if (!created) {
        UtilityFunctions::push_error("MediaCodec setup failed for: ", String::utf8(path));
        return false;
    }
    hw = created;
    hardware_duration = created->duration;
    publish_hw();
    if (!picture) {
        stop_hw();
        return false;
    }
    UtilityFunctions::print("MediaCodec first frame ready: ", String::utf8(path),
        " duration=", hardware_duration);
    return true;
}

void ClientVideo::stop_hw() {
    auto *decoder_hw = static_cast<HwLoop *>(hw);
    if (!decoder_hw) return;
    hw = nullptr;
    decoder_hw->run.store(false);
    if (decoder_hw->thread.joinable()) decoder_hw->thread.join();
    if (decoder_hw->codec) {
        AMediaCodec_stop(decoder_hw->codec);
        AMediaCodec_delete(decoder_hw->codec);
    }
    if (decoder_hw->extractor) AMediaExtractor_delete(decoder_hw->extractor);
    delete decoder_hw;
    hardware_duration = 0;
    hardware_position = 0;
}

void ClientVideo::publish_hw() {
    ClientVideo *video = this;
    auto *hw = static_cast<HwLoop *>(this->hw);
    if (!hw) return;
    const int serial = hw->serial.load();
    if (serial == hw->shown) return;
    hardware_position = hw->position.load();
    std::vector<uint8_t> y, u, v;
    int width = 0, height = 0;
    {
        std::lock_guard<std::mutex> lock(hw->mutex);
        if (hw->y.empty()) return;
        y.swap(hw->y);
        u.swap(hw->u);
        v.swap(hw->v);
        width = hw->width;
        height = hw->height;
        hw->shown = serial;
    }
    auto upload = [&](int plane, const std::vector<uint8_t> &src, int w, int h) {
        PackedByteArray bytes;
        bytes.resize(int(src.size()));
        memcpy(bytes.ptrw(), src.data(), src.size());
        Ref<Image> image = Image::create_from_data(w, h, false, Image::FORMAT_R8, bytes);
        if (!image.is_valid()) return;
        if (!video->planes[plane].is_valid()) video->planes[plane] = ImageTexture::create_from_image(image);
        else video->planes[plane]->update(image);
    };
    upload(0, y, width, height);
    upload(1, u, width / 2, height / 2);
    upload(2, v, width / 2, height / 2);
    if (!video->planes[3].is_valid()) {
        Ref<Image> white = Image::create_empty(width, height, false, Image::FORMAT_R8);
        white->fill(Color(1, 1, 1));
        video->planes[3] = ImageTexture::create_from_image(white);
    }
    if (!video->picture) {
        Ref<Shader> shader = ResourceLoader::get_singleton()->load("res://native/client/video_yuv.gdshader");
        video->material.instantiate();
        video->material->set_shader(shader);
        video->material->set_shader_parameter("resolution", Vector2i(width, height));
        video->material->set_shader_parameter("full_color", false);
        video->material->set_shader_parameter("interlaced", 0);
        video->material->set_shader_parameter("rotation", 0.f);
        video->material->set_shader_parameter("color_profile", Vector4(1.5748f, .1873f, .4681f, 1.8556f));
        const char *names[] = {"y_data", "u_data", "v_data", "a_data"};
        for (int i = 0; i < 4; ++i) video->material->set_shader_parameter(names[i], video->planes[i]);
        video->picture = memnew(TextureRect);
        video->picture->set_name("VideoPicture");
        video->picture->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        video->picture->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        video->picture->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_COVERED);
        video->picture->set_texture(video->planes[0]);
        video->picture->set_material(video->material);
        video->add_child(video->picture);
        video->picture->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    }
    video->picture->set_visible(true);
}
#elif defined(_WIN32)
struct HwLoop {
    std::wstring path;
    bool loop = false;
    std::atomic<bool> run{false};
    std::atomic<bool> started{false};
    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> done{false};
    std::atomic<int> serial{0};
    int shown = 0;
    std::atomic<double> position{0};
    double duration = 0;
    LONGLONG first_pts = 0;
    LONGLONG epoch_pts = 0;
    std::chrono::steady_clock::time_point epoch_wall;
    int width = 0;
    int height = 0;
    bool full_color = false;
    Vector4 color_profile = Vector4(1.5748f, .1873f, .4681f, 1.8556f);
    std::vector<uint8_t> y, u, v;
    std::mutex mutex;
    std::thread thread;

    static void pump(HwLoop *hw) {
        const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize_com = SUCCEEDED(com_result);
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        IMFAttributes *attributes = nullptr;
        IMFSourceReader *reader = nullptr;
        IMFMediaType *output = nullptr;
        IMFMediaType *actual = nullptr;
        bool mf_started = SUCCEEDED(hr);
        if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attributes, 2);
        if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(hw->path.c_str(), attributes, &reader);
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&output);
        if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (SUCCEEDED(hr)) hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, output);
        if (SUCCEEDED(hr)) hr = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actual);
        UINT32 width = 0, height = 0;
        if (SUCCEEDED(hr)) hr = MFGetAttributeSize(actual, MF_MT_FRAME_SIZE, &width, &height);
        if (SUCCEEDED(hr)) {
            PROPVARIANT duration;
            PropVariantInit(&duration);
            if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration))) {
                LONGLONG ticks = 0;
                if (SUCCEEDED(PropVariantToInt64(duration, &ticks))) hw->duration = double(ticks) / 10000000.0;
            }
            PropVariantClear(&duration);
            hr = S_OK;
        }
        if (SUCCEEDED(hr)) {
            UINT32 matrix = 0;
            UINT32 nominal_range = 0;
            actual->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
            actual->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &nominal_range);
            if (matrix == 1) {
                hw->color_profile = Vector4(1.402f, .344136f, .714136f, 1.772f);
            }
            hw->full_color = nominal_range == 2;
            UINT32 raw_stride = 0;
            LONG stride = int(width);
            if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride))) stride = LONG(raw_stride);
            stride = std::abs(stride);
            hw->width = int(width);
            hw->height = int(height);
            const size_t y_size = size_t(stride) * height;
            const size_t uv_stride = size_t((stride + 1) & ~1);
            const size_t uv_size = uv_stride * ((height + 1) / 2);
            while (hw->run.load()) {
                DWORD stream = 0, flags = 0;
                LONGLONG timestamp = 0;
                IMFSample *sample = nullptr;
                hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &timestamp, &sample);
                if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
                    if (sample) sample->Release();
                    hw->failed.store(true);
                    hw->ready.store(true);
                    break;
                }
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                    if (sample) sample->Release();
                    if (!hw->loop) {
                        hw->done.store(true);
                        break;
                    }
                    PROPVARIANT zero;
                    PropVariantInit(&zero);
                    zero.vt = VT_I8;
                    zero.hVal.QuadPart = 0;
                    reader->SetCurrentPosition(GUID_NULL, zero);
                    hw->epoch_pts = 0;
                    hw->epoch_wall = std::chrono::steady_clock::now();
                    continue;
                }
                if (!sample) continue;
                if (hw->serial.load() == 0) hw->first_pts = timestamp;
                if (hw->started.load() && timestamp >= hw->epoch_pts) {
                    const auto due = hw->epoch_wall + std::chrono::nanoseconds(timestamp - hw->epoch_pts) * 100;
                    while (hw->run.load() && std::chrono::steady_clock::now() < due)
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                IMFMediaBuffer *buffer = nullptr;
                hr = sample->ConvertToContiguousBuffer(&buffer);
                BYTE *data = nullptr;
                IMF2DBuffer2 *buffer_2d = nullptr;
                DWORD maximum = 0, length = 0;
                LONG actual_pitch = stride;
                if (SUCCEEDED(hr)) buffer->QueryInterface(IID_PPV_ARGS(&buffer_2d));
                if (buffer_2d) {
                    BYTE *buffer_start = nullptr;
                    hr = buffer_2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &data,
                        &actual_pitch, &buffer_start, &maximum);
                    actual_pitch = std::abs(actual_pitch);
                } else if (SUCCEEDED(hr)) {
                    hr = buffer->Lock(&data, &maximum, &length);
                }
                 // Hardware NV12 surfaces are commonly padded from 1080 to
                 // 1088 rows. The UV plane starts after the padded luma
                 // height, not after the visible height; using the latter
                 // produces a thin green strip at the top of the image.
                 int storage_height = height;
                 const size_t plane_bytes = size_t(actual_pitch) * 3 / 2;
                 if (plane_bytes > 0 && maximum >= plane_bytes * size_t(height)
                         && maximum % plane_bytes == 0) {
                     const size_t candidate = maximum / plane_bytes;
                     if (candidate >= size_t(height) && candidate <= size_t(height + 64))
                         storage_height = int(candidate);
                 }
                 const size_t actual_y_size = size_t(actual_pitch) * storage_height;
                 const size_t actual_uv_stride = size_t((actual_pitch + 1) & ~1);
                 const size_t actual_uv_size = actual_uv_stride * ((storage_height + 1) / 2);
                if (SUCCEEDED(hr) && data && maximum >= actual_y_size + actual_uv_size) {
                    std::vector<uint8_t> next_y(size_t(width) * height);
                    std::vector<uint8_t> next_u(size_t((width + 1) / 2) * ((height + 1) / 2));
                    std::vector<uint8_t> next_v(next_u.size());
                    for (UINT32 row = 0; row < height; ++row)
                        memcpy(next_y.data() + size_t(row) * width, data + size_t(row) * actual_pitch, width);
                    const uint8_t *chroma = data + actual_y_size;
                    const int chroma_width = int((width + 1) / 2);
                    const int chroma_height = int((height + 1) / 2);
                    for (int row = 0; row < chroma_height; ++row)
                        for (int col = 0; col < chroma_width; ++col) {
                            const size_t source = size_t(row) * actual_uv_stride + size_t(col) * 2;
                            const size_t target = size_t(row) * chroma_width + col;
                            next_u[target] = chroma[source];
                            next_v[target] = chroma[source + 1];
                        }
                    if (buffer_2d) buffer_2d->Unlock2D();
                    else if (buffer) buffer->Unlock();
                    {
                        std::lock_guard<std::mutex> lock(hw->mutex);
                        hw->y.swap(next_y);
                        hw->u.swap(next_u);
                        hw->v.swap(next_v);
                    }
                    hw->position.store(double(timestamp) / 10000000.0);
                    hw->serial.fetch_add(1);
                    hw->ready.store(true);
                } else {
                    if (buffer_2d) buffer_2d->Unlock2D();
                    else if (SUCCEEDED(hr) && buffer) buffer->Unlock();
                    hw->failed.store(true);
                    hw->ready.store(true);
                }
                if (buffer_2d) buffer_2d->Release();
                if (buffer) buffer->Release();
                sample->Release();
                if (hw->failed.load()) break;
                if (!hw->started.load())
                    while (hw->run.load() && !hw->started.load())
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } else {
            UtilityFunctions::push_error("Media Foundation video setup failed, HRESULT=0x", String::num_uint64(uint64_t(hr), 16));
            hw->failed.store(true);
            hw->ready.store(true);
        }
        if (actual) actual->Release();
        if (output) output->Release();
        if (reader) reader->Release();
        if (attributes) attributes->Release();
        if (mf_started) MFShutdown();
        if (uninitialize_com) CoUninitialize();
    }
};

bool ClientVideo::start_hw(const char *path, bool repeat) {
    stop_hw();
    if (!path) return false;
    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    if (wide_length <= 1) return false;
    auto *created = new HwLoop();
    created->path.resize(wide_length);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, created->path.data(), wide_length);
    created->path.resize(wide_length - 1);
    created->loop = repeat;
    created->run.store(true);
    created->thread = std::thread(HwLoop::pump, created);
    for (int i = 0; i < 500 && !created->ready.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!created->ready.load() || created->failed.load() || created->serial.load() == 0) {
        created->run.store(false);
        if (created->thread.joinable()) created->thread.join();
        delete created;
        return false;
    }
    hw = created;
    hardware_duration = created->duration;
    publish_hw();
    return picture != nullptr;
}

void ClientVideo::stop_hw() {
    auto *decoder = static_cast<HwLoop *>(hw);
    if (!decoder) return;
    hw = nullptr;
    decoder->run.store(false);
    decoder->started.store(true);
    if (decoder->thread.joinable()) decoder->thread.join();
    delete decoder;
    hardware_duration = 0;
    hardware_position = 0;
}

void ClientVideo::publish_hw() {
    auto *decoder = static_cast<HwLoop *>(hw);
    if (!decoder) return;
    hardware_position = decoder->position.load();
    const int serial = decoder->serial.load();
    if (serial == 0 || serial == decoder->shown) return;
    std::vector<uint8_t> y, u, v;
    {
        std::lock_guard<std::mutex> lock(decoder->mutex);
        y = decoder->y;
        u = decoder->u;
        v = decoder->v;
    }
    auto upload = [&](int plane, const std::vector<uint8_t> &src, int width, int height) {
        PackedByteArray bytes;
        bytes.resize(int(src.size()));
        memcpy(bytes.ptrw(), src.data(), src.size());
        Ref<Image> image = Image::create_from_data(width, height, false, Image::FORMAT_R8, bytes);
        if (!image.is_valid()) return;
        if (!planes[plane].is_valid()) planes[plane] = ImageTexture::create_from_image(image);
        else planes[plane]->update(image);
    };
    const int width = decoder->width, height = decoder->height;
    upload(0, y, width, height);
    upload(1, u, (width + 1) / 2, (height + 1) / 2);
    upload(2, v, (width + 1) / 2, (height + 1) / 2);
    decoder->shown = serial;
    if (!planes[3].is_valid()) {
        Ref<Image> alpha = Image::create_empty(width, height, false, Image::FORMAT_R8);
        alpha->fill(Color(1, 1, 1));
        planes[3] = ImageTexture::create_from_image(alpha);
    }
    if (!picture) {
        material.instantiate();
        material->set_shader(ResourceLoader::get_singleton()->load("res://native/client/video_yuv.gdshader"));
        material->set_shader_parameter("resolution", Vector2i(width, height));
        material->set_shader_parameter("full_color", decoder->full_color);
        material->set_shader_parameter("interlaced", 0);
        material->set_shader_parameter("rotation", 0.f);
        material->set_shader_parameter("color_profile", decoder->color_profile);
        const char *names[] = {"y_data", "u_data", "v_data", "a_data"};
        for (int i = 0; i < 4; ++i) material->set_shader_parameter(names[i], planes[i]);
        picture = memnew(TextureRect);
        picture->set_name("VideoPicture");
        picture->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        picture->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        picture->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_COVERED);
        picture->set_texture(planes[0]);
        picture->set_material(material);
        add_child(picture);
        picture->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    }
}
#else
bool ClientVideo::start_hw(const char *, bool) { return false; }
void ClientVideo::stop_hw() {}
void ClientVideo::publish_hw() {}
#endif

static String extract_video(const String &resource) {
    if (!resource.begins_with("res://assets/resources/video/startup/") || !resource.ends_with(".mp4")) return "";
    Ref<FileAccess> source = FileAccess::open(resource, FileAccess::READ);
    if (!source.is_valid()) {
        UtilityFunctions::push_error("Video is missing from export: ", resource);
        return "";
    }
    const String target = "user://" + resource.get_file();
    if (FileAccess::file_exists(target)) {
        Ref<FileAccess> cached = FileAccess::open(target, FileAccess::READ);
        if (cached.is_valid() && cached->get_length() == source->get_length())
            return ProjectSettings::get_singleton()->globalize_path(target);
    }
    Ref<FileAccess> dest = FileAccess::open(target, FileAccess::WRITE);
    if (!dest.is_valid()) return "";
    while (source->get_position() < source->get_length()) {
        const int64_t amount = std::min<uint64_t>(65536, source->get_length() - source->get_position());
        if (!dest->store_buffer(source->get_buffer(amount))) return "";
    }
    dest->close();
    return ProjectSettings::get_singleton()->globalize_path(target);
}

bool ClientVideo::play(const String &resource, bool repeat, bool paused) {
    const String path = extract_video(resource);
    if (path.is_empty()) return false;
#if defined(__ANDROID__) || defined(_WIN32)
    if (hw && prepared_resource == resource) {
        loop = repeat;
        finished = false;
        initially_paused = paused;
        if (!paused) start();
        return true;
    }
#endif
    stop();
    finished = false;
    loop = repeat;
    if (OS::get_singleton()->get_name() == "iOS") {
        if (!Engine::get_singleton()->has_singleton("UnFalsusVideo")) {
            UtilityFunctions::push_error("UnFalsusVideo iOS plugin is unavailable");
            return false;
        }
        ios = Engine::get_singleton()->get_singleton("UnFalsusVideo");
        ios_previous_transparent_background = get_viewport()->has_transparent_background();
        get_viewport()->set_transparent_background(true);
        if (bool(ios->call("play", path, loop))) return true;
        get_viewport()->set_transparent_background(ios_previous_transparent_background);
        ios = nullptr;
        return false;
    }

#if defined(__ANDROID__)
    const CharString hardware_path = path.utf8();
    if (!start_hw(hardware_path.get_data(), repeat)) return false;
    initially_paused = paused;
    if (!paused) start();
    return true;
#elif defined(_WIN32)
    const CharString hardware_path = path.utf8();
    if (!start_hw(hardware_path.get_data(), repeat)) return false;
    initially_paused = paused;
    if (!paused) start();
    return true;
#endif

    // Loading the .gdextension resource is required in exported/headless runs;
    // the addon directory is not otherwise scanned for native extensions.
    ResourceLoader::get_singleton()->load("res://addons/gde_gozen/gozen.gdextension");
    auto *db = ClassDBSingleton::get_singleton();
    if (!db->class_exists("GoZenVideo")) {
        UtilityFunctions::push_error("GoZen GoZenVideo class is unavailable");
        return false;
    }
    Variant instance = db->instantiate("GoZenVideo");
    decoder = instance;
    if (!decoder || int(decoder->call("open", path)) != OK) {
        UtilityFunctions::push_error("GoZen could not open video: ", path);
        decoder = nullptr;
        return false;
    }
    decoder_ref = Ref<RefCounted>(Object::cast_to<RefCounted>(decoder));
    const Vector2i resolution = decoder->call("get_resolution");
    frame_time = 1.0 / float(decoder->call("get_framerate"));
    frame_count = int(decoder->call("get_frame_count"));
    has_alpha = bool(decoder->call("get_has_alpha"));
    if (resolution.x <= 0 || resolution.y <= 0 || frame_time <= 0 || frame_count <= 0) {
        UtilityFunctions::push_error("GoZen returned invalid video metadata: ", resource);
        stop();
        return false;
    }
    Ref<Shader> shader = ResourceLoader::get_singleton()->load("res://addons/gde_gozen/shaders/yuv_to_rgb_compatibility.gdshader");
    material.instantiate();
    material->set_shader(shader);
    material->set_shader_parameter("resolution", decoder->call("get_actual_resolution"));
    material->set_shader_parameter("full_color", decoder->call("is_full_color_range"));
    material->set_shader_parameter("interlaced", decoder->call("get_interlaced"));
    material->set_shader_parameter("rotation", float(decoder->call("get_rotation")) * float(Math_PI / 180.0));
    const String profile = decoder->call("get_color_profile");
    const Vector4 color_profile = profile == "bt601" || profile == "bt470"
        ? Vector4(1.402f, .344136f, .714136f, 1.772f)
        : profile == "bt2020" || profile == "bt2100"
        ? Vector4(1.4746f, .16455f, .57135f, 1.8814f)
        : Vector4(1.5748f, .1873f, .4681f, 1.8556f);
    material->set_shader_parameter("color_profile", color_profile);
    const char *names[] = {"y_data", "u_data", "v_data", "a_data"};
    const char *methods[] = {"get_y_data", "get_u_data", "get_v_data", "get_a_data"};
    Ref<Image> opaque_alpha = Image::create_empty(resolution.x, resolution.y, false, Image::FORMAT_R8);
    opaque_alpha->fill(Color(1, 1, 1));
    for (int i = 0; i < 4; ++i) {
        Ref<Image> image = (i == 3 && !has_alpha) ? opaque_alpha : Ref<Image>(decoder->call(methods[i]));
        if (!image.is_valid() || image->is_empty()) {
            UtilityFunctions::push_error("GoZen returned an empty video plane: ", names[i]);
            stop();
            return false;
        }
        planes[i] = ImageTexture::create_from_image(image);
        material->set_shader_parameter(names[i], planes[i]);
    }
    picture = memnew(TextureRect);
    picture->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    picture->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    picture->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_COVERED);
    picture->set_texture(planes[0]);
    picture->set_material(material);
    add_child(picture);
    picture->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    if (!paused) start();
    return true;
}

void ClientVideo::start() {
#if defined(__ANDROID__)
    if (hw) {
        auto *decoder_hw = static_cast<HwLoop *>(hw);
        decoder_hw->epoch_pts = decoder_hw->first_pts;
        decoder_hw->epoch_wall = std::chrono::steady_clock::now();
        decoder_hw->started.store(true);
        initially_paused = false;
        return;
    }
#elif defined(_WIN32)
    if (hw) {
        auto *decoder_hw = static_cast<HwLoop *>(hw);
        decoder_hw->epoch_pts = decoder_hw->first_pts;
        decoder_hw->epoch_wall = std::chrono::steady_clock::now();
        decoder_hw->started.store(true);
        initially_paused = false;
        return;
    }
#endif
    // GoZen begins decoding after open(); frames are advanced in _process().
}

bool ClientVideo::hardware_finished() const {
    if (!hw) return false;
#if defined(__ANDROID__) || defined(_WIN32)
    return static_cast<const HwLoop *>(hw)->done.load();
#else
    return false;
#endif
}

bool ClientVideo::prepare(const String &resource, bool repeat) {
    const String path = extract_video(resource);
    if (path.is_empty()) return false;
#if defined(__APPLE__) && !defined(__ANDROID__)
    if (OS::get_singleton()->get_name() == "iOS") {
        prepared_resource = resource;
        loop = repeat;
        finished = false;
        return true;
    }
#endif
#if defined(__ANDROID__) || defined(_WIN32)
    if (hw && prepared_resource == resource) return true;
    stop();
    const CharString hardware_path = path.utf8();
    if (!start_hw(hardware_path.get_data(), repeat)) return false;
    prepared_resource = resource;
    loop = repeat;
    finished = false;
    initially_paused = true;
#endif
    return true;
}

void ClientVideo::stop() {
    stop_hw();
    if (ios) {
        ios->call("stop");
        if (get_viewport()) get_viewport()->set_transparent_background(ios_previous_transparent_background);
        ios = nullptr;
    }
    if (decoder) {
        decoder->call("close");
        decoder = nullptr;
        decoder_ref.unref();
    }
    if (picture) {
        picture->queue_free();
        picture = nullptr;
    }
    for (auto &plane : planes) plane.unref();
    material.unref();
    frame_time = 0;
    accumulator = 0;
    frame = 0;
    frame_count = 0;
    has_alpha = false;
    prepared_resource = String();
}

void ClientVideo::_process(double delta) {
    if (ios) {
        if (!loop && bool(ios->call("is_finished"))) finished = true;
        return;
    }
#if defined(__ANDROID__)
    if (hw) {
        publish_hw();
        finished = hardware_finished();
        return;
    }
#elif defined(_WIN32)
    if (hw) {
        publish_hw();
        finished = hardware_finished();
        return;
    }
#endif
    if (!decoder || finished || frame_time <= 0) return;
    accumulator += delta;
    if (accumulator < frame_time) return;
    const int frames_to_advance = std::min(3, int(accumulator / frame_time));
    accumulator -= frames_to_advance * frame_time;
    bool advanced = false;
    for (int i = 0; i < frames_to_advance; ++i) {
        if (!bool(decoder->call("next_frame", false))) {
            if (loop) {
                decoder->call("seek_frame", 0);
                frame = 0;
            } else {
                finished = true;
            }
            break;
        }
        ++frame;
        advanced = true;
    }
    if (finished || !advanced) {
        if (loop) decoder->call("seek_frame", 0);
        else finished = true;
        return;
    }
    const char *methods[] = {"get_y_data", "get_u_data", "get_v_data", "get_a_data"};
    const int plane_count = has_alpha ? 4 : 3;
    for (int i = 0; i < plane_count; ++i) {
        Ref<Image> image = decoder->call(methods[i]);
        if (image.is_valid() && !image->is_empty() && planes[i].is_valid()) planes[i]->update(image);
    }
}

void ClientVideo::set_native_white(float alpha) {
    if (ios) ios->call("set_white", std::clamp(alpha, 0.f, 1.f));
}

void ClientVideo::set_native_title(const String &resource) {
    if (!ios) return;
    Ref<Texture2D> texture = ResourceLoader::get_singleton()->load(resource);
    if (!texture.is_valid()) return;
    const String path = ProjectSettings::get_singleton()->globalize_path("user://startup_title.png");
    if (texture->get_image()->save_png(path) == OK) ios->call("set_title", path);
}
