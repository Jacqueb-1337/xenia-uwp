/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xgi_app.h"

#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include <cstring>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>

#ifdef XE_PLATFORM_WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <WS2tcpip.h>
#endif

#define RAPIDJSON_HAS_STDSTRING 1
#include "third_party/libcurl/include/curl/curl.h"
#include "third_party/rapidjson/include/rapidjson/document.h"
#include "third_party/rapidjson/include/rapidjson/prettywriter.h"
#include "xenia/kernel/xam/xam_net.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {
using namespace rapidjson;

struct X_XUSER_ACHIEVEMENT {
  xe::be<uint32_t> user_idx;
  xe::be<uint32_t> achievement_id;
};

struct XNKID {
  uint8_t ab[8];
};

struct XNKEY {
  uint8_t ab[16];
};

struct XNADDR {
  in_addr ina;
  in_addr inaOnline;
  xe::be<uint16_t> wPortOnline;
  uint8_t abEnet[6];
  uint8_t abOnline[20];
};

struct XSESSION_INFO {
  XNKID sessionID;
  XNADDR hostAddress;
  XNKEY keyExchangeKey;
};

std::size_t NetplayXgiCurlCallback(const char* in, std::size_t size,
                                   std::size_t num, char* out) {
  std::string data(in, size * num);
  *reinterpret_cast<std::stringstream*>(out) << data;
  return size * num;
}

bool NetplayStringToHex(const std::string& input, unsigned char* output) {
  for (size_t i = 0; i + 1 < input.size(); i += 2) {
    unsigned int value = 0;
    if (std::sscanf(input.c_str() + i, "%2x", &value) != 1) {
      return false;
    }
    *output++ = static_cast<unsigned char>(value);
  }
  return true;
}

uint64_t NetplayXnkidToUint64(const XNKID* session_id) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value = (value << 8) | session_id->ab[i];
  }
  return value;
}

uint64_t NetplayMacToUint64(const unsigned char* mac) {
  uint64_t value = 0;
  for (int i = 0; i < 6; ++i) {
    value = (value << 8) | mac[i];
  }
  return value;
}

void NetplayUint64ToXnkid(uint64_t value, XNKID* session_id) {
  for (int i = 7; i >= 0; --i) {
    session_id->ab[i] = static_cast<uint8_t>(value & 0xFF);
    value >>= 8;
  }
}

std::map<uint32_t, uint64_t> netplay_session_handles;

struct XUSER_CONTEXT {
  xe::be<uint32_t> context_id;
  xe::be<uint32_t> value;
};

struct XSESSION_SEARCHRESULT {
  XSESSION_INFO info;
  xe::be<uint32_t> open_public_slots;
  xe::be<uint32_t> open_priv_slots;
  xe::be<uint32_t> filled_public_slots;
  xe::be<uint32_t> filled_priv_slots;
  xe::be<uint32_t> properties_count;
  xe::be<uint32_t> contexts_count;
  xe::be<uint32_t> properties_ptr;
  xe::be<uint32_t> contexts_ptr;
};

struct XSESSION_SEARCHRESULT_HEADER {
  xe::be<uint32_t> search_results_count;
  xe::be<uint32_t> search_results_ptr;
};

struct XSESSION_LOCAL_DETAILS {
  xe::be<uint32_t> dwUserIndexHost;
  xe::be<uint32_t> dwGameType;
  xe::be<uint32_t> dwGameMode;
  xe::be<uint32_t> dwFlags;
  xe::be<uint32_t> dwMaxPublicSlots;
  xe::be<uint32_t> dwMaxPrivateSlots;
  xe::be<uint32_t> dwAvailablePublicSlots;
  xe::be<uint32_t> dwAvailablePrivateSlots;
  xe::be<uint32_t> dwActualMemberCount;
  xe::be<uint32_t> dwReturnedMemberCount;
  xe::be<uint32_t> eState;
  xe::be<uint64_t> qwNonce;
  XSESSION_INFO sessionInfo;
  XNKID xnkidArbitration;
  xe::be<uint32_t> pSessionMembers;
};

