#include <cstdio>
#include <memory>
#include <mutex>
#include <atomic>
#include <numeric>
#include <algorithm>
#include <chrono>

#include <jack/jack.h>
#include <jack/midiport.h>

#include <fmt/core.h>

#include <ring-buffer.h>

using namespace std::chrono_literals;

namespace {
struct captures_t {
    jack_client_t *client;
    jack_port_t *midi_in_port;
    jack_port_t *audio_out_port;
};

const char *toString(jack_transport_state_t val)
{
    switch (val)
    {
    case JackTransportStopped: return "Stopped";
    case JackTransportRolling: return "Rolling";
    case JackTransportLooping: return "Looping";
    case JackTransportStarting: return "Starting";
    case JackTransportNetStarting: return "NetStarting";
    }

    __builtin_unreachable();
}

std::atomic<int32_t>       bar;            /**< current bar */
std::atomic<int32_t>       beat;           /**< current beat-within-bar */
std::atomic<int32_t>       tick;           /**< current tick-within-beat */
std::atomic<double>        beats_per_minute;

std::chrono::steady_clock::time_point lastTimebaseCallback;
} // namespace

int main(int argc, char *argv[])
{
    std::printf("Start\n");

    std::unique_ptr<jack_client_t, typeof(&jack_client_close)> client {{}, &jack_client_close};

    {
        const char *client_name = "biepometer";
        jack_options_t options = JackNoStartServer;
        jack_status_t status;
        const char *serverName = nullptr;

        client = std::unique_ptr<jack_client_t, typeof(&jack_client_close)>{
            jack_client_open(client_name, options, &status, serverName),
            &jack_client_close
        };
        if (!client)
            throw std::runtime_error{"no jack client"};
        if (status & JackNameNotUnique)
            std::printf("du depp du ned unique: %s\n", jack_get_client_name(client.get()));
    }

    std::mutex mutex;
    mutex.lock();

    jack_nframes_t sampleRate = jack_get_sample_rate(client.get());
    std::printf("sampleRate=%u\n", sampleRate);

    jack_nframes_t bufferSize = jack_get_buffer_size(client.get());
    std::printf("bufferSize=%u\n", bufferSize);

    auto unregisterCb = [&client](jack_port_t *arg){ jack_port_unregister(client.get(), arg); };

    auto midi_in_port = std::unique_ptr<jack_port_t, typeof(unregisterCb)> {
        jack_port_register(client.get(), "midi_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0),
        unregisterCb
    };
    if (!midi_in_port)
        throw std::runtime_error{"could not register midi in port"};
    std::printf("midi_in_port = %p\n", midi_in_port.get());

    auto audio_out_port = std::unique_ptr<jack_port_t, typeof(unregisterCb)> {
        jack_port_register(client.get(), "audio_out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0),
        std::move(unregisterCb)
    };
    if (!audio_out_port)
        throw std::runtime_error{"could not register audio out port"};
    std::printf("audio_out_port = %p\n", audio_out_port.get());

    captures_t captures { client.get(), midi_in_port.get(), audio_out_port.get() };

    if (const auto result = jack_set_process_callback(client.get(),
        [](jack_nframes_t nframes, void *arg)->int{
            const captures_t *captures = (const captures_t *)arg;
//            std::printf("%s callback %u\n", "", nframes);

            jack_position_t position;
            jack_transport_state_t transport = jack_transport_query(captures->client, &position);
//            std::printf("transport=%s bar=%i beat=%i tick=%i\n", toString(transport), position.bar, position.beat, position.tick);

            jack_nframes_t sample_rate = jack_get_sample_rate(captures->client);

            auto sample_interval = (1000000000ULL + (sample_rate/2)) / sample_rate;

            {
                void *inputPortBuf = jack_port_get_buffer(captures->midi_in_port, nframes);

                jack_nframes_t event_count = jack_midi_get_event_count(inputPortBuf);

                for (jack_nframes_t i = 0; i < event_count; i++)
                {
                    jack_midi_event_t in_event;
                    if (const auto result = jack_midi_event_get(&in_event, inputPortBuf, i); result != 0)
                    {
                        std::printf("jack_midi_event_get() failed with %i\n", result);
                        continue;
                    }

                    static uint64_t lastClock{};

                    switch (in_event.size)
                    {
                    case 0:
                        std::printf("midi event %u\n", in_event.time);
                        break;
                    case 1:
    //                    std::printf("midi event %u %hhu\n", in_event.time, in_event.buffer[0]);
                        switch (in_event.buffer[0])
                        {
                        case 248:
                        {
                            const uint64_t absolute_timestamp = position.usecs + (sample_interval * in_event.time / 1000);
                            const double bpm = int(60000000./(absolute_timestamp - lastClock) / 24 * 10) / 10.;
                            static ring_buffer<double, 10> bpmBuffer;
                            bpmBuffer.push_back(bpm);
                            beats_per_minute =
                                std::accumulate(std::cbegin(bpmBuffer), std::cend(bpmBuffer), 0.) /
                                std::size(bpmBuffer);
    //                        std::printf("clock usecs=%lu delta=%lu bpm=%.1f sample_rate=%u sample_interval=%llu\n", absolute_timestamp, absolute_timestamp - lastClock, bpm, sample_rate, sample_interval);
                            if (false && std::chrono::steady_clock::now() - lastTimebaseCallback > 1s)
                            {
                                std::printf("Registering timebase callback...\n");
                                jack_transport_start(captures->client);
                                lastTimebaseCallback = std::chrono::steady_clock::now();
                                if (const auto result = jack_set_timebase_callback(captures->client, 0, [](jack_transport_state_t state,
                                                                                                           jack_nframes_t nframes,
                                                                                                           jack_position_t *pos,
                                                                                                           int new_pos,
                                                                                                           void *arg){
                                    lastTimebaseCallback = std::chrono::steady_clock::now();
                                    pos->valid = JackPositionBBT;

                                    pos->bar = bar;
                                    pos->beat = beat;
                                    pos->tick = tick;

                                    pos->bar_start_tick = 0;

                                    pos->beats_per_bar = 4;
                                    pos->beat_type = 0;
                                    pos->ticks_per_beat = 24;
                                    pos->beats_per_minute = beats_per_minute;

                                    std::printf("timebase_callback(): state=%s nframes=%u bar=%i beat=%i tick=%i bpm=%f\n", toString(state), nframes, pos->bar, pos->beat, pos->tick, pos->beats_per_minute);
                                }, nullptr); result != 0)
                                    throw std::runtime_error{fmt::format("jack_set_timebase_callback() failed with {}", result)};
                            }
                            lastClock = absolute_timestamp;
                            tick++;
                            if (tick >= 24)
                            {
                                tick = 0;
                                beat++;
                                if (beat >= 4)
                                {
                                    beat = 0;
                                    bar++;
                                }
                            }
                            break;
                        }
                        case 250: // midi start
                            std::printf("midi start received\n");
                            bar = 0;
                            beat = 0;
                            tick = 0;
                            break;
                        case 252: // midi stop
                            std::printf("midi stop received\n");
                            break;
                        default:
                            std::printf("unknown 1-byte message %hhu\n", in_event.buffer[0]);
                        }
                        break;
                    case 2:
                        std::printf("midi event %hhu %hhu\n", in_event.buffer[0], in_event.buffer[1]);
                        break;
                    case 3:
                        std::printf("midi event %hhu %hhu %hhu\n", in_event.buffer[0], in_event.buffer[1], in_event.buffer[2]);
                        break;
                    case 4:
                        std::printf("midi event %hhu %hhu %hhu %hhu\n", in_event.buffer[0], in_event.buffer[1], in_event.buffer[2], in_event.buffer[3]);
                        break;
                    case 5:
                        std::printf("midi event %hhu %hhu %hhu %hhu %hhu\n", in_event.buffer[0], in_event.buffer[1], in_event.buffer[2], in_event.buffer[3], in_event.buffer[4]);
                        break;
                    }
                }
            }

            {
                jack_default_audio_sample_t *outputPortBuf = (jack_default_audio_sample_t*)jack_port_get_buffer(captures->audio_out_port, nframes);
                for (jack_nframes_t i = 0; i < nframes; i++)
                {
                    static jack_nframes_t j{};
                    switch (tick)
                    {
                    case 0:
//                    case 1:
                    case 12:
//                    case 13:
                        outputPortBuf[i] = (float(j++ % 512) / 512.f) - 1.f;
                        if (j >= 512)
                            j = 0;
                        break;
                    default:
                        outputPortBuf[i] = 0.f;
                        break;
                    }

                }
            }

            return {};
        }, &captures); result != 0)
        throw std::runtime_error{fmt::format("jack_set_process_callback() failed with {}", result)};

    if (const auto result = jack_activate(client.get()); result != 0)
        throw std::runtime_error{fmt::format("jack_activate() failed with {}", result)};
    auto unactivateCb = [&client](void *){
        std::printf("deactivate...\n");
        if (const auto result = jack_deactivate(client.get()); result != 0)
            std::printf("jack_deactivate() failed with %i\n", result);
    };
    auto activateGuard = std::unique_ptr<void, typeof(unactivateCb)>{(void*)1, unactivateCb};

    jack_on_shutdown(client.get(), [](void *arg){
        std::printf("shutdown received\n");
        std::mutex *mutex = (std::mutex *)arg;
        mutex->unlock();
    }, &mutex);

    std::printf("application working...\n");
    mutex.lock();

    std::printf("main thread ending\n");
    mutex.unlock();
}
