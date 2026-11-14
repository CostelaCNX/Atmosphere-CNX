#pragma once
#include "sf_hipc_forward_response.hpp"

namespace ams::sf::hipc::impl {

    /* Observers are diagnostics, not the transport. Preserve TLS on both
     * sides, even when a diagnostic performs nested IPC. Invoke send once. */
    template<std::size_t Size, typename Begin, typename Send, typename End>
    auto ObserveForwardExchange(void *message, Begin begin, Send send, End end) {
        {
            const ScopedForwardResponse<Size> saved_request(message);
            begin();
        }
        const auto result = send();
        {
            const ScopedForwardResponse<Size> saved_response(message);
            end(result);
        }
        return result;
    }
}
