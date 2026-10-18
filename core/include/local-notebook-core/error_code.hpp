#pragma once

namespace lnb::api {
enum class error_code {
  SUCCESS,


  ERR_INVALID_SESSION_ID = 100,
  ERR_SESSION_MEMORY_TRIGGER_HAS_MET = 200,
  ERR_ASSISTANT_MEMORY_TRIGGER_HAS_MET = 201,


};


}
