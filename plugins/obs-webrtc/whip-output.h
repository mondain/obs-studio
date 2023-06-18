#pragma once
#include <obs-module.h>
#include <obs-avc.h>
#include <obs-nal.h>

#include <util/curl/curl-helper.h>
#include <util/array-serializer.h>
#include <util/platform.h>
#include <util/base.h>
#include <util/dstr.h>

#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <iostream>
#include <cstdint>
#include <vector>

#include <rtc/rtc.h>

#define do_log(level, format, ...)                              \
	blog(level, "[obs-webrtc] [whip_output: '%s'] " format, \
	     obs_output_get_name(output), ##__VA_ARGS__)
#define do_log_s(level, format, ...)                            \
	blog(level, "[obs-webrtc] [whip_output: '%s'] " format, \
	     obs_output_get_name(whipOutput->output), ##__VA_ARGS__)

class WHIPOutput {
public:
	WHIPOutput(obs_data_t *settings, obs_output_t *output);
	~WHIPOutput();

	bool Start();
	void Stop(bool signal = true);
	void Data(struct encoder_packet *packet);

	inline size_t GetTotalBytes() { return total_bytes_sent; }

	inline int GetConnectTime() { return connect_time_ms; }

private:
	void ConfigureAudioTrack(std::string media_stream_id,
				 std::string cname);
	void ConfigureVideoTrack(std::string media_stream_id,
				 std::string cname);
    bool Init();
	bool Setup();
	bool Connect();
	void StartThread();
    void SendOptions();
	void SendDelete();
	void StopThread(bool signal);

	void Send(void *data, uintptr_t size, uint64_t duration, int track);

	obs_output_t *output;

	std::string endpoint_url;
	std::string bearer_token;
	std::string resource_url;
    // turn server url and any params
	std::string turn_url;

	std::atomic<bool> running;
	
	// whether or not we've collected video private data
	std::atomic<bool> got_critical_video;
	// sprops for h264
	std::string sprop_parameter_sets;

	std::mutex start_stop_mutex;
	std::thread start_stop_thread;

	int peer_connection;
	int audio_track;
	int video_track;

	std::atomic<size_t> total_bytes_sent;
	std::atomic<int> connect_time_ms;
	int64_t start_time_ns;
	int64_t last_audio_timestamp;
	int64_t last_video_timestamp;
};

void register_whip_output();

static std::string trim_string(const std::string &source)
{
	std::string ret(source);
	ret.erase(0, ret.find_first_not_of(" \n\r\t"));
	ret.erase(ret.find_last_not_of(" \n\r\t") + 1);
	return ret;
}

static size_t curl_writefunction(char *data, size_t size, size_t nmemb,
				 void *priv_data)
{
	auto read_buffer = static_cast<std::string *>(priv_data);

	size_t real_size = size * nmemb;

	read_buffer->append(data, real_size);
	return real_size;
}

#define LOCATION_HEADER_LENGTH 10

static size_t curl_header_location_function(char *data, size_t size, size_t nmemb,
				  void *priv_data)
{
	auto header_buffer = static_cast<std::string *>(priv_data);

	size_t real_size = size * nmemb;

	if (real_size < LOCATION_HEADER_LENGTH)
		return real_size;

	if (!astrcmpi_n(data, "location: ", LOCATION_HEADER_LENGTH)) {
		char *val = data + LOCATION_HEADER_LENGTH;
		header_buffer->append(val, real_size - LOCATION_HEADER_LENGTH);
		*header_buffer = trim_string(*header_buffer);
	}

	return real_size;
}

#define LINK_HEADER_LENGTH 6

static size_t curl_header_link_function(char *data, size_t size, size_t nmemb,
				  void *priv_data)
{
	auto header_buffer = static_cast<std::string *>(priv_data);

	size_t real_size = size * nmemb;

	if (real_size < LINK_HEADER_LENGTH)
		return real_size;

	if (!astrcmpi_n(data, "link: ", LINK_HEADER_LENGTH)) {
		char *val = data + LINK_HEADER_LENGTH;
		header_buffer->append(val, real_size - LINK_HEADER_LENGTH);
		*header_buffer = trim_string(*header_buffer);
	}

	return real_size;
}

static std::vector<std::vector<uint8_t>> parse_h264_nals(const char* data, size_t length) {
    std::vector<std::vector<uint8_t>> nalus;

    const char* end = data + length;
    const char* start = data;
    const char* current = data;

    while (current < end) {
        // Find the start code prefix (0x000001 or 0x00000001)
        if (current[0] == 0x00 && current[1] == 0x00 && (current[2] == 0x01 || (current[2] == 0x00 && current[3] == 0x01))) {
            if (start < current) {
                // Extract the NALU unit
                size_t nalSize = current - start;
                nalus.emplace_back(start, start + nalSize);
            }

            // Move the start pointer to the next NALU
            start = current + 3;
            if (current[2] == 0x00)
                start++;

            // Skip the start code prefix
            current += 3;
            if (current[0] == 0x00)
                current++;
        } else {
            current++;
        }
    }

    // Extract the last NALU unit
    size_t nalSize = current - start;
    if (nalSize > 0)
        nalus.emplace_back(start, start + nalSize);

    return nalus;
}