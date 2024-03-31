#ifdef WIN32
#include "stdafx.h"
#endif
#include "configuration-pimpl.hxx"
#include "postmark-pimpl.hxx"
#include "postmark-simpl.hxx"
#include "Postmarks.h"

#include <stdint.h>
#include <boost/asio.hpp>
#include <boost/optional.hpp>
#include <string>
#include <sstream>
#include <fstream>
#include <chrono>

const PubSub::Subject PUB_ALIVE{ "Alive", "Postmarks" };
const PubSub::Subject PUB_HERE{ "Here", "Postmarks" };
const PubSub::Subject PUB_DEAD{ "Dead", "Postmarks" };
const PubSub::Subject PUB_CFG_REQUEST{ "_", "CFG", "Request", "Postmarks" };

const PubSub::Subject SUB_CFG{ "CFG", "Postmarks" };
const PubSub::Subject SUB_PMREQ{ "_", "Postmark", "Request" };
const PubSub::Subject PUB_PMRSP{ "Postmark", "Response" };

#if defined(_DEBUG) && defined(WIN32)
const PubSub::Subject SUB_DIE
{ "Die", "Postmarks"
};
HANDLE g_exitEvent;
#endif

extern std::string g_version;

constexpr qpc_clock::duration TTL_LONGTIME{std::chrono::hours(-12)}; // up to 12 hrs or until superseded
constexpr qpc_clock::duration TTL_STATUS{std::chrono::minutes(1)};

Postmarks::Postmarks(Logging::LogFile& log, const std::string& psubAddr)
	: Task::TActiveTask<Postmarks>(2)
	, Logging::LogClient(log)
	, m_pubsubaddr(psubAddr)
	, m_sock(m_iosvc)
{
}

Postmarks::~Postmarks()
{
	stop();
}

bool Postmarks::start()
{
	LOG(Logging::LL_Debug, Logging::LC_Postmarks, "start");

	m_sockThread = std::thread([=] { socketThread(); });

	if (!getMsgDispatcher().started())
		getMsgDispatcher().start();

	return true;
}

void Postmarks::stop()
{
	LOG(Logging::LL_Debug, Logging::LC_Postmarks, "stop");
	sendMsg(PubSub::Message(PUB_DEAD, TTL_LONGTIME));

	if (m_here)
	{
		m_here->cancelMsg();
		m_here.reset();
	}

	while (getMsgDispatcher().started())
		getMsgDispatcher().stop();

	m_iosvc.stop();
	m_sockThread.join();
}

void Postmarks::socketThread()
{
	try
	{
		BA::io_service::work work(m_iosvc);
		initSock();
		m_iosvc.run();
	}
	catch (const std::exception& ex)
	{
		LOG(Logging::LL_Severe, Logging::LC_Postmarks, ex.what());
		throw ex;
	}
}

void Postmarks::initSock()
{
	std::string::size_type i = m_pubsubaddr.find(':');
	if (i == std::string::npos)
		connect(m_pubsubaddr, "3101");
	else
		connect(m_pubsubaddr.substr(0, i), m_pubsubaddr.substr(i + 1));
}

template <> void Postmarks::processEvent<Postmarks::evReconnect>(void)
{
	initSock();
}

void Postmarks::connect(const std::string& address, const std::string& port)
{
	namespace BIP = boost::asio::ip;

	std::shared_ptr<BIP::tcp::resolver> resolver = std::make_shared<BIP::tcp::resolver>(m_iosvc);

	auto connectHandler = [this] (const boost::system::error_code& errorCode, const BIP::tcp::endpoint& ep)
	{
		if (errorCode)
			onConnectionError("Could not connect: " + errorCode.message());
		else
			onConnected(ep);
	};

	auto resolveHandler = [resolver, connectHandler, this]
		(const boost::system::error_code& errorCode, const BIP::tcp::resolver::results_type results)
	{
		if (errorCode)
			onConnectionError("Could not resolve address: " + errorCode.message());
		else
			boost::asio::async_connect(m_sock, results, connectHandler);
	};

	resolver->async_resolve(address, port, resolveHandler);
}

