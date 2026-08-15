#pragma once

#include <functional>
#include <list>
#include <mutex>

namespace playback::utils::event {

struct StateChangedEvent {
    int   currentTick{};
    int   totalTicks{};
    bool  playing{};
    float playbackSpeed{};
};

struct SelectionChangedEvent {
    // Empty = selection cleared
};

struct CommandExecutedEvent {
    std::string commandLabel;
    bool        isUndo{false};
};

struct ReplayStartedEvent {
    std::string filePath;
};

struct ReplayStoppedEvent {
    // Empty
};

class EventBus {
public:
    using Token = size_t;

    template <typename E>
    using Handler = std::function<void(E const&)>;

    template <typename E>
    static Token on(Handler<E> handler) {
        std::scoped_lock lock(mMutex());
        auto&            handlers = handlersFor<E>();
        Token            token    = nextToken();
        handlers[token]           = std::move(handler);
        return token;
    }

    template <typename E>
    static void off(Token token) {
        std::scoped_lock lock(mMutex());
        auto&            handlers = handlersFor<E>();
        handlers.erase(token);
    }

    template <typename E>
    static void emit(E const& event) {
        std::scoped_lock lock(mMutex());
        auto&            handlers = handlersFor<E>();
        for (auto& [_, handler] : handlers) {
            if (handler) handler(event);
        }
    }

private:
    template <typename E>
    static std::unordered_map<Token, Handler<E>>& handlersFor() {
        static std::unordered_map<Token, Handler<E>> sHandlers;
        return sHandlers;
    }

    static std::mutex& mMutex() {
        static std::mutex sMutex;
        return sMutex;
    }

    static Token nextToken() {
        static Token sToken = 0;
        return ++sToken;
    }
};

} // namespace playback::state::editing