/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 */

#include "AI/LivingNPC/AIService.h"
#include "Define.h"
#include "Errors.h"
#include "Log.h"

#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <openssl/ssl.h>
#include <boost/beast/core.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/strand.hpp>
#include <thread>
#include <mutex>
#include <atomic>

namespace ba = boost::asio;
namespace bs = boost::asio::ssl;
namespace bh = boost::beast::http;
using tcp = boost::asio::ip::tcp;
using ssl_stream = bs::stream<tcp::socket>;

AIService AIService::s_instance;
AIService* AIService::Get() { return &s_instance; }

namespace
{
    std::string JsonEscape(std::string const& in)
    {
        std::string out;
        out.reserve(in.size() + 8);
        for (char c : in)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        static const char* hex = "0123456789abcdef";
                        out += "\\u00";
                        out += hex[(unsigned char)c >> 4];
                        out += hex[(unsigned char)c & 0xF];
                    }
                    else
                        out += c;
            }
        }
        return out;
    }

    bool FindJsonString(std::string const& body, std::string const& key, std::string& out)
    {
        std::string needle = "\"" + key + "\":";
        size_t pos = body.find(needle);
        if (pos == std::string::npos)
            return false;
        pos = body.find('"', pos + needle.size());
        if (pos == std::string::npos)
            return false;
        size_t start = pos + 1;
        std::string val;
        while (start < body.size())
        {
            char c = body[start];
            if (c == '"')
                break;
            if (c == '\\' && start + 1 < body.size())
            {
                char n = body[start + 1];
                switch (n)
                {
                    case 'n': val += '\n'; break;
                    case 't': val += '\t'; break;
                    case 'r': val += '\r'; break;
                    case '"': val += '"'; break;
                    case '\\': val += '\\'; break;
                    case '/': val += '/'; break;
                    case 'b': val += '\b'; break;
                    case 'f': val += '\f'; break;
                    case 'u':
                    {
                        if (start + 5 < body.size())
                        {
                            unsigned int cp = 0;
                            for (int i = 0; i < 4; ++i)
                            {
                                char h = body[start + 2 + i];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= (h - '0');
                                else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
                            }
                            if (cp < 0x80)
                                val += static_cast<char>(cp);
                            else if (cp < 0x800)
                            {
                                val += static_cast<char>(0xC0 | (cp >> 6));
                                val += static_cast<char>(0x80 | (cp & 0x3F));
                            }
                            else
                            {
                                val += static_cast<char>(0xE0 | (cp >> 12));
                                val += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                val += static_cast<char>(0x80 | (cp & 0x3F));
                            }
                            start += 6;
                            continue;
                        }
                        break;
                    }
                    default: val += n; break;
                }
                start += 2;
                continue;
            }
            val += c;
            ++start;
        }
        out = std::move(val);
        return true;
    }
}

struct AIService::Impl
{
    ba::io_context ioc;
    // Keeps ioc.run() alive between requests (Boost 1.70+: executor_work_guard
    // replaces the deprecated io_context::work). Reset in Stop().
    std::unique_ptr<ba::executor_work_guard<ba::io_context::executor_type>> work;
    std::unique_ptr<std::thread> worker;
    std::atomic<bool> running{ false };

    void Run()
    {
        work = std::make_unique<ba::executor_work_guard<ba::io_context::executor_type>>(ioc.get_executor());
        running = true;
        try { ioc.run(); }
        catch (std::exception const& e)
        {
            TC_LOG_ERROR("AISystem", "AIService worker exception: {}", e.what());
        }
        running = false;
    }
};

// Per-request operation state. Holds a shared_ptr to itself (passed as `self`) so it
// lives until the async chain completes.
struct AIRequestOp
{
    AIService::Impl* impl;
    bs::context sslCtx;
    tcp::resolver resolver;
    std::unique_ptr<ssl_stream> stream;
    bh::request<bh::string_body> req;
    boost::beast::flat_buffer buffer;
    bh::response<bh::string_body> res;
    std::string host;
    AIChatCallback cb;

    explicit AIRequestOp(AIService::Impl* i, std::string const& h, AIChatCallback callback)
        : impl(i), sslCtx(bs::context::tls_client), resolver(i->ioc),
          host(h), cb(std::move(callback))
    {
        // Dev convenience: don't validate the server cert (Windows OpenSSL has no
        // default CA bundle). SNI is still required for Cloudflare-fronted hosts.
        sslCtx.set_verify_mode(bs::verify_none);
    }

    void Fail(std::string const& what)
    {
        if (cb) cb(false, "", what);
    }

    void Begin(std::shared_ptr<AIRequestOp> self, std::string const& apiKey,
               std::string const& model, std::vector<AIMessage> const& messages,
               float temperature, uint32 maxTokens)
    {
        req.version(11);
        req.method(bh::verb::post);
        req.target("/api/v1/chat/completions");
        req.set(bh::field::host, host);
        req.set(bh::field::user_agent, "TrinityCore-LivingNPC");
        req.set(bh::field::content_type, "application/json");
        req.set("Authorization", "Bearer " + apiKey);
        req.set("HTTP-Referer", "https://trinitycore.org");
        req.set("X-Title", "TrinityCore LivingNPC");
        req.body() = AIService::BuildJson(model, messages, temperature, maxTokens);
        req.prepare_payload();

        resolver.async_resolve(host, "443",
            [self](boost::system::error_code ec, tcp::resolver::results_type results)
            {
                if (ec) { self->Fail("resolve: " + ec.message()); return; }
                self->stream = std::make_unique<ssl_stream>(self->impl->ioc, self->sslCtx);
                ba::async_connect(self->stream->next_layer(), results,
                    [self](boost::system::error_code ec, tcp::resolver::endpoint_type)
                    {
                        if (ec) { self->Fail("connect: " + ec.message()); return; }
                        self->stream->next_layer().set_option(tcp::no_delay(true));
                        // Set SNI so Cloudflare-fronted hosts accept the handshake.
                        SSL_set_tlsext_host_name(self->stream->native_handle(), self->host.c_str());
                        self->stream->async_handshake(bs::stream_base::client,
                            [self](boost::system::error_code ec)
                            {
                                if (ec) { self->Fail("handshake: " + ec.message()); return; }
                                bh::async_write(*self->stream, self->req,
                                    [self](boost::system::error_code ec, std::size_t)
                                    {
                                        if (ec) { self->Fail("write: " + ec.message()); return; }
                                        bh::async_read(*self->stream, self->buffer, self->res,
                                            [self](boost::system::error_code ec, std::size_t)
                                            {
                                                if (ec) { self->Fail("read: " + ec.message()); return; }
                                                std::string body = self->res.body();
                                                std::string content;
                                                if (!AIService::ExtractContent(body, content))
                                                {
                                                    std::string errMsg;
                                                    if (FindJsonString(body, "message", errMsg))
                                                        self->Fail("API error: " + errMsg);
                                                    else
                                                        self->Fail("unexpected response");
                                                    return;
                                                }
                                                if (self->cb) self->cb(true, std::move(content), "");
                                            });
                                    });
                            });
                    });
            });
    }
};

AIService::AIService() : m_impl(std::make_unique<Impl>()) { }
AIService::~AIService() { Stop(); }

void AIService::Start()
{
    if (m_impl->worker)
        return;
    m_impl->worker = std::make_unique<std::thread>([this]() { m_impl->Run(); });
    TC_LOG_INFO("AISystem", "AIService started (async HTTP worker thread).");
}

void AIService::Stop()
{
    if (!m_impl->worker)
        return;
    m_impl->work.reset();   // release the work guard so ioc.run() can exit
    m_impl->ioc.stop();
    if (m_impl->worker->joinable())
        m_impl->worker->join();
    m_impl->worker.reset();
}

void AIService::RequestChatCompletion(std::string const& apiKey, std::string const& model,
                                      std::vector<AIMessage> const& messages, float temperature,
                                      uint32 maxTokens, uint32 /*timeoutSec*/, AIChatCallback callback)
{
    if (!m_impl->running.load())
    {
        if (callback) callback(false, "", "AIService not running");
        return;
    }
    std::string host = "openrouter.ai";
    auto op = std::make_shared<AIRequestOp>(m_impl.get(), host, std::move(callback));
    ba::post(m_impl->ioc, [op, apiKey, model, messages, temperature, maxTokens]()
    {
        op->Begin(op, apiKey, model, messages, temperature, maxTokens);
    });
}

bool AIService::RequestChatCompletionSync(std::string const& apiKey, std::string const& model,
                                       std::vector<AIMessage> const& messages, float temperature,
                                       uint32 maxTokens, uint32 timeoutSec, std::string& outReply)
{
    if (!m_impl->running.load())
        return false;

    ba::io_context ioc;
    bs::context sslCtx(bs::context::tlsv12_client);
    sslCtx.set_verify_mode(bs::verify_none);
    sslCtx.set_default_verify_paths();

    std::string host = "openrouter.ai";
    tcp::resolver resolver(ioc);
    std::unique_ptr<ssl_stream> stream;
    bh::request<bh::string_body> req;
    boost::beast::flat_buffer buffer;
    bh::response<bh::string_body> res;
    std::string errMsg;

    req.version(11);
    req.method(bh::verb::post);
    req.target("/api/v1/chat/completions");
    req.set(bh::field::host, host);
    req.set(bh::field::user_agent, "TrinityCore-LivingNPC");
    req.set(bh::field::content_type, "application/json");
    req.set("Authorization", "Bearer " + apiKey);
    req.set("HTTP-Referer", "https://trinitycore.org");
    req.set("X-Title", "TrinityCore LivingNPC");
    req.body() = BuildJson(model, messages, temperature, maxTokens);
    req.prepare_payload();

    boost::system::error_code ec;
    auto endpoints = resolver.resolve(host, "443", ec);
    if (ec) { TC_LOG_ERROR("AISystem", "sync resolve: {}", ec.message()); return false; }

    stream = std::make_unique<ssl_stream>(ioc, sslCtx);
    ba::connect(stream->next_layer(), endpoints, ec);
    if (ec) { TC_LOG_ERROR("AISystem", "sync connect: {}", ec.message()); return false; }

    stream->handshake(bs::stream_base::client, ec);
    if (ec) { TC_LOG_ERROR("AISystem", "sync handshake: {}", ec.message()); return false; }

    bh::write(*stream, req, ec);
    if (ec) { TC_LOG_ERROR("AISystem", "sync write: {}", ec.message()); return false; }

    bool timedOut = false;
    ba::steady_timer timer(ioc);
    timer.expires_after(std::chrono::seconds(timeoutSec));
    timer.async_wait([&timedOut](boost::system::error_code) { timedOut = true; });

    while (!timedOut)
    {
        ioc.poll_one();
        boost::system::error_code rec;
        bh::read(*stream, buffer, res, rec);
        if (!rec)
            break; // complete
        if (rec != boost::beast::http::error::partial_message)
        {
            errMsg = rec.message();
            break;
        }
        // partial: keep polling
    }
    timer.cancel();
    boost::system::error_code ignore;
    stream->shutdown(ignore);

    if (timedOut) { TC_LOG_ERROR("AISystem", "sync request timed out"); return false; }
    if (!errMsg.empty()) { TC_LOG_ERROR("AISystem", "sync request failed: {}", errMsg); return false; }

    return ExtractContent(res.body(), outReply);
}

std::string AIService::BuildJson(std::string const& model, std::vector<AIMessage> const& messages,
                                 float temperature, uint32 maxTokens)
{
    std::string body = "{";
    body += "\"model\":\"" + JsonEscape(model) + "\",";
    body += "\"temperature\":" + std::to_string(temperature) + ",";
    body += "\"max_tokens\":" + std::to_string(maxTokens) + ",";
    body += "\"stream\":false,";
    body += "\"messages\":[";
    for (size_t i = 0; i < messages.size(); ++i)
    {
        if (i) body += ",";
        body += "{\"role\":\"" + JsonEscape(messages[i].role) + "\",\"content\":\""
              + JsonEscape(messages[i].content) + "\"}";
    }
    body += "]}";
    return body;
}

bool AIService::ExtractContent(std::string const& body, std::string& out)
{
    size_t choicesPos = body.find("\"choices\"");
    std::string search = choicesPos == std::string::npos ? body : body.substr(choicesPos);
    return FindJsonString(search, "content", out);
}
