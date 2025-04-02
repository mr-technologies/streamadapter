// std
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// json
#include <nlohmann/json.hpp>

// gstreamer
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

// IFF SDK
#include <iff.h>


constexpr char CONFIG_FILENAME[] = "streamadapter.json";
constexpr char FORMAT[] = "RGBA";
constexpr size_t BYTES_PER_PIXEL = 4;

int main(int argc, char* argv[])
{
    gst_init(&argc, &argv);
    std::string pipeline_description = "appsrc do-timestamp=TRUE is-live=TRUE name=streamadapter";
    for(int i = 1; i < argc; ++i)
    {
        pipeline_description += " ";
        pipeline_description += argv[i];
    }
    GstElement* pipeline = gst_parse_launch(pipeline_description.c_str(), NULL);
    if(!pipeline)
    {
        std::cerr << "Invalid pipeline description provided: " << pipeline_description << "\n";
        return EXIT_FAILURE;
    }
    GstElement* appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "streamadapter");
    if(!appsrc)
    {
        std::cerr << "Invalid pipeline description provided: no element with name `streamadapter`\n";
        return EXIT_FAILURE;
    }

    nlohmann::json config;
    try
    {
        config = nlohmann::json::parse(std::ifstream(CONFIG_FILENAME), nullptr, true, true);
    }
    catch(const std::exception& e)
    {
        std::cerr << "Invalid configuration provided: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    const auto it_chains = config.find("chains");
    if(it_chains == config.end())
    {
        std::cerr << "Invalid configuration provided: missing `chains` section\n";
        return EXIT_FAILURE;
    }
    if(!it_chains->is_array())
    {
        std::cerr << "Invalid configuration provided: section `chains` must be an array\n";
        return EXIT_FAILURE;
    }
    if(it_chains->size() != 1)
    {
        std::cerr << "Invalid configuration provided: section `chains` must contain exactly one element\n";
        return EXIT_FAILURE;
    }
    const auto it_iff = config.find("IFF");
    if(it_iff == config.end())
    {
        std::cerr << "Invalid configuration provided: missing `IFF` section\n";
        return EXIT_FAILURE;
    }

    iff_initialize(it_iff->dump().c_str());

    std::vector<iff_chain_handle_t> chain_handles;
    for(const auto& chain_config : *it_chains)
    {
        const auto chain_handle = iff_create_chain(chain_config.dump().c_str(),
                [](const char* const element_name, const int error_code, void*)
                {
                    std::ostringstream message;
                    message << "Chain element `" << element_name << "` reported an error: " << error_code;
                    iff_log(IFF_LOG_LEVEL_ERROR, "streamadapter", message.str().c_str());
                }, nullptr);
        chain_handles.push_back(chain_handle);
    }
    const auto total_chains = chain_handles.size();

    using exporter_t = std::function<void(const void*, size_t, const iff_image_metadata*)>;
    auto export_callbacks = std::vector<exporter_t>(total_chains);
    for(size_t i = 0; i < total_chains; ++i)
    {
        export_callbacks[i] = [appsrc, pipeline](const void* const data, const size_t size, const iff_image_metadata* const metadata)
        {
            static bool caps_set = false;
            if(!caps_set)
            {
                GstCaps* caps = gst_caps_new_simple("video/x-raw",
                                                    "width",       G_TYPE_INT,    metadata->width,
                                                    "height",      G_TYPE_INT,    metadata->height,
                                                    "format",      G_TYPE_STRING, FORMAT,
                                                    "framerate",   GST_TYPE_FRACTION, 0, 1,
                                                    NULL);
                gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
                gst_caps_unref(caps);
                const GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
                if(ret != GST_STATE_CHANGE_FAILURE)
                {
                    caps_set = true;
                }
                else
                {
                    iff_log(IFF_LOG_LEVEL_ERROR, "streamadapter", "Failed to switch GstPipeline into PLAYING state");
                }
            }
            const auto width_in_bytes = BYTES_PER_PIXEL * metadata->width;
            const auto pitch = width_in_bytes + metadata->padding;
            if(size < pitch * metadata->height)
            {
                std::ostringstream message;
                message << "Ignoring invalid buffer: " << metadata->width << "x" << metadata->height << "+" << metadata->padding << " " << size << " bytes";
                iff_log(IFF_LOG_LEVEL_WARNING, "streamadapter", message.str().c_str());
                return;
            }
            const auto buffer_size = width_in_bytes * metadata->height;
            GstBuffer* buffer = gst_buffer_new_allocate(0, buffer_size, nullptr);
            if(!buffer)
            {
                iff_log(IFF_LOG_LEVEL_ERROR, "streamadapter", "Failed to allocate GstBuffer");
                return;
            }
            if(metadata->padding == 0)
            {
                gst_buffer_fill(buffer, 0, data, buffer_size);
            }
            else
            {
                for(uint32_t j = 0; j < metadata->height; ++j)
                {
                    gst_buffer_fill(buffer, width_in_bytes * j, reinterpret_cast<const uint8_t*>(data) + pitch * j, width_in_bytes);
                }
            }
            const GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
            if(ret != GST_FLOW_OK)
            {
                iff_log(IFF_LOG_LEVEL_ERROR, "streamadapter", "Failed to push GstBuffer");
            }
        };
        const auto& chain_handle = chain_handles[i];
        iff_set_export_callback(chain_handle, "exporter",
                [](const void* const data, const size_t size, iff_image_metadata* const metadata, void* const private_data)
                {
                    const auto export_function = reinterpret_cast<const exporter_t*>(private_data);
                    (*export_function)(data, size, metadata);
                },
                &export_callbacks[i]);
        iff_execute(chain_handle, nlohmann::json{{"exporter", {{"command", "on"}}}}.dump().c_str(), [](const char*, void*){}, nullptr);
    }

    iff_log(IFF_LOG_LEVEL_INFO, "streamadapter", "Press Enter to terminate the program");
    std::getchar();

    for(const auto chain_handle : chain_handles)
    {
        iff_release_chain(chain_handle);
    }

    iff_finalize();

    gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(appsrc));
    gst_object_unref(GST_OBJECT(pipeline));

    return EXIT_SUCCESS;
}