void Postmarks::onConnected(const BA::ip::tcp::endpoint& ep)
{
	LOG(Logging::LL_Info, Logging::LC_PubSub, "Connected to pSub bus at " << ep);

	resetPSub();

	m_sock.set_option(boost::asio::socket_base::keep_alive(true));

	// Subscribe to stuff
	subscribe(SUB_CFG);
	subscribe(SUB_PMREQ);
#if defined(_DEBUG) && defined(WIN32)
	subscribe(SUB_DIE);
#endif
	sendMsg(PubSub::Message(PUB_ALIVE, g_version, TTL_LONGTIME));

	if (!haveCfg)
		m_cfgAliveDeferred = enqueueWithDelay<evCfgDeferred>(3s);

	if (m_here)
		m_here->cancelMsg();
	m_here = enqueueWithDelay<evHereTime>(2s, true);

	m_sock.async_read_some(BA::buffer(readBuff, 1024), [&](const boost::system::error_code& error, size_t bytes){ OnReadSome(error, bytes); });
}

void Postmarks::onConnectionError(const std::string& error)
{
	LOG(Logging::LL_Warning, Logging::LC_PubSub, error << " Reconnect in 1 second");
	enqueueWithDelay<evReconnect>(1s);
}

void Postmarks::OnReadSome(const boost::system::error_code& error, size_t bytes_transferred)
{
	if (!error)
	{
		try
		{
			processBuffer((char*)readBuff, bytes_transferred);
		}
		catch (const std::exception& ex)
		{
			LOG(Logging::LL_Warning, Logging::LC_PubSub, "Error processing pSub buffer. Read buffers reset: " << ex.what());
			LOG(Logging::LL_Dump, Logging::LC_PubSub, readBuff);
		}

		m_sock.async_read_some(BA::buffer(readBuff, 1024), [&](const boost::system::error_code& error, size_t bytes){ OnReadSome(error, bytes); });
	}
	else
	{
		LOG(Logging::LL_Warning, Logging::LC_PubSub, "Lost connection to pSub bus");

		resetPSub();
		if (m_here)
		{
			m_here->cancelMsg();
			m_here.reset();
		}
		enqueueWithDelay<evReconnect>(1s);
	}
}

void Postmarks::configure(const std::string& cfgStr)
{
	PmConfig::Postmarks_paggr s;
	xml_schema::document_pimpl d(s.root_parser(), s.root_name());

	std::istringstream cfgstrm(cfgStr);

	s.pre();

	try
	{
		if (!haveCfg)
		{
			d.parse(cfgstrm);

			std::unique_lock<std::recursive_mutex> sync(m_lk);

			s.post()->_copy(m_cfg);

			m_postmarks.clear();

			for (const PmConfig::Range& r : m_cfg.range())
			{
				m_postmarks.push_back({ std::regex(r.regex()), Postmarks_t() });
				Postmarks_t::NumericRangeList rangelist;

				rangelist += Postmarks_t::NumericRange(r.from(), r.to());
				for (const Postmarks_t::NumericRange& n : rangelist.getRangeSet())
					m_postmarks.back().second += n;
			}

			//int rc = sqlite3_open_v2(m_cfg.DbFile().c_str(), &m_pmdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
			int rc = sqlite3_open(m_cfg.DbFile().c_str(), &m_pmdb);
			if (rc)
			{
				LOG(Logging::LL_Warning, Logging::LC_Postmarks, "Can't open database: " << sqlite3_errmsg(m_pmdb));
				sqlite3_close(m_pmdb);
				return;
			}

			char* err = nullptr;
			rc = sqlite3_exec(m_pmdb, "CREATE TABLE IF NOT EXISTS postmarks (pm INTEGER PRIMARY KEY, device TEXT UNIQUE)", nullptr, nullptr, &err);
			if (rc)
			{
				LOG(Logging::LL_Warning, Logging::LC_Postmarks, "Error creating tables: " << err);
				sqlite3_free(err);
			}

			sqlite3_stmt* stmt;
			sqlite3_prepare_v2(m_pmdb, "SELECT * FROM Postmarks", 24, &stmt, nullptr);

			bool done = false;
			while (!done)
			{
				switch (sqlite3_step(stmt))
				{
				case SQLITE_ROW:
					{
						uint32_t assigned = Postmarks_t::MAX_N;

						for (regex_pm_t::value_type& v : m_postmarks)
						{
							if (!v.second.full())
							{
								if (assigned != Postmarks_t::MAX_N)
								{
									v.second.addNum(assigned);
									continue;
								}

								if (std::regex_match((const char*)sqlite3_column_text(stmt, 1), v.first))
								{
									if (v.second.addNum(sqlite3_column_int(stmt, 0)))
										assigned = sqlite3_column_int(stmt, 0);
								}
							}
						}

						if (assigned == Postmarks_t::MAX_N)
						{
							// record failed to pass current config rules
							// discard and publish
							std::stringstream sql;
							sql << "DELETE FROM Postmarks WHERE device = '" << (const char*)sqlite3_column_text(stmt, 1) << "'";
							sqlite3_exec(m_pmdb, sql.str().c_str(), nullptr, nullptr, nullptr);

							postmarks::pmRsp r;
							r.devId((const char*)sqlite3_column_text(stmt, 1));
							r.pm_present(false);
							enqueue(r);
						}
					}
					break;
				case SQLITE_DONE:
					done = true;
					break;
				default:
					LOG(Logging::LL_Warning, Logging::LC_Postmarks, "Error creating reading postmarks");
					break;
				}
			}

			sqlite3_finalize(stmt);

			haveCfg = true;
		}
	}
	catch (const xml_schema::parser_exception& ex)
	{
		std::stringstream err;
		err << "parser_exception " << ex.text() << " " << ex.what() << " at " << (int)ex.line() << ":" << (int)ex.column();

		LOG(Logging::LL_Warning, Logging::LC_Postmarks, "CONFIG ERROR: The following errors were found:\r\n" << err.str());
		sendMsg(PubSub::Message{{ "Error", "Postmarks", "Config" }, err.str()});
	}
}

