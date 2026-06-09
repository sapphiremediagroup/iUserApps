#include "common.hpp"

bool build_background_reply(
    const std::IPCMessage& message,
    std::services::graphics_compositor::SetBackgroundReply* reply,
    DesktopBackground* background,
    std::uint32_t width,
    std::uint32_t height,
    bool* sceneDirty
) {
    if (!reply || !background || !sceneDirty) {
        return false;
    }

    std::services::graphics_compositor::SetBackgroundRequest request = {};
    if (!std::services::decode_message(message, &request)) {
        return false;
    }

    std::memset(reply, 0, sizeof(*reply));
    reply->header.version = std::services::graphics_compositor::VERSION;
    reply->header.opcode = static_cast<std::uint16_t>(std::services::graphics_compositor::Opcode::SetBackground);
    reply->status = std::services::STATUS_BAD_PAYLOAD;

    if (request.header.version != std::services::graphics_compositor::VERSION) {
        reply->status = std::services::STATUS_BAD_VERSION;
        return true;
    }
    if (request.header.opcode != static_cast<std::uint16_t>(std::services::graphics_compositor::Opcode::SetBackground)) {
        reply->status = std::services::STATUS_BAD_OPCODE;
        return true;
    }

    if (request.path[0] != '\0' && load_background(background, width, height, request.path)) {
        reply->status = std::services::STATUS_OK;
        *sceneDirty = true;
    }
    return true;
}
