/**
 * This work copyright Chao Sun(qq:296449610) and licensed under
 * a Creative Commons Attribution 3.0 Unported License(https://creativecommons.org/licenses/by/3.0/).
 */

#ifndef MINIKV_RPC_COMMON_DEF_H
#define MINIKV_RPC_COMMON_DEF_H

#include "../net/snd-message.h"

namespace google {
namespace protobuf {
class Message;
}
}

namespace minikv {
namespace rpc {
typedef uint16_t HandlerType;

enum class RpcCode {
    // server side
    OK                   = 0,
    ErrorNoHandler       = 1,
    ErrorMsg             = 2,
    ErrorInternal        = 3,

    // client side
    ErrorNoRegisteredRpc = 4,

    // common
    ErrorUnknown         = 5
};
} // namespace rpc
} // namespace minikv

#endif //MINIKV_RPC_COMMON_DEF_H
