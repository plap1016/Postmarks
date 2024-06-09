#pragma once

#include "Logging/Log.h"
#include "Task/TTask.h"
#include "HubApp/HubApp.h"
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

extern std::string g_version;

namespace Logging
{
	const uint32_t LC_Task = 0x0100;
	const uint32_t LC_Postmarks = 0x0200;
}

namespace BA = boost::asio;

class Postmarks : public Task::TActiveTask<Postmarks>, public Logging::LogClient
{
	std::recursive_mutex m_lk; // General lock on dispatcher state

	friend HubApps::HubApp;
	HubApps::HubApp m_hub;
	void receiveEvent(PubSub::Message&& msg) { /*hand off to thread queue*/enqueue<PubSub::Message&&>(std::move(msg)); }
	void receiveUnknown(uint8_t, const std::string&) {}
	void eventBusConnected(HubApps::HubConnectionState state);

	std::recursive_mutex m_dispLock;
	PmConfig::Postmarks m_cfg;
	void configure(const std::string& cfgStr);
	bool haveCfg = false;
	void assignPostmark(const std::string& req);
	bool getStoredPostmark(postmarks::pmRsp& rsp);
	bool updStoredPostmark(postmarks::pmRsp& rsp);

	typedef Nmrh::NumericRangeHandler<uint32_t> Postmarks_t;
	typedef std::vector<std::pair<std::regex, Postmarks_t> > regex_pm_t;
	regex_pm_t m_postmarks;

	sqlite3* m_pmdb;

public:
	explicit Postmarks(Logging::LogFile& log, const std::string& psubAddr = "127.0.0.1");
	~Postmarks();

	bool start();
	void stop();

	static constexpr const char* appName() { return "Postmarks"; }
	constexpr std::string& version() const { return g_version; }

	void processMsg(PubSub::Message&& m);
	void processMsg(const postmarks::pmRsp& rsp);
};