template <> void Postmarks::processEvent<Postmarks::evCfgDeferred>()
{
	if (!haveCfg)
	{
		sendMsg(PubSub::Message(PUB_CFG_REQUEST, g_version));
		m_cfgAliveDeferred = enqueueWithDelay<evCfgDeferred>(3s);
	}
}

template <> void Postmarks::processEvent<Postmarks::evHereTime>()
{
	sendMsg(PubSub::Message(PUB_HERE));
}

void Postmarks::processMsg(PubSub::Message&& m)
{
	std::string str;
	LOG(Logging::LL_Debug, Logging::LC_Postmarks, "Received msg " << PubSub::toString(m.subject, str));

	std::unique_lock<std::recursive_mutex> syn(m_lk);

	if (PubSub::match(SUB_CFG, m.subject))
		configure(m.payload);
#if defined(_DEBUG) && defined(WIN32)
	else if (PubSub::match(SUB_DIE, m.subject))
		SetEvent(g_exitEvent);
#endif
	else if (PubSub::match(SUB_PMREQ, m.subject))
		assignPostmark(m.payload);
	else
		// Unknown message - weird
		LOG(Logging::LL_Warning, Logging::LC_Postmarks, "Received unknown msg " << PubSub::toString(m.subject, str));
	;
}

bool Postmarks::getStoredPostmark(postmarks::pmRsp& rsp)
{
	sqlite3_stmt* stmt;
	std::stringstream sql;
	sql << "SELECT * FROM Postmarks WHERE device = '" << rsp.devId() << "'";
	sqlite3_prepare_v2(m_pmdb, sql.str().c_str(), sql.str().size(), &stmt, nullptr);

	bool found = false;
	//while (!done)
	//{
	switch (sqlite3_step(stmt))
	{
	case SQLITE_ROW:
		rsp.pm(sqlite3_column_int(stmt, 0));
		found = true;
		break;
	case SQLITE_DONE:
		break;
	default:
		LOG(Logging::LL_Warning, Logging::LC_Postmarks, "Error creating reading postmarks");
		break;
	}
	//}

	sqlite3_finalize(stmt);
	return found;
}

