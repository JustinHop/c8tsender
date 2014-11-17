#include "chromecast.hpp"
#include "cast_channel.pb.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

ChromeCast::ChromeCast(const std::string& ip)
{
	m_s = socket(PF_INET, SOCK_STREAM, 0);
	if (m_s == -1)
		throw std::runtime_error("could not connect");
	struct sockaddr_in inaddr;
	memset(&inaddr, 0, sizeof inaddr);
	inaddr.sin_family = AF_INET;
	inaddr.sin_port = htons(8009);
	inaddr.sin_addr.s_addr = inet_addr(ip.c_str());
	if (connect(m_s, (const struct sockaddr *)&inaddr,
				sizeof inaddr) != 0)
		throw std::runtime_error("could not connect");

	SSL_CTX* x = SSL_CTX_new(TLSv1_client_method());
	m_ssls = SSL_new(x);
	SSL_set_fd(m_ssls, m_s);
	SSL_set_mode(m_ssls, SSL_get_mode(m_ssls) | SSL_MODE_AUTO_RETRY);
	SSL_set_connect_state(m_ssls);
	if (SSL_connect(m_ssls) != 1)
		throw std::runtime_error("could not connect ssl");
	m_tread = std::thread(&ChromeCast::_read, this);
}

ChromeCast::~ChromeCast()
{
	m_tread.join();
	if (m_s != -1)
		close(m_s);
}

void ChromeCast::setMediaStatusCallback(std::function<void(const std::string&,
			const std::string&, const std::string&)> func)
{
	m_mediaStatusCallback = func;
}

unsigned int ChromeCast::_request_id()
{
	unsigned int request_id;
	m_mutex.lock();
	request_id = m_request_id++;
	m_mutex.unlock();
	return request_id;
}

void ChromeCast::init()
{
	Json::Value response;
	{
		Json::Value msg;
		msg["type"] = "CONNECT";
		msg["origin"] = Json::Value(Json::objectValue);
		send("urn:x-cast:com.google.cast.tp.connection", msg);
	}
	{
		Json::Value msg;
		msg["type"] = "GET_STATUS";
		msg["requestId"] = _request_id();
		response = send("urn:x-cast:com.google.cast.receiver", msg);
		bool isRunning = false;
		if (response.isMember("status") &&
			response["status"].isMember("applications") &&
			response["status"]["applications"].isValidIndex(0u)) {
			Json::Value& application = response["status"]["applications"][0u];
			if (application["appId"].asString() == "CC1AD845")
				isRunning = true;
		}
		if (!isRunning)
		{
			Json::Value msg;
			msg["type"] = "LAUNCH";
			msg["requestId"] = _request_id();
			msg["appId"] = "CC1AD845";
			response = send("urn:x-cast:com.google.cast.receiver", msg);
		}
	}
	if (response.isMember("status") &&
			response["status"].isMember("applications") &&
			response["status"]["applications"].isValidIndex(0u))
	{
		Json::Value& application = response["status"]["applications"][0u];
		m_destination_id = application["transportId"].asString();
		m_session_id = application["sessionId"].asString();
	}
	{
		Json::Value msg;
		msg["type"] = "CONNECT";
		msg["origin"] = Json::Value(Json::objectValue);
		send("urn:x-cast:com.google.cast.tp.connection", msg);
	}
}

void ChromeCast::load(const std::string& url, const std::string& title, const std::string& uuid)
{
	Json::Value msg;
	msg["type"] = "LOAD";
	msg["requestId"] = _request_id();
	msg["sessionId"] = m_session_id;
	msg["media"]["contentId"] = url;
	msg["media"]["streamType"] = "buffered";
	msg["media"]["contentType"] = "video/x-matroska";
	msg["media"]["customData"] = uuid;
	msg["media"]["metadata"]["title"] = title;
	msg["autoplay"] = true;
	msg["currentTime"] = 0;
	send("urn:x-cast:com.google.cast.media", msg);
}

void ChromeCast::pause()
{
	Json::Value msg;
	msg["type"] = "PAUSE";
	msg["requestId"] = _request_id();
	msg["mediaSessionId"] = m_media_session_id;
	send("urn:x-cast:com.google.cast.media", msg);
}

void ChromeCast::play()
{
	Json::Value msg;
	msg["type"] = "PLAY";
	msg["requestId"] = _request_id();
	msg["mediaSessionId"] = m_media_session_id;
	send("urn:x-cast:com.google.cast.media", msg);
}

void ChromeCast::stop()
{
	Json::Value msg;
	msg["type"] = "STOP";
	msg["requestId"] = _request_id();
	msg["mediaSessionId"] = m_media_session_id;
	send("urn:x-cast:com.google.cast.media", msg);
}