struct XSESSION_MEMBER {
  xe::be<uint64_t> xuidOnline;
  xe::be<uint32_t> dwUserIndex;
  xe::be<uint32_t> dwFlags;
};

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length || buffer_length == 24);
      // dword r3 user index
      // dword (unwritten?)
      // qword 0
      // dword r4 context enum
      // dword r5 value
      uint32_t user_index = xe::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_id = xe::load_and_swap<uint32_t>(buffer + 16);
      uint32_t context_value = xe::load_and_swap<uint32_t>(buffer + 20);
      XELOGD("XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index,
             context_id, context_value);

      const util::XdbfGameData title_xdbf = kernel_state_->title_xdbf();
      if (title_xdbf.is_valid()) {
        const auto context = title_xdbf.GetContext(context_id);
        const XLanguage title_language = title_xdbf.GetExistingLanguage(
            static_cast<XLanguage>(XLanguage::kEnglish));
        const std::string desc =
            title_xdbf.GetStringTableEntry(title_language, context.string_id);
        XELOGD("XGIUserSetContextEx: {} - Set to value: {}", desc,
               context_value);

        UserProfile* user_profile = kernel_state_->user_profile(user_index);
        if (user_profile) {
          user_profile->contexts_[context_id] = context_value;
        }
      }
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      uint32_t user_index = xe::load_and_swap<uint32_t>(buffer + 0);
      uint32_t property_id = xe::load_and_swap<uint32_t>(buffer + 16);
      uint32_t value_size = xe::load_and_swap<uint32_t>(buffer + 20);
      uint32_t value_ptr = xe::load_and_swap<uint32_t>(buffer + 24);
      XELOGD("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index,
             property_id, value_size, value_ptr);

      const util::XdbfGameData title_xdbf = kernel_state_->title_xdbf();
      if (title_xdbf.is_valid()) {
        const auto property = title_xdbf.GetContext(property_id);
        const XLanguage title_language = title_xdbf.GetExistingLanguage(
            static_cast<XLanguage>(XLanguage::kEnglish));
        const std::string desc =
            title_xdbf.GetStringTableEntry(title_language, property.string_id);
        XELOGD("XGIUserSetPropertyEx: Setting property: {}", desc);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t achievement_count = xe::load_and_swap<uint32_t>(buffer + 0);
      uint32_t achievements_ptr = xe::load_and_swap<uint32_t>(buffer + 4);
      XELOGD("XGIUserWriteAchievements({:08X}, {:08X})", achievement_count,
             achievements_ptr);

      auto* achievement =
          (X_XUSER_ACHIEVEMENT*)memory_->TranslateVirtual(achievements_ptr);
      for (uint32_t i = 0; i < achievement_count; i++, achievement++) {
        kernel_state_->achievement_manager()->EarnAchievement(
            achievement->user_idx, 0, achievement->achievement_id);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      XELOGD("XSessionCreate({:08X}, {:08X}), implemented in netplay",
             buffer_ptr, buffer_length);
        assert_true(!buffer_length || buffer_length == 28);
        // Sequence:
        // - XamSessionCreateHandle
        // - XamSessionRefObjByHandle
        // - [this]
        // - CloseHandle
        uint32_t session_handle = xe::load_and_swap<uint32_t>(buffer + 0x0);
        uint32_t flags = xe::load_and_swap<uint32_t>(buffer + 0x4);
        uint32_t num_slots_public = xe::load_and_swap<uint32_t>(buffer + 0x8);
        uint32_t num_slots_private =
        xe::load_and_swap<uint32_t>(buffer + 0xC); uint32_t user_index =
        xe::load_and_swap<uint32_t>(buffer + 0x10); uint32_t
        session_info_ptr = xe::load_and_swap<uint32_t>(buffer + 0x14);
        uint32_t nonce_ptr = xe::load_and_swap<uint32_t>(buffer + 0x18);

        std::random_device rd;
        std::uniform_int_distribution<uint64_t> dist(0,0xFFFFFFFFFFFFFFFFu);

        auto* pSessionInfo =
            memory_->TranslateVirtual<XSESSION_INFO*>(session_info_ptr);

        for (int i = 0; i < 16; i++) {
            pSessionInfo->keyExchangeKey.ab[i] = i;
        }

        // If host
        if (flags & 1) {

            NetplayUint64ToXnkid(dist(rd), &pSessionInfo->sessionID);
            *memory_->TranslateVirtual<uint64_t*>(nonce_ptr) = dist(rd);


    #pragma region Curl
            /*
                TODO:
                    - Refactor the CURL out to a separate class.
                    - Use the overlapped task to do this asyncronously.
            */

            char str[INET_ADDRSTRLEN];
            in_addr ip_online = getOnlineIp();
            inet_ntop(AF_INET, &ip_online, str, INET_ADDRSTRLEN);

            Document d;
            d.SetObject();

            Document::AllocatorType& allocator = d.GetAllocator();

            size_t sz = allocator.Size();

            std::stringstream sessionIdStr;
            sessionIdStr << std::hex << std::noshowbase << std::setw(16)
                         << std::setfill('0')
                << NetplayXnkidToUint64(&pSessionInfo->sessionID);

            std::stringstream macAddressString;
            macAddressString << std::hex << std::noshowbase << std::setw(12)
                             << std::setfill('0')
                << NetplayMacToUint64(getMacAddress());

            d.AddMember("sessionId", sessionIdStr.str(), allocator);
            d.AddMember("flags", flags, allocator);
            d.AddMember("publicSlotsCount", num_slots_public, allocator);
            d.AddMember("privateSlotsCount", num_slots_private, allocator);
            d.AddMember("userIndex", user_index, allocator);
            d.AddMember("hostAddress", std::string(str), allocator);
            d.AddMember("macAddress", macAddressString.str(), allocator);
            d.AddMember("port", getPort(), allocator);

            rapidjson::StringBuffer strbuf;
            PrettyWriter<rapidjson::StringBuffer> writer(strbuf);
            d.Accept(writer);

            CURL* curl;
            CURLcode res;

            curl_global_init(CURL_GLOBAL_ALL);
            curl = curl_easy_init();
            if (curl == NULL) {
                return 128;
            }

            std::stringstream out;

            struct curl_slist* headers = NULL;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers, "Accept: application/json");
            headers = curl_slist_append(headers, "charset: utf-8");

            std::stringstream titleId;
            titleId << std::hex << std::noshowbase << std::setw(8)
                    << std::setfill('0') << kernel_state_->title_id();

            std::stringstream url;
            url << GetApiAddress() << "/title/"
                << titleId.str() << "/sessions";

            curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());

            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "xenia");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, strbuf.GetString());

            res = curl_easy_perform(curl);

            int httpCode(0);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            curl_easy_cleanup(curl);
            curl_global_cleanup();

            pSessionInfo->hostAddress.inaOnline.S_un.S_addr = getOnlineIp().S_un.S_addr;

            pSessionInfo->hostAddress.ina.S_un.S_addr =
                pSessionInfo->hostAddress.inaOnline.S_un.S_addr;

            memcpy(&pSessionInfo->hostAddress.abEnet, getMacAddress(), 6);
            memcpy(&pSessionInfo->hostAddress.abOnline, getMacAddress(), 6);

            pSessionInfo->hostAddress.wPortOnline = getPort();

    #pragma endregion
        } else {
    #pragma region Curl
            /*
                TODO:
                    - Refactor the CURL out to a separate class.
                    - Use the overlapped task to do this asyncronously.
            */

            char str[INET_ADDRSTRLEN];
            in_addr ip_online = getOnlineIp();
            inet_ntop(AF_INET, &ip_online, str, INET_ADDRSTRLEN);

            Document d;
            d.SetObject();

            Document::AllocatorType& allocator = d.GetAllocator();

            size_t sz = allocator.Size();

            std::stringstream sessionIdStr;
            sessionIdStr << std::hex << std::noshowbase << std::setw(16)
                         << std::setfill('0')
                         << NetplayXnkidToUint64(&pSessionInfo->sessionID);

            std::stringstream macAddressString;
            macAddressString << std::hex << std::noshowbase << std::setw(12)
                             << std::setfill('0')
                             << NetplayMacToUint64(getMacAddress());

            d.AddMember("sessionId", sessionIdStr.str(), allocator);
            d.AddMember("flags", flags, allocator);
            d.AddMember("publicSlotsCount", num_slots_public, allocator);
            d.AddMember("privateSlotsCount", num_slots_private, allocator);
            d.AddMember("userIndex", user_index, allocator);
            d.AddMember("hostAddress", std::string(str), allocator);
            d.AddMember("macAddress", macAddressString.str(), allocator);
            d.AddMember("port", getPort(), allocator);

            rapidjson::StringBuffer strbuf;
            PrettyWriter<rapidjson::StringBuffer> writer(strbuf);
            d.Accept(writer);

            CURL* curl;
            CURLcode res;

            curl_global_init(CURL_GLOBAL_ALL);
            curl = curl_easy_init();
            if (curl == NULL) {
                return 128;
            }

            std::stringstream out;

            struct curl_slist* headers = NULL;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers, "Accept: application/json");
            headers = curl_slist_append(headers, "charset: utf-8");

            std::stringstream titleId;
            titleId << std::hex << std::noshowbase << std::setw(8)
                    << std::setfill('0') << kernel_state_->title_id();

            std::stringstream url;
            url << GetApiAddress() << "/title/"
                << titleId.str() << "/sessions";

            curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());

            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "xenia");
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NetplayXgiCurlCallback);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, strbuf.GetString());

            res = curl_easy_perform(curl);

            int httpCode(0);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            curl_easy_cleanup(curl);
            curl_global_cleanup();

            if (httpCode == 200) {
                rapidjson::Document d;
                d.Parse(out.str());

                pSessionInfo->hostAddress.inaOnline.S_un.S_addr =
                    inet_addr(d["hostAddress"].GetString());

                pSessionInfo->hostAddress.ina.S_un.S_addr =
                    pSessionInfo->hostAddress.inaOnline.S_un.S_addr;

                auto myMac = new unsigned char[6];
                NetplayStringToHex(d["macAddress"].GetString(), myMac);

                memcpy(&pSessionInfo->hostAddress.abEnet, myMac, 6);
                memcpy(&pSessionInfo->hostAddress.abOnline, myMac, 6);

                pSessionInfo->hostAddress.wPortOnline = 36020;
            }
    #pragma endregion
        }

        netplay_session_handles.emplace(session_handle, NetplayXnkidToUint64(&pSessionInfo->sessionID));
        clearXnaddrCache();
        return X_E_SUCCESS;
    }
    case 0x000B0011: {
      // TODO(PermaNull): reverse buffer contents.
      XELOGD("XGISessionDelete({:08X}, {:08X}), implemented in netplay",
             buffer_ptr, buffer_length);

      struct message_data {
        xe::be<uint32_t> session_handle;
      }* data = reinterpret_cast<message_data*>(buffer);

      #pragma region Curl
      /*
          TODO:
              - Refactor the CURL out to a separate class.
              - Use the overlapped task to do this asyncronously.
      */

      std::stringstream sessionIdStr;
      sessionIdStr << std::hex << std::noshowbase << std::setw(16)
                   << std::setfill('0')
                   << netplay_session_handles[data->session_handle];

      CURL* curl;
      CURLcode res;

      curl_global_init(CURL_GLOBAL_ALL);
      curl = curl_easy_init();
      if (curl == NULL) {
        return 128;
      }

      struct curl_slist* headers = NULL;
      headers = curl_slist_append(headers, "Content-Type: application/json");
      headers = curl_slist_append(headers, "Accept: application/json");
      headers = curl_slist_append(headers, "charset: utf-8");

      std::stringstream titleId;
      titleId << std::hex << std::noshowbase << std::setw(8)
              << std::setfill('0') << kernel_state_->title_id();

      std::stringstream url;
      url << GetApiAddress() << "/title/"
          << titleId.str() << "/sessions/"
          << sessionIdStr.str();

      curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());

      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "xenia");

      res = curl_easy_perform(curl);

      int httpCode(0);
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
      curl_easy_cleanup(curl);
      curl_global_cleanup();

