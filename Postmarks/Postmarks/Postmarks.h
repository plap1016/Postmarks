#pragma once

#include "Logging/Log.h"
#include "Task/TTask.h"
#include "PubSubLib/PubSub.h"
#include "pugixml/pugixml.hpp"
#include "sqlite3.h"
#include "NumericRangeHandler.h"
#include "configuration.hxx"
#include "postmark.hxx"

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <set>
#include <vector>
#include <chrono>
#include <thread>
#include <memory>
#include <mutex>
#include <istream>

#if defined(_DEBUG) && defined(WIN32)
extern HANDLE g_exitEvent;
#endif

namespace Logging
{
	const uint32_t LC_Task = 0x0001;
	const uint32_t LC_PubSub = 0x0002;
	const uint32_t LC_TcpConn = 0x0004;
	const uint32_t LC_Postmarks = 0x0008;
}

namespace BA = boost::asio;

class Postmarks : public Task::TActiveTask<Postmarks>, public PubSub::TPubSubClient<Postmarks>, public Logging::LogClient
{
	friend PubSub::TPubSubClient<Postmarks>;

	std::recursive_mutex m_lk; // General lock on dispatcher state
	std::recursive_mutex m_socklk; // General lock on socket

	uint8_t readBuff[1024];

	std::thread m_sockThread;
	void socketThread();
	BA::io_service m_iosvc;
	std::string m_pubsubaddr;
	BA::ip::tcp::socket m_sock;
	void initSock();
	void connect(const std::string& address, const std::string& port);
	void onConnected(const BA::ip::tcp::endpoint& ep);
	void onConnectionError(const std::string& error);
	void OnReadSome(const boost::system::error_code& error, size_t bytes_transferred);
	void receivePSub(PubSub::Message&& msg) { /*hand off to thread queue*/enqueue(msg); }
	void sendBuffer(const std::string& buff)
	{
		try
		{
			BA::write(m_sock, BA::buffer(frameMsg(buff)));
		}
		catch (const boost::exception&)
		{
			// do nothing - the OnReadSome method will handle it
		}
	};

	std::recursive_mutex m_dispLock;
	PmConfig::Postmarks m_cfg;
	void configure(const std::string& cfgStr);
	bool haveCfg = false;
	void assignPostmark(const std::string& req);
	bool getStoredPostmark(postmarks::pmRsp& rsp);
	bool updStoredPostmark(postmarks::pmRsp& rsp);

	Task::MsgDelayMsgPtr m_cfgAliveDeferred;
	Task::MsgDelayMsgPtr m_here;

	typedef Nmrh::NumericRangeHandler<uint32_t> Postmarks_t;
	typedef std::vector<std::pair<std::regex, Postmarks_t> > regex_pm_t;
	regex_pm_t m_postmarks;

	sqlite3* m_pmdb;

public:
	explicit Postmarks(Logging::LogFile& log, const std::string& psubAddr = "127.0.0.1");
	~Postmarks();

	bool start();
	void stop();

	struct evHereTime;
	struct evReconnect;
	struct evCfgDeferred;
	template <typename M> void processEvent();

	void processMsg(const PubSub::Message& m);
	void processMsg(const postmarks::pmRsp& rsp);
};

