// Copyright 2020 Mobilinkd LLC.

#include "M17Modulator.h"

#include <boost/program_options.hpp>
#include <boost/optional.hpp>

#include <signal.h>

const char VERSION[] = "1.0";

struct Config
{
    std::string source_address;
    std::string destination_address;
    std::string audio_device;
    std::string event_device;
    uint16_t key;
    bool verbose = false;
    bool debug = false;
    bool quiet = false;
    bool bitstream = false; // default is baseband audio

    static std::optional<Config> parse(int argc, char* argv[])
    {
        namespace po = boost::program_options;

        Config result;

        // Declare the supported options.
        po::options_description desc(
            "Program options");
        desc.add_options()
            ("help,h", "Print this help message and exit.")
            ("version,V", "Print the application verion and exit.")
            ("src,S", po::value<std::string>(&result.source_address)->required(),
                "transmitter identifier (your callsign).")
            ("dest,D", po::value<std::string>(&result.destination_address),
                "destination (default is broadcast).")
            ("audio,a", po::value<std::string>(&result.audio_device),
                "audio device (default is STDIN).")
            ("event,e", po::value<std::string>(&result.event_device)->default_value("/dev/input/by-id/usb-C-Media_Electronics_Inc._USB_Audio_Device-event-if03"),
                "event device (default is C-Media Electronics Inc. USB Audio Device).")
            ("key,k", po::value<uint16_t>(&result.key)->default_value(385),
                "Linux event code for PTT (default is RADIO).")
            ("bitstream,b", po::bool_switch(&result.bitstream),
                "output bitstream (default is baseband).")
            ("verbose,v", po::bool_switch(&result.verbose), "verbose output")
            ("debug,d", po::bool_switch(&result.debug), "debug-level output")
            ("quiet,q", po::bool_switch(&result.quiet), "silence all output")
            ;

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help"))
        {
            std::cout << "Read audio from STDIN and write baseband M17 to STDOUT\n"
                << desc << std::endl;

            return std::nullopt;
        }

        if (vm.count("version"))
        {
            std::cout << argv[0] << ": " << VERSION << std::endl;
            return std::nullopt;
        }

        try {
            po::notify(vm);
        } catch (std::exception& ex)
        {
            std::cerr << ex.what() << std::endl;
            std::cout << desc << std::endl;
            return std::nullopt;
        }

        if (result.debug + result.verbose + result.quiet > 1)
        {
            std::cerr << "Only one of quiet, verbos or debug may be chosen." << std::endl;
            return std::nullopt;
        }

        if (result.source_address.size() > 9)
        {
            std::cerr << "Source identifier too long." << std::endl;
            return std::nullopt;
        }

        if (result.destination_address.size() > 9)
        {
            std::cerr << "Destination identifier too long." << std::endl;
            return std::nullopt;
        }

        return result;
    }
};

std::atomic<bool> running{false};

void signal_handler(int)
{
    running = false;
    std::cerr << "quitting" << std::endl;
}

int main(int argc, char* argv[])
{
    using namespace mobilinkd;
    using namespace std::chrono_literals;

    auto config = Config::parse(argc, argv);
    if (!config) return 0;

    signal(SIGINT, &signal_handler);

    auto audio_queue = std::make_shared<M17Modulator::audio_queue_t>();
    auto bitstream_queue = std::make_shared<M17Modulator::bitstream_queue_t>();
    
    M17Modulator modulator(config->source_address, config->destination_address);
    auto future = modulator.run(audio_queue, bitstream_queue);
    modulator.ptt_on();

    std::cerr << "m17-mod running. ctrl-D to break." << std::endl;

    M17Modulator::bitstream_t bitstream;
    uint8_t bits;
    size_t index = 0;

    std::thread thd([audio_queue](){
        int16_t sample = 0;
        running = true;
        while (running && std::cin)
        {
            std::cin.read(reinterpret_cast<char*>(&sample), 2);
            audio_queue->put(sample, 5s);
        }
        running = false;
    });

    while (!running) std::this_thread::yield();

    // Input must be 8000 SPS, 16-bit LE, 1 channel raw audio.
    while (running)
    {
        if (!(bitstream_queue->get(bits, 1s)))
        {
            assert(bitstream_queue->is_closed());
            std::clog << "bitstream queue is closed; done transmitting." << std::endl;
            running = false;
            break;
        }

        if (config->bitstream)
        {
            std::cout << bits;
            index += 1;
            if (index == bitstream.size())
            {
                index == 0;
                std::cout.flush();
            }
        }
        else
        {
            bitstream[index++] = bits;
            if (index == bitstream.size())
            {
                auto baseband = M17Modulator::symbols_to_baseband(M17Modulator::bytes_to_symbols(bitstream));
                for (auto b : baseband) std::cout << uint8_t((b & 0xFF00) >> 8) << uint8_t(b & 0xFF);
                std::cout.flush();
            }
        }
    }

    std::clog << "No longer running" << std::endl;

    running = false;
    modulator.ptt_off();
    modulator.wait_until_idle();
    thd.join();
    audio_queue->close();
    future.get();
    bitstream_queue->close();

    return EXIT_SUCCESS;
}