#pragma endregion

      clearXnaddrCache();
      return X_STATUS_SUCCESS;
    }
    case 0x000B0012: {
      assert_true(buffer_length == 0x14);
      uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t array_count = xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t xuid_array = xe::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t user_index_array = xe::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t private_slots_array = xe::load_and_swap<uint32_t>(buffer + 0x10);

      // Local uses user indices, remote uses XUIDs
      if (xuid_array == 0) {
        XELOGD("XGISessionJoinLocal({:08X}, {}, {:08X}, {:08X}, {:08X})",
               session_ptr, array_count, xuid_array, user_index_array,
               private_slots_array);
      } else {
        XELOGD("XGISessionJoinRemote({:08X}, {}, {:08X}, {:08X}, {:08X})",
               session_ptr, array_count, xuid_array, user_index_array,
               private_slots_array);

        #pragma region Curl
        /*
            TODO:
                - Refactor the CURL out to a separate class.
                - Use the overlapped task to do this asyncronously.
        */
        struct message_data {
          xe::be<uint32_t> hSession;
          xe::be<uint32_t> array_count;
          xe::be<uint32_t> xuid_array;
          xe::be<uint32_t> user_index_array;
          xe::be<uint32_t> private_slots_array;
        }* data = reinterpret_cast<message_data*>(buffer);

        auto xuids = memory_->TranslateVirtual<xe::be<uint64_t>*>(xuid_array);

        std::stringstream sessionIdStr;
        sessionIdStr << std::hex << std::noshowbase << std::setw(16)
                     << std::setfill('0') << netplay_session_handles[data->hSession];

        Document d;
        d.SetObject();

        rapidjson::Value xuidsJsonArray(rapidjson::kArrayType);
        Document::AllocatorType& allocator = d.GetAllocator();

        size_t sz = allocator.Size();

        for (unsigned int i = 0; i < array_count; i++) {
          std::stringstream xuidSS;
          xuidSS << std::hex << std::noshowbase << std::setw(16)
                 << std::setfill('0') << xuids[i];
          rapidjson::Value value;
          value.SetString(xuidSS.str().c_str(), 16, allocator);
          xuidsJsonArray.PushBack(value, allocator);
        }

        d.AddMember("xuids", xuidsJsonArray, allocator);

        rapidjson::StringBuffer strbuf;
        PrettyWriter<rapidjson::StringBuffer> writer(strbuf);
        d.Accept(writer);

        CURL* curl;
        CURLcode res;

        curl_global_init(CURL_GLOBAL_ALL);
        curl = curl_easy_init();
        if (curl == NULL) {
          return 128;
        }

        std::stringstream out;

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");
        headers = curl_slist_append(headers, "charset: utf-8");

        std::stringstream titleId;
        titleId << std::hex << std::noshowbase << std::setw(8)
                << std::setfill('0') << kernel_state_->title_id();

        std::stringstream url;
        url << GetApiAddress() << "/title/" << titleId.str() << "/sessions/"
            << sessionIdStr.str() << "/join";

        curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());

        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "xenia");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, strbuf.GetString());

        res = curl_easy_perform(curl);

        curl_easy_cleanup(curl);
        int httpCode(0);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_global_cleanup();