Json::Value ChromeCast::send(const std::string& namespace_, const Json::Value& payload)
{
	Json::FastWriter fw;
	fw.omitEndingLineFeed();

	extensions::core_api::cast_channel::CastMessage msg;
	msg.set_payload_type(msg.STRING);
	msg.set_protocol_version(msg.CASTV2_1_0);
	msg.set_namespace_(namespace_);
	msg.set_source_id(m_source_id);
	msg.set_destination_id(m_destination_id);
	msg.set_payload_utf8(fw.write(payload));

	std::string data, foo;
	char pktlen[4];
	unsigned int len = htonl(msg.ByteSize());
	memcpy(&pktlen, &len, sizeof len);
	foo.append(pktlen, sizeof pktlen);
	msg.SerializeToString(&data);
	foo += data;

	std::condition_variable f;
	bool wait = false;
	unsigned int requestId;
	if (payload.isMember("requestId"))
	{
		requestId = payload["requestId"].asUInt();
		m_mutex.lock();
		m_wait[requestId] = std::make_pair(&f, std::string());
		wait = true;
		m_mutex.unlock();
	}

	printf("%ld: %s -> %s (%s): %s\n",
			time(NULL),
			msg.source_id().c_str(),
			msg.destination_id().c_str(),
			msg.namespace_().c_str(),
			msg.payload_utf8().c_str()
		  );
	int w = SSL_write(m_ssls, foo.c_str(), foo.size());

	Json::Value ret;
	if (wait) {
		std::unique_lock<std::mutex> wl(m_mutex);
		f.wait(wl);
		ret = m_wait[requestId].second;
		m_wait.erase(requestId);
		m_mutex.unlock();
	}

	return ret;
}

void ChromeCast::_read()
{
	while (true)
	{
		char pktlen[4];
		for (int r = 0; r < sizeof pktlen; ++r) {
			if (SSL_read(m_ssls, pktlen + r, 1) < 1)
				return;
		}

		unsigned int len;
		memcpy(&len, pktlen, sizeof len);
		len = ntohl(len);

		std::string buf;
		while (buf.size() < len) {
			char b[2048];
			int r = SSL_read(m_ssls, b, sizeof b);
			if (r < 1)
				break;
			buf.append(b, r);
		}

		extensions::core_api::cast_channel::CastMessage msg;
		msg.ParseFromString(buf);

		printf("%ld: %s -> %s (%s): %s\n",
				time(NULL),
				msg.source_id().c_str(),
				msg.destination_id().c_str(),
				msg.namespace_().c_str(),
				msg.payload_utf8().c_str()
			  );

		Json::Value response;
		Json::Reader reader;
		if (!reader.parse(msg.payload_utf8(), response, false))
			continue;

		if (msg.namespace_() == "urn:x-cast:com.google.cast.tp.heartbeat")
		{
			Json::FastWriter fw;
			fw.omitEndingLineFeed();

			extensions::core_api::cast_channel::CastMessage reply(msg);
			Json::Value msg;
			msg["type"] = "PONG";
			reply.set_payload_utf8(fw.write(msg));
			std::string data;
			reply.SerializeToString(&data);
			unsigned int len = htonl(data.size());
			SSL_write(m_ssls, &len, sizeof len);
			SSL_write(m_ssls, data.c_str(), data.size());
			continue;
		}

		if (msg.namespace_() == "urn:x-cast:com.google.cast.media")
		{
			if (response["type"].asString() == "MEDIA_STATUS")
			{
				if (response.isMember("status") &&
					response["status"].isValidIndex(0u))
				{
					Json::Value& status = response["status"][0u];
					m_media_session_id = status["mediaSessionId"].asUInt();
				}
			}
		}

		if (response.isMember("requestId"))
		{
			m_mutex.lock();
			auto waitIter = m_wait.find(response["requestId"].asUInt());
			if (waitIter != m_wait.end())
			{
				waitIter->second.second = response;
				waitIter->second.first->notify_all();
			}
			m_mutex.unlock();
		}

		if (msg.namespace_() == "urn:x-cast:com.google.cast.media")
		{
			if (response["type"].asString() == "MEDIA_STATUS")
			{
				if (response.isMember("status") &&
					response["status"].isValidIndex(0u))
				{
					std::string customdata;
					Json::Value& status = response["status"][0u];
					if (m_mediaStatusCallback)
						m_mediaStatusCallback(status["playerState"].asString(),
								status["idleReason"].asString(),
								status["media"]["customData"].asString());
				}
			}
		}
	}
}

std::string ChromeCast::getSocketName() const
{
	struct sockaddr_in addr;
    socklen_t len = sizeof(struct sockaddr);
	getsockname(m_s, (struct sockaddr*)&addr, &len);
	return inet_ntoa(addr.sin_addr);
}