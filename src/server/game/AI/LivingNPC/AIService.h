/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information.
 *
 * Alive NPCs (LivingNPC) — async outbound HTTP client for OpenRouter.
 * Runs on its own boost::asio::io_context + worker thread so the game loop is never blocked
 * by network latency. Results are delivered via the provided callback (on the worker thread).
 */

#ifndef TRINITY_AI_SERVICE_H
#define TRINITY_AI_SERVICE_H

#include "Define.h"
#include <string>
#include <vector>
#include <functional>
#include <memory>

struct AIMessage
{
    std::string role;     // "system", "user", "assistant"
    std::string content;
};

using AIChatCallback = std::function<void(bool success, std::string reply, std::string error)>;

class TC_GAME_API AIService
{
public:
    AIService();
    ~AIService();

    // Starts the worker thread + io_context. Safe to call once.
    void Start();
    void Stop();

    // Posts an async chat-completion request to OpenRouter.
    // apiKey/model are passed per-call so config reloads are picked up.
    // callback is invoked on the worker thread (do NOT touch game objects from it).
    void RequestChatCompletion(std::string const& apiKey,
                               std::string const& model,
                               std::vector<AIMessage> const& messages,
                               float temperature,
                               uint32 maxTokens,
                               uint32 timeoutSec,
                               AIChatCallback callback);

    // Synchronous variant: runs a private io_context to completion (blocking up to
    // timeoutSec). Used for the small/fast decision call. Returns false on error/timeout.
    bool RequestChatCompletionSync(std::string const& apiKey,
                                   std::string const& model,
                                   std::vector<AIMessage> const& messages,
                                   float temperature,
                                   uint32 maxTokens,
                                   uint32 timeoutSec,
                                   std::string& outReply);

    static AIService* Get();

    // Parse chat completion JSON body and extract the assistant message content.
    static bool ExtractContent(std::string const& body, std::string& out);

    // Build the OpenRouter chat-completions JSON request body.
    static std::string BuildJson(std::string const& model,
                                 std::vector<AIMessage> const& messages,
                                 float temperature,
                                 uint32 maxTokens);

    // PIMPL detail. Declared public so the async operation handler (AIRequestOp)
    // in this .cpp can reference it; the definition remains in the .cpp.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
    static AIService s_instance;
};

#endif // TRINITY_AI_SERVICE_H