#pragma endregion

      }
      clearXnaddrCache();
      return X_E_SUCCESS;
    }
    case 0x000B0013: {
      assert_true(buffer_length == 0x14);
      uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t array_count = xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t xuid_array = xe::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t user_index_array = xe::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t unk010 = xe::load_and_swap<uint32_t>(buffer + 0x10);

      // Local uses user indices, remote uses XUIDs
      if (xuid_array == 0) {
        XELOGD("XGISessionLeaveLocal({:08X}, {}, {:08X}, {:08X}, {:08X})",
               session_ptr, array_count, xuid_array, user_index_array, unk010);
      } else {
        XELOGD("XGISessionLeaveRemote({:08X}, {}, {:08X}, {:08X}, {:08X})",
               session_ptr, array_count, xuid_array, user_index_array, unk010);

                #pragma region Curl
        /*
            TODO:
                - Refactor the CURL out to a separate class.
                - Use the overlapped task to do this asyncronously.
        */
        struct message_data {
          xe::be<uint32_t> hSession;
          xe::be<uint32_t> array_count;
          xe::be<uint32_t> xuid_array;
          xe::be<uint32_t> user_index_array;
          xe::be<uint32_t> private_slots_array;
        }* data = reinterpret_cast<message_data*>(buffer);

        auto xuids = memory_->TranslateVirtual<xe::be<uint64_t>*>(xuid_array);

        std::stringstream sessionIdStr;
        sessionIdStr << std::hex << std::noshowbase << std::setw(16)
                     << std::setfill('0') << netplay_session_handles[data->hSession];

        Document d;
        d.SetObject();

        rapidjson::Value xuidsJsonArray(rapidjson::kArrayType);
        Document::AllocatorType& allocator = d.GetAllocator();

        size_t sz = allocator.Size();

        for (unsigned int i = 0; i < array_count; i++) {
          std::stringstream xuidSS;
          xuidSS << std::hex << std::noshowbase << std::setw(16)
                 << std::setfill('0') << xuids[i];
          rapidjson::Value value;
          value.SetString(xuidSS.str().c_str(), 16, allocator);
          xuidsJsonArray.PushBack(value, allocator);
        }

        d.AddMember("xuids", xuidsJsonArray, allocator);

        rapidjson::StringBuffer strbuf;
        PrettyWriter<rapidjson::StringBuffer> writer(strbuf);
        d.Accept(writer);

        CURL* curl;
        CURLcode res;

        curl_global_init(CURL_GLOBAL_ALL);
        curl = curl_easy_init();
        if (curl == NULL) {
          return 128;
        }

        std::stringstream out;

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");
        headers = curl_slist_append(headers, "charset: utf-8");

        std::stringstream titleId;
        titleId << std::hex << std::noshowbase << std::setw(8)
                << std::setfill('0') << kernel_state_->title_id();

        std::stringstream url;
        url << GetApiAddress() << "/title/"
            << titleId.str() << "/sessions/"
            << sessionIdStr.str() << "/leave";

        curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());

        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "xenia");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, strbuf.GetString());

        res = curl_easy_perform(curl);

        curl_easy_cleanup(curl);
        int httpCode(0);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_global_cleanup();

#pragma endregion

      }

      clearXnaddrCache();
      return X_E_SUCCESS;
    }
    case 0x000B001C: {
      XELOGI("XSessionSearchEx");

      int i = 0;
      int j = 0;

      struct message_data {
        xe::be<uint32_t> proc_index;
        xe::be<uint32_t> user_index;
        xe::be<uint32_t> num_results;
        xe::be<uint16_t> num_props;
        xe::be<uint16_t> num_ctx;
        xe::be<uint32_t> props_ptr;
        xe::be<uint32_t> ctx_ptr;
        xe::be<uint32_t> cbResultsBuffer;
        xe::be<uint32_t> pSearchResults;
        xe::be<uint32_t> num_users;
      }* data = reinterpret_cast<message_data*>(buffer);

      auto* pSearchContexts =
          memory_->TranslateVirtual<XUSER_CONTEXT*>(data->ctx_ptr);

      uint32_t results_ptr = data->pSearchResults + sizeof(XSESSION_SEARCHRESULT_HEADER);
      auto* result = memory_->TranslateVirtual<XSESSION_SEARCHRESULT*>(results_ptr);

            auto resultsHeader =
          memory_->TranslateVirtual<XSESSION_SEARCHRESULT_HEADER*>(
              data->pSearchResults);

#pragma region Curl
      /*
          TODO:
              - Refactor the CURL out to a separate class.
              - Use the overlapped task to do this asyncronously.
      */

      Document d;
      d.SetObject();

      Document::AllocatorType& allocator = d.GetAllocator();

      size_t sz = allocator.Size();

      d.AddMember("searchIndex", data->proc_index, allocator);
      d.AddMember("resultsCount", data->num_results, allocator);

      rapidjson::StringBuffer strbuf;
      PrettyWriter<rapidjson::StringBuffer> writer(strbuf);
      d.Accept(writer);

      CURL* curl;
      CURLcode res;

      curl_global_init(CURL_GLOBAL_ALL);
      curl = curl_easy_init();
      if (curl == NULL) {
        return 128;
      }

      std::stringstream out;

      struct curl_slist* headers = NULL;
      headers = curl_slist_append(headers, "Content-Type: application/json");
      headers = curl_slist_append(headers, "Accept: application/json");
      headers = curl_slist_append(headers, "charset: utf-8");

      std::stringstream titleId;
      titleId << std::hex << std::noshowbase << std::setw(8)
              << std::setfill('0') << kernel_state_->title_id();

      std::stringstream url;
      url << GetApiAddress() << "/title/" << titleId.str()
          << "/sessions/search";

      curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());

      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "xenia");
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NetplayXgiCurlCallback);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, strbuf.GetString());

      res = curl_easy_perform(curl);

      int httpCode(0);
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
      curl_easy_cleanup(curl);
      curl_global_cleanup();

      if (httpCode == 201) {
        rapidjson::Document d;
        d.Parse(out.str());

        const Value& sessionsJsonArray = d.GetArray();

        unsigned int i = 0;
        for (Value::ConstValueIterator sessionJsonObjectPtr =
                 sessionsJsonArray.Begin();
             sessionJsonObjectPtr != sessionsJsonArray.End();
             ++sessionJsonObjectPtr) {
          uint32_t result_guest_address = data->pSearchResults +
                                          sizeof(XSESSION_SEARCHRESULT_HEADER) +
                                          (sizeof(XSESSION_SEARCHRESULT) * i);
          auto* resultHostPtr =
              memory_->TranslateVirtual<XSESSION_SEARCHRESULT*>(
                  result_guest_address);


          // if (i > 1) break;
          if (data->num_results <= i) break;
            result[i].contexts_count = (uint32_t)data->num_ctx;
            result[i].properties_count = 3;
            result[i].contexts_ptr = data->ctx_ptr;
            result[i].properties_ptr = data->props_ptr;
          result[i].filled_priv_slots =
              (*sessionJsonObjectPtr)["filledPrivateSlotsCount"].GetInt();
          result[i].filled_public_slots =
              (*sessionJsonObjectPtr)["filledPublicSlotsCount"].GetInt();
          result[i].open_priv_slots =
              (*sessionJsonObjectPtr)["openPrivateSlotsCount"].GetInt();
          result[i].open_public_slots =
              (*sessionJsonObjectPtr)["openPublicSlotsCount"].GetInt();
          NetplayStringToHex((*sessionJsonObjectPtr)["id"].GetString(),
                        (unsigned char*)&result[i].info.sessionID.ab);

            resultHostPtr[i].info.hostAddress.wPortOnline =
                (*sessionJsonObjectPtr)["port"].GetInt();

            for (int j = 0; j < 16; j++) {
              result[i].info.keyExchangeKey.ab[j] = j;
            }

            NetplayStringToHex((*sessionJsonObjectPtr)["macAddress"].GetString(), result[i].info.hostAddress.abEnet);
            NetplayStringToHex((*sessionJsonObjectPtr)["macAddress"].GetString(), result[i].info.hostAddress.abOnline);

            inet_pton(AF_INET,
            (*sessionJsonObjectPtr)["hostAddress"].GetString(),
                      &resultHostPtr[i].info.hostAddress.ina.S_un.S_addr);
            inet_pton(AF_INET,
            (*sessionJsonObjectPtr)["hostAddress"].GetString(),
                      &resultHostPtr[i].info.hostAddress.inaOnline.S_un.S_addr);

          i += 1;
        }

        resultsHeader->search_results_count = i;
        resultsHeader->search_results_ptr =
            data->pSearchResults + sizeof(XSESSION_SEARCHRESULT_HEADER);
      }
#pragma endregion
      return X_E_SUCCESS;
    }
    case 0xB001D: {
      struct message_data {
        xe::be<uint32_t> unk_handle;
        xe::be<uint32_t> details_buffer_size;
        xe::be<uint32_t> details_buffer;
        xe::be<uint32_t> unk4;
        xe::be<uint32_t> unk5;
        xe::be<uint32_t> unk6;
      }* data = reinterpret_cast<message_data*>(buffer);

      XELOGI("XSessionGetDetails({:08X});", buffer_length);

      auto details = memory_->TranslateVirtual<XSESSION_LOCAL_DETAILS*>(
          data->details_buffer);

            #pragma region Curl
            /*
                TODO:
                    - Refactor the CURL out to a separate class.
                    - Use the overlapped task to do this asyncronously.
            */

            std::stringstream sessionIdStr;
            sessionIdStr << std::hex << std::noshowbase << std::setw(16)
                         << std::setfill('0') <<
                         netplay_session_handles[data->unk_handle];

            CURL* curl;
            CURLcode res;

            curl_global_init(CURL_GLOBAL_ALL);
            curl = curl_easy_init();
            if (curl == NULL) {
              return 128;
            }

            std::stringstream out;

            struct curl_slist* headers = NULL;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers, "Accept: application/json");
            headers = curl_slist_append(headers, "charset: utf-8");

            std::stringstream titleId;
            titleId << std::hex << std::noshowbase << std::setw(8)
                    << std::setfill('0') << kernel_state_->title_id();

            std::stringstream url;
            url << GetApiAddress() << "/title/"
                << titleId.str() <<
            "/sessions/"
                << sessionIdStr.str() << "/details";

            curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());

            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "xenia");
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NetplayXgiCurlCallback);

            res = curl_easy_perform(curl);

            int httpCode(0);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            curl_easy_cleanup(curl);
            curl_global_cleanup();

            if (httpCode == 200) {
              rapidjson::Document d;
              d.Parse(out.str());

                    memcpy(&details->sessionInfo.sessionID,
                    sessionIdStr.str().c_str(), 8);


              details->sessionInfo.hostAddress.inaOnline.S_un.S_addr =
                  inet_addr(d["hostAddress"].GetString());

              details->sessionInfo.hostAddress.ina.S_un.S_addr =
                  details->sessionInfo.hostAddress.inaOnline.S_un.S_addr;

              auto myMac = new unsigned char[12];
              NetplayStringToHex(d["macAddress"].GetString(), myMac);

              memcpy(&details->sessionInfo.hostAddress.abEnet, myMac, 6);

              details->sessionInfo.hostAddress.wPortOnline =
              d["port"].GetInt();

              details->dwUserIndexHost = 0;
              details->dwGameMode = 0;
              details->dwGameType = 0;
              details->eState = 0;

              details->dwFlags = d["flags"].GetInt();
              details->dwMaxPublicSlots = d["publicSlotsCount"].GetInt();
              details->dwMaxPrivateSlots = d["privateSlotsCount"].GetInt();
              details->dwAvailablePrivateSlots =
              d["openPublicSlotsCount"].GetInt();
              details->dwAvailablePublicSlots =
              d["openPrivateSlotsCount"].GetInt();
              details->dwActualMemberCount =
              d["filledPublicSlotsCount"].GetInt() +
                                             d["filledPrivateSlotsCount"].GetInt();
              details->dwReturnedMemberCount = d["players"].GetArray().Size();


              details->qwNonce = 0xAAAAAAAAAAAAAAAA;

              for (int i = 0; i < 16; i++) {
                details->sessionInfo.keyExchangeKey.ab[i] = i;
              }

              for (int i = 0; i < 20; i++) {
                details->sessionInfo.hostAddress.abOnline[i] = i;
              }

              uint32_t members_ptr =
                  memory_->SystemHeapAlloc(sizeof(XSESSION_MEMBER) *
                  details->dwReturnedMemberCount);

              auto members =
              memory_->TranslateVirtual<XSESSION_MEMBER*>(members_ptr);

              details->pSessionMembers = members_ptr;

              unsigned int i = 0;
              for (const auto& player : d["players"].GetArray()) {
                members[i].dwUserIndex = 0xFE;
                NetplayStringToHex(player["xuid"].GetString(), (unsigned char*)&members[i].xuidOnline); i += 1;
              }


            } else {
              return 1;
            }
      #pragma endregion

      return X_E_SUCCESS;
    }
    case 0x000B0018: {
      struct message_data {
        xe::be<uint32_t> hSession;
        xe::be<uint32_t> dwFlags;
        xe::be<uint32_t> dwMaxPublicSlots;
        xe::be<uint16_t> dwMaxPrivateSlots;
      }* data = reinterpret_cast<message_data*>(buffer);

      XELOGI("XSessionModify({:08X} {:08X} {:08X} {:08X})",
             data->hSession.get(), data->dwFlags.get(),
             data->dwMaxPublicSlots.get(), data->dwMaxPrivateSlots.get());

            #pragma region Curl
      /*
          TODO:
              - Refactor the CURL out to a separate class.
              - Use the overlapped task to do this asyncronously.
      */

      Document d;
      d.SetObject();

      Document::AllocatorType& allocator = d.GetAllocator();

      size_t sz = allocator.Size();

      std::stringstream sessionIdStr;
      sessionIdStr << std::hex << std::noshowbase << std::setw(16)
                   << std::setfill('0') << netplay_session_handles[data->hSession];

      d.AddMember("flags", data->dwFlags, allocator);
      d.AddMember("publicSlotsCount", data->dwMaxPublicSlots, allocator);
      d.AddMember("privateSlotsCount", data->dwMaxPrivateSlots, allocator);

      rapidjson::StringBuffer strbuf;
      PrettyWriter<rapidjson::StringBuffer> writer(strbuf);
      d.Accept(writer);

      CURL* curl;
      CURLcode res;

      curl_global_init(CURL_GLOBAL_ALL);
      curl = curl_easy_init();
      if (curl == NULL) {
        return 128;
      }

      std::stringstream out;

      struct curl_slist* headers = NULL;
      headers = curl_slist_append(headers, "Content-Type: application/json");
      headers = curl_slist_append(headers, "Accept: application/json");
      headers = curl_slist_append(headers, "charset: utf-8");

      std::stringstream titleId;
      titleId << std::hex << std::noshowbase << std::setw(8)
              << std::setfill('0') << kernel_state_->title_id();

      std::stringstream url;
      url << GetApiAddress() << "/title/"
          << titleId.str() << "/sessions/"
          << sessionIdStr.str() << "/modify";

      curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());

      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "xenia");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, strbuf.GetString());

      res = curl_easy_perform(curl);

      int httpCode(0);
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
      curl_easy_cleanup(curl);
      curl_global_cleanup();

