#include "remote_source.h"
#include "common/utils.h"

#include "udp_discovery.h"

void RemoteSource::set_others()
{
    sendPacketWithVector(tcp_client, dsp::remote::PKT_TYPE_BITDEPTHSET, {(uint8_t)bit_depth_used});
}

void RemoteSource::set_settings(nlohmann::json settings)
{
    if (d_settings.contains("remote_bit_depth"))
        bit_depth_used = d_settings["remote_bit_depth"];

    sendPacketWithVector(tcp_client, dsp::remote::PKT_TYPE_SETSETTINGS, nlohmann::json::to_cbor(settings));

    if (is_open)
        set_others();

    d_settings = settings;
}

nlohmann::json RemoteSource::get_settings()
{
    sendPacketWithVector(tcp_client, dsp::remote::PKT_TYPE_GETSETTINGS);
    waiting_for_settings = true;
    while (waiting_for_settings)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    logger->trace("Done waiting for settings!");
    d_settings["remote_bit_depth"] = bit_depth_used;
    return d_settings;
}

void RemoteSource::open()
{
    sendPacketWithVector(tcp_client, dsp::remote::PKT_TYPE_SOURCEOPEN, nlohmann::json::to_cbor(nlohmann::json(remote_source_desc)));
    is_open = true;
}

void RemoteSource::start()
{
    DSPSampleSource::start();
    sendPacketWithVector(tcp_client, dsp::remote::PKT_TYPE_SOURCESTART);
    is_started = true;
}

void RemoteSource::stop()
{
    sendPacketWithVector(tcp_client, dsp::remote::PKT_TYPE_SOURCESTOP);

    if (is_started)
    {
        is_started = false;

        output_stream->stopReader();
        output_stream->stopWriter();
    }
}

void RemoteSource::close()
{
    if (is_open)
    {
        sendPacketWithVector(tcp_client, dsp::remote::PKT_TYPE_SOURCECLOSE);
        is_open = false;
    }
}

void RemoteSource::set_frequency(uint64_t frequency)
{
    std::vector<uint8_t> pkt(8);
    *((double *)&pkt[0]) = frequency;
    sendPacketWithVector(tcp_client, dsp::remote::PKT_TYPE_SETFREQ, pkt);
    DSPSampleSource::set_frequency(frequency);
}

void RemoteSource::drawControlUI()
{
    // Local settings
    if (RImGui::Combo("Bit Depth###remotesdrbitdepth", &selected_bit_depth, "8\0"
                                                                            "16\0"
                                                                            "32\0"))
    {
        if (selected_bit_depth == 0)
            bit_depth_used = 8;
        else if (selected_bit_depth == 1)
            bit_depth_used = 16;
        else if (selected_bit_depth == 2)
            bit_depth_used = 32;
        set_others();
    }

    RImGui::Separator();

    // Draw remote UI
    drawelems_mtx.lock();
    std::vector<RImGui::UiElem> feedback = RImGui::draw(&gui_remote, last_draw_elems);
    last_draw_elems.clear();
    drawelems_mtx.unlock();

    if (feedback.size() > 0)
    {
        logger->info("FeedBack %d", feedback.size());

        drawelems_mtx.lock();
        gui_buffer_tx.resize(65535);
        int len = RImGui::encode_vec(gui_buffer_tx.data(), feedback);
        gui_buffer_tx.resize(len);
        sendPacketWithVector(tcp_client, dsp::remote::PKT_TYPE_GUI, gui_buffer_tx);
        drawelems_mtx.unlock();
    }
}

void RemoteSource::set_samplerate(uint64_t samplerate)
{
    std::vector<uint8_t> pkt(8);
    *((uint64_t *)&pkt[0]) = samplerate;
    sendPacketWithVector(tcp_client, dsp::remote::PKT_TYPE_SAMPLERATESET, pkt);
}

uint64_t RemoteSource::get_samplerate()
{
    return samplerate_current;
}

std::vector<dsp::SourceDescriptor> RemoteSource::getAvailableSources()
{
    std::vector<dsp::SourceDescriptor> results;

    service_discovery::UDPDiscoveryConfig cfg = {REMOTE_NETWORK_DISCOVERY_PORT, REMOTE_NETWORK_DISCOVERY_REQPKT, REMOTE_NETWORK_DISCOVERY_REPPKT};

    auto detected_servers = service_discovery::discoverUDPServers(cfg, 100);

    for (auto server_ip : detected_servers)
    {
        logger->debug("Found server on %s:%d", server_ip.first.c_str(), server_ip.second);

        try
        {
            TCPClient tcp_client((char *)server_ip.first.c_str(), server_ip.second);

            tcp_client.readOne = true;
            tcp_client.callback_func = [&results, &server_ip](uint8_t *buf, int len)
            {
                if (buf[0] == dsp::remote::PKT_TYPE_SOURCELIST && len > 2)
                {
                    std::vector<uint8_t> pkt(buf + 1, buf + len);
                    for (dsp::SourceDescriptor &src : nlohmann::json::from_cbor(pkt).get<std::vector<dsp::SourceDescriptor>>())
                    {
                        src.source_type = "remote";
                        src.name = server_ip.first + ":" + std::to_string(server_ip.second) + " - " + src.name;
                        results.push_back(src);
                    }
                }
            };

            sendPacketWithVector(&tcp_client, dsp::remote::PKT_TYPE_SOURCELIST); // Request source list
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        catch (std::exception &e)
        {
            logger->error("Error connecting to Remote SDR Server : %s", e.what());
        }
    }

    return results;
}