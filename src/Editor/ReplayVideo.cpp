#include <Editor/ReplayVideo.h>

#include <cmath>
#include <cstdio>
#include <format>
#include <regex>
#include <string_view>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_process.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <limits>

#include <Core/StringUtils.h>

#include <System/File.h>

#include <Editor/ReplayFile.h>
#include <Editor/ReplaySync.h>
#include <Editor/Sound.h>

namespace Vortex {

// ================================================================================================
// Reading the audio of a recording.

namespace {

// Frees the FFmpeg objects of ReadRecordingEnvelope.
struct Decoder {
    ~Decoder() {
        swr_free(&resampler);
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    SwrContext* resampler = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
};

// Days from 1970-01-01 to the given date (proleptic Gregorian calendar).
long DaysFromCivil(long y, long m, long d) {
    y -= m <= 2 ? 1 : 0;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

double ToSeconds(int y, int mo, int d, int h, int mi, int s) {
    return static_cast<double>(DaysFromCivil(y, mo, d)) * 86400.0 + h * 3600.0 +
           mi * 60.0 + s;
}

};  // anonymous namespace

bool ReadRecordingEnvelope(const fs::path& path, double hop, double begin,
                           double end, std::vector<float>& envelope,
                           double& start, std::string& error,
                           const std::atomic<bool>* cancel) {
    envelope.clear();
    start = 0.0;
    Decoder d;
    std::string file = pathToUtf8(path);
    if (avformat_open_input(&d.format, file.c_str(), nullptr, nullptr) < 0) {
        error = "could not open the recording";
        return false;
    }
    if (avformat_find_stream_info(d.format, nullptr) < 0) {
        error = "could not read the recording";
        return false;
    }
    const AVCodec* codec = nullptr;
    int stream =
        av_find_best_stream(d.format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream < 0 || !codec) {
        error = "the recording has no audio";
        return false;
    }
    AVStream* audio = d.format->streams[stream];
    d.codec = avcodec_alloc_context3(codec);
    if (!d.codec ||
        avcodec_parameters_to_context(d.codec, audio->codecpar) < 0 ||
        avcodec_open2(d.codec, codec, nullptr) < 0) {
        error = "could not decode the audio of the recording";
        return false;
    }
    const int frequency = d.codec->sample_rate;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    if (frequency <= 0 ||
        swr_alloc_set_opts2(&d.resampler, &mono, AV_SAMPLE_FMT_FLT, frequency,
                            &d.codec->ch_layout, d.codec->sample_fmt, frequency,
                            0, nullptr) < 0 ||
        swr_init(d.resampler) < 0) {
        error = "could not convert the audio of the recording";
        return false;
    }

    // Recording time 0 as players show it, the container may start later.
    double origin =
        d.format->start_time != AV_NOPTS_VALUE
            ? static_cast<double>(d.format->start_time) / AV_TIME_BASE
            : 0.0;
    if (begin > 0.0) {
        auto ts = static_cast<int64_t>((begin + origin) * AV_TIME_BASE);
        if (av_seek_frame(d.format, -1, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
            avcodec_flush_buffers(d.codec);
        }
    }

    d.packet = av_packet_alloc();
    d.frame = av_frame_alloc();
    if (!d.packet || !d.frame) {
        error = "out of memory";
        return false;
    }
    OnsetEnvelope onsets(frequency * hop);
    std::vector<float> samples;
    bool started = false, done = false;
    auto addFrame = [&]() {
        if (!started) {
            start = d.frame->pts != AV_NOPTS_VALUE
                        ? d.frame->pts * av_q2d(audio->time_base) - origin
                        : 0.0;
            started = true;
        }
        double time = start + static_cast<double>(onsets.values().size()) * hop;
        if (end >= 0.0 && time > end) {
            done = true;
            return;
        }
        int capacity = swr_get_out_samples(d.resampler, d.frame->nb_samples);
        if (capacity <= 0) return;
        samples.resize(capacity);
        auto* out = reinterpret_cast<uint8_t*>(samples.data());
        int count =
            swr_convert(d.resampler, &out, capacity,
                        const_cast<const uint8_t**>(d.frame->extended_data),
                        d.frame->nb_samples);
        if (count > 0) onsets.add(samples.data(), count);
    };
    while (!done && av_read_frame(d.format, d.packet) >= 0) {
        if (cancel && cancel->load()) {
            error = "cancelled";
            return false;
        }
        if (d.packet->stream_index == stream &&
            avcodec_send_packet(d.codec, d.packet) >= 0) {
            while (!done && avcodec_receive_frame(d.codec, d.frame) >= 0) {
                addFrame();
            }
        }
        av_packet_unref(d.packet);
    }
    if (!done && avcodec_send_packet(d.codec, nullptr) >= 0) {
        while (!done && avcodec_receive_frame(d.codec, d.frame) >= 0) {
            addFrame();
        }
    }
    envelope = onsets.values();
    if (envelope.empty()) {
        error = "no audio was found in that part of the recording";
        return false;
    }
    return true;
}

double ParseLocalDate(const std::string& date) {
    int y, mo, d, h, mi, s;
    if (std::sscanf(date.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi,
                    &s) != 6) {
        return -1.0;
    }
    return ToSeconds(y, mo, d, h, mi, s);
}

double RecordingStartTime(const fs::path& path) {
    // OBS names recordings "%CCYY-%MM-%DD %hh-%mm-%ss" by default.
    static const std::regex kDate(
        R"((\d{4})-(\d{2})-(\d{2})[ _T](\d{2})-(\d{2})-(\d{2}))");
    std::string name = pathToUtf8(path.filename());
    std::smatch m;
    if (!std::regex_search(name, m, kDate)) return -1.0;
    return ToSeconds(std::stoi(m[1]), std::stoi(m[2]), std::stoi(m[3]),
                     std::stoi(m[4]), std::stoi(m[5]), std::stoi(m[6]));
}

// ================================================================================================
// Finding the song in a recording.

VideoSyncInput PrepareVideoSync(const ReplayFile& file, const Sound& song,
                                double previewStart, double previewLength,
                                const fs::path& recording) {
    VideoSyncInput input;
    input.rate = file.rate;
    const int frequency = song.getFrequency();
    const int frames = song.getNumFrames();
    const double songLength =
        frequency > 0 ? static_cast<double>(frames) / frequency : 0.0;
    input.songSeconds = songLength / file.rate;

    // The song as the recording has it: at the rate, so a hop of song time
    // is hop * rate.
    OnsetEnvelope onsets(frequency * kVideoSyncHop * file.rate);
    std::vector<float> mono(4096);
    for (int i = 0; i < frames; i += static_cast<int>(mono.size())) {
        int count = std::min(static_cast<int>(mono.size()), frames - i);
        for (int j = 0; j < count; ++j) {
            mono[j] =
                (song.samplesL()[i + j] + song.samplesR()[i + j]) / 65536.0f;
        }
        onsets.add(mono.data(), count);
    }
    input.song = onsets.values();

    // Parts up to where the play ended, without the preview that also loops
    // in the song select menu (12 seconds if the simfile has no length).
    double end = songLength;
    if (file.endTime > 0.0) {
        end = std::min(end, file.endTime + file.rate * file.globalOffset +
                                file.syncOffset);
    }
    double previewEnd =
        previewStart + (previewLength > 0.0 ? previewLength : 12.0);
    input.parts = PickSongParts(0.0, end, previewStart, previewEnd, 8, 8.0);

    // The replay is dated when gameplay started, OBS names recordings after
    // the time they started.
    double date = ParseLocalDate(file.get("Date"));
    double recordingStart = RecordingStartTime(recording);
    input.hint = date >= 0.0 && recordingStart >= 0.0
                     ? date - recordingStart
                     : std::numeric_limits<double>::quiet_NaN();
    return input;
}

RecordingSync SyncVideo(const VideoSyncInput& input, const fs::path& recording,
                        const std::atomic<bool>* cancel) {
    // With an expected offset only that part of the recording is read first,
    // a whole evening of recording takes a while to decode.
    auto search = [&](double begin, double end) {
        RecordingSync result;
        std::vector<float> envelope;
        double start = 0.0;
        if (!ReadRecordingEnvelope(recording, kVideoSyncHop, begin, end,
                                   envelope, start, result.error, cancel)) {
            return result;
        }
        result =
            FindSongInRecording(input.song, envelope, kVideoSyncHop, input.rate,
                                input.parts, input.hint - start);
        result.offset += start;
        return result;
    };
    if (!std::isnan(input.hint)) {
        RecordingSync result = search(std::max(0.0, input.hint - 180.0),
                                      input.hint + input.songSeconds + 180.0);
        if (result.found || (cancel && cancel->load())) return result;
    }
    return search(0.0, -1.0);
}

// ================================================================================================
// VideoPlayer.

VideoPlayer::~VideoPlayer() { close(); }

bool VideoPlayer::open(const std::string& mpvPath, const std::string& extraArgs,
                       const fs::path& video, std::string& error) {
    close();
#ifdef _WIN32
    myPipeName =
        std::format("\\\\.\\pipe\\arrowvortex-mpv-{}", GetCurrentProcessId());
#else
    myPipeName = std::format("/tmp/arrowvortex-mpv-{}.sock", getpid());
#endif
    std::vector<std::string> args = {mpvPath,
                                     "--input-ipc-server=" + myPipeName,
                                     "--pause",
                                     "--keep-open=always",
                                     "--force-window=yes",
                                     "--no-audio",
                                     "--title=ArrowVortex video"};
    for (auto& arg : Str::split(extraArgs, " ", true, true)) {
        args.push_back(arg);
    }
    args.emplace_back("--");  // A file name is never read as an option.
    args.push_back(pathToUtf8(video));
    std::vector<const char*> argv;
    for (auto& arg : args) argv.push_back(arg.c_str());
    argv.push_back(nullptr);

    // Without a console the editor has no standard I/O to pass on, which
    // fails on Windows, and mpv's output is not needed.
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                           argv.data());
    for (const char* stream : {SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
                               SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                               SDL_PROP_PROCESS_CREATE_STDERR_NUMBER}) {
        SDL_SetNumberProperty(props, stream, SDL_PROCESS_STDIO_NULL);
    }
    myProcess = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!myProcess) {
        error = SDL_GetError();
        return false;
    }
    myPlaying = false;
    myTime = -1e9;
    return true;
}

void VideoPlayer::close() {
    if (!myProcess) return;
    if (!send(R"({"command":["quit"]})")) SDL_KillProcess(myProcess, true);
    disconnect();
    SDL_DestroyProcess(myProcess);
    myProcess = nullptr;
}

bool VideoPlayer::connect() {
#ifdef _WIN32
    if (myPipe) return true;
    HANDLE pipe = CreateFileA(myPipeName.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    myPipe = pipe;
#else
    if (mySocket >= 0) return true;
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return false;
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s",
                  myPipeName.c_str());
    if (::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) <
        0) {
        ::close(s);
        return false;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    mySocket = s;
#endif
    // Seeking fails until the video is loaded, mpv reports its duration then
    // (and right away if it already is).
    myLoaded = false;
    myInput.clear();
    return send(R"({"command":["observe_property",1,"duration"]})");
}

void VideoPlayer::disconnect() {
#ifdef _WIN32
    if (myPipe) CloseHandle(static_cast<HANDLE>(myPipe));
    myPipe = nullptr;
#else
    if (mySocket >= 0) ::close(mySocket);
    mySocket = -1;
#endif
}

bool VideoPlayer::send(const std::string& command) {
    if (!connect()) return false;
    std::string line = command + "\n";
#ifdef _WIN32
    DWORD written = 0;
    bool ok = WriteFile(static_cast<HANDLE>(myPipe), line.data(),
                        static_cast<DWORD>(line.size()), &written, nullptr) &&
              written == line.size();
#else
    bool ok = ::write(mySocket, line.data(), line.size()) ==
              static_cast<ssize_t>(line.size());
#endif
    if (!ok) disconnect();
    return ok;
}

// Reads what mpv sent: an answer to every command and events, of which only
// the duration matters. It has to be read so the connection does not fill up.
void VideoPlayer::drain() {
    char buffer[4096];
#ifdef _WIN32
    DWORD available = 0;
    while (myPipe &&
           PeekNamedPipe(static_cast<HANDLE>(myPipe), nullptr, 0, nullptr,
                         &available, nullptr) &&
           available > 0) {
        DWORD read = 0;
        if (!ReadFile(static_cast<HANDLE>(myPipe), buffer,
                      std::min<DWORD>(available, sizeof(buffer)), &read,
                      nullptr)) {
            disconnect();
            break;
        }
        myInput.append(buffer, read);
    }
#else
    ssize_t read = 0;
    while (mySocket >= 0 &&
           (read = ::read(mySocket, buffer, sizeof(buffer))) > 0) {
        myInput.append(buffer, static_cast<size_t>(read));
    }
#endif
    size_t begin = 0;
    for (size_t end; (end = myInput.find('\n', begin)) != std::string::npos;
         begin = end + 1) {
        std::string_view line(myInput.data() + begin, end - begin);
        if (!myLoaded && line.find(R"("name":"duration","data":)") !=
                             std::string_view::npos) {
            // Show the current time again, the commands before were lost.
            myLoaded = true;
            myPlaying = false;
            myTime = -1e9;
        }
    }
    myInput.erase(0, begin);
}

void VideoPlayer::show(double time, bool playing, double speed) {
    if (!myProcess) return;
    if (SDL_WaitProcess(myProcess, false, nullptr)) {
        // The mpv window was closed.
        disconnect();
        SDL_DestroyProcess(myProcess);
        myProcess = nullptr;
        return;
    }
    if (!connect()) return;  // mpv is still starting.
    drain();
    if (!myLoaded) return;

    const uint64_t now = SDL_GetTicks();
    const bool changed =
        playing != myPlaying || (playing && std::abs(speed - mySpeed) > 1e-6);
    auto seek = [&]() {
        send(std::format(R"({{"command":["seek",{:.4f},"absolute+exact"]}})",
                         time));
        myTime = time;
        myLastSeekTicks = now;
    };
    if (playing) {
        // Seek again when the editor jumped somewhere else while playing.
        double expected =
            myPlayStartTime +
            static_cast<double>(now - myPlayStartTicks) / 1000.0 * speed;
        if (changed || std::abs(time - expected) > 0.1) {
            seek();
            send(std::format(R"({{"command":["set_property","speed",{:.4f}]}})",
                             speed));
            send(R"({"command":["set_property","pause",false]})");
            myPlayStartTime = time;
            myPlayStartTicks = now;
        }
    } else {
        if (changed) send(R"({"command":["set_property","pause",true]})");
        if ((changed || std::abs(time - myTime) > 0.0005) &&
            now - myLastSeekTicks >= 30) {
            seek();
        }
    }
    myPlaying = playing;
    mySpeed = speed;
}

};  // namespace Vortex