#pragma endregion


      return X_E_SUCCESS;
    }
    case 0xB001E: {
        // I think there's more in here.
      struct message_data {
        xe::be<uint32_t> session_handle;
        xe::be<uint32_t> session_info;
      }* data = reinterpret_cast<message_data*>(buffer);

      XELOGI("XSessionMigrateHost({:08X});", buffer_length);

      auto sessionInfo = memory_->TranslateVirtual<XSESSION_INFO*>(data->session_info);
          #pragma region Curl
      /*
          TODO:
              - Refactor the CURL out to a separate class.
              - Use the overlapped task to do this asyncronously.
      */

                  char str[INET_ADDRSTRLEN];
      in_addr ip_online = getOnlineIp();
      inet_ntop(AF_INET, &ip_online, str, INET_ADDRSTRLEN);

      Document d;
      d.SetObject();

      Document::AllocatorType& allocator = d.GetAllocator();

      size_t sz = allocator.Size();

      std::stringstream macAddressString;
      macAddressString << std::hex << std::noshowbase << std::setw(12)
                       << std::setfill('0')
                       << NetplayMacToUint64(getMacAddress());

      d.AddMember("hostAddress", std::string(str), allocator);
      d.AddMember("macAddress", macAddressString.str(), allocator);
      d.AddMember("port", getPort(), allocator);

      rapidjson::StringBuffer strbuf;
      PrettyWriter<rapidjson::StringBuffer> writer(strbuf);
      d.Accept(writer);

      CURL* curl;
      CURLcode res;

      curl_global_init(CURL_GLOBAL_ALL);
      curl = curl_easy_init();
      if (curl == NULL) {
        return 128;
      }

      std::stringstream out;

      struct curl_slist* headers = NULL;
      headers = curl_slist_append(headers, "Content-Type: application/json");
      headers = curl_slist_append(headers, "Accept: application/json");
      headers = curl_slist_append(headers, "charset: utf-8");

      std::stringstream titleId;
      titleId << std::hex << std::noshowbase << std::setw(8)
              << std::setfill('0') << kernel_state_->title_id();

      std::stringstream sessionIdStr;
      sessionIdStr << std::hex << std::noshowbase << std::setw(16)
                   << std::setfill('0')
                   << netplay_session_handles[data->session_handle];

      std::stringstream url;
      url << GetApiAddress() << "/title/"
          << titleId.str()
          << "/sessions/" << sessionIdStr.str() << "/migrate";

      curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());

      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "xenia");
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NetplayXgiCurlCallback);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, strbuf.GetString());

      res = curl_easy_perform(curl);

      int httpCode(0);
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
      curl_easy_cleanup(curl);
      curl_global_cleanup();

      if (httpCode == 200) {
        rapidjson::Document d;
        d.Parse(out.str());

        sessionInfo->hostAddress.inaOnline.S_un.S_addr =
            inet_addr(d["hostAddress"].GetString());

        sessionInfo->hostAddress.ina.S_un.S_addr =
            sessionInfo->hostAddress.inaOnline.S_un.S_addr;

        auto myMac = new unsigned char[6];
        NetplayStringToHex(d["macAddress"].GetString(), myMac);

        memcpy(&sessionInfo->hostAddress.abEnet, myMac, 6);

        sessionInfo->hostAddress.wPortOnline = getPort();
      }