bool Postmarks::updStoredPostmark(postmarks::pmRsp& rsp)
{
	if (!rsp.pm_present())
		return false;

	uint32_t pm = rsp.pm();
	if (getStoredPostmark(rsp))
	{
		std::stringstream sql;
		sql << "UPDATE Postmarks SET pm = " << rsp.pm() << " WHERE device = '" << rsp.devId() << "'";
		sqlite3_exec(m_pmdb, sql.str().c_str(), nullptr, nullptr, nullptr);
	}
	else
	{
		std::stringstream sql;
		sql << "INSERT INTO Postmarks VALUES (" << rsp.pm() << ",'" << rsp.devId() << "')";
		sqlite3_exec(m_pmdb, sql.str().c_str(), nullptr, nullptr, nullptr);
	}
	return sqlite3_changes(m_pmdb) > 0;
}

void Postmarks::assignPostmark(const std::string& reqStr)
{
	postmarks::pmReq_paggr s;
	xml_schema::document_pimpl d(s.root_parser(), s.root_name());

	s.pre();

	try
	{
		std::istringstream reqstrm(reqStr);
		d.parse(reqstrm);

		std::unique_lock<std::recursive_mutex> sync(m_lk);

		postmarks::pmReq req = s.post();
		if (req.devId().empty())
			return;  // Do nothing

		postmarks::pmRsp rsp;
		rsp.devId(req.devId());

		uint32_t assigned = Postmarks_t::MAX_N;
		auto assign = [&]()
		{
			for (regex_pm_t::value_type& v : m_postmarks)
			{
				if (!v.second.full())
				{
					if (assigned != Postmarks_t::MAX_N)
					{
						v.second.addNum(assigned);
						continue;
					}

					if (std::regex_match(req.devId(), v.first))
					{
						if (req.requested_present() && !v.second.contains(req.requested()))
						{
							v.second.addNum(req.requested());
							rsp.pm(req.requested());
						}
						else
							rsp.pm(v.second.addLowestUnused());

						assigned = rsp.pm();
					}
				}
			}
		};

		// Check to see if the devId is already in the db
		if (getStoredPostmark(rsp))
		{
			if (req.requested_present() && rsp.pm() != req.requested())
			{
				for (regex_pm_t::value_type& v : m_postmarks)
					v.second.removeNum(rsp.pm());

				assign();
				if (assigned != Postmarks_t::MAX_N)
					updStoredPostmark(rsp);
			}
		}
		else
		{
			assign();
			if (assigned != Postmarks_t::MAX_N)
				updStoredPostmark(rsp);
		}

		enqueue(rsp);
	}
	catch (xml_schema::parser_exception& ex)
	{
		LOG(Logging::LL_Warning, Logging::LC_Postmarks, "CONFIG ERROR: The following errors were found:\r\n" << ex.what());

		PubSub::Message err;
		err.subject = { "Error", "Updates", "Config" };
		err.payload = ex.text();
		err.payload += " ";
		err.payload += ex.what();

		sendMsg(err);
	}
}

void Postmarks::processMsg(const postmarks::pmRsp& rsp)
{
	PubSub::Message msg;
	msg.subject = PUB_PMRSP;
	msg.ttl = TTL_LONGTIME;

	try
	{
		postmarks::pmRsp_saggr rsp_s;
		xml_schema::document_simpl rsp_d(rsp_s.root_serializer(), rsp_s.root_name());

		std::ostringstream rspstrm;
		rsp_s.pre(rsp);
		rsp_d.serialize(rspstrm, 0);

		msg.payload = rspstrm.str();

		sendMsg(msg);
	}
	catch (xml_schema::serializer_xml& ex)
	{
		LOG(Logging::LL_Warning, Logging::LC_Postmarks, "serializer_xml exception: " << ex.text());
	}
	catch (xml_schema::serializer_schema& ex)
	{
		LOG(Logging::LL_Warning, Logging::LC_Postmarks, "serializer_schema exception: " << ex.text());
	}
}

namespace Logging
{
	template <> const char* getLCStr<LC_Task      >()
	{
		return "Task       ";
	}
	template <> const char* getLCStr<LC_PubSub    >()
	{
		return "PubSub     ";
	}
	template <> const char* getLCStr<LC_TcpConn   >()
	{
		return "TcpConn    ";
	}
	template <> const char* getLCStr<LC_Postmarks >()
	{
		return "Postmarksr ";
	}
}