#pragma endregion

      return X_E_SUCCESS;
    }
    case 0x000B0014: {
      // Gets 584107FB in game.
      // get high score table?
      XELOGD("XGI_unknown");
      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      // send high scores?
      XELOGD("XGI_unknown");
      return X_STATUS_SUCCESS;
    }
    case 0x000B0041: {
      assert_true(!buffer_length || buffer_length == 32);
      // 00000000 2789fecc 00000000 00000000 200491e0 00000000 200491f0 20049340
      uint32_t user_index = xe::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_ptr = xe::load_and_swap<uint32_t>(buffer + 16);
      auto context =
          context_ptr ? memory_->TranslateVirtual(context_ptr) : nullptr;
      uint32_t context_id =
          context ? xe::load_and_swap<uint32_t>(context + 0) : 0;
      XELOGD("XGIUserGetContext({:08X}, {:08X}{:08X}))", user_index,
             context_ptr, context_id);
      uint32_t value = 0;
      if (context) {
        UserProfile* user_profile = kernel_state_->user_profile(user_index);
        if (user_profile) {
          if (user_profile->contexts_.find(context_id) !=
              user_profile->contexts_.cend()) {
            value = user_profile->contexts_[context_id];
          }
        }
        xe::store_and_swap<uint32_t>(context + 4, value);
      }
      return X_E_FAIL;
    }
    case 0x000B0071: {
      XELOGD("XGI 0x000B0071, unimplemented");
      return X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
