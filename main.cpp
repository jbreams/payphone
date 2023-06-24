#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stack>
#include <thread>

#include "dialer.hpp"
#include "yaml_persisted_obj.hpp"

#include <phonenumbers/phonenumberutil.h>
#include <phonenumbers/region_code.h>
#include <pjsua2.hpp>
#include <yaml-cpp/yaml.h>

using namespace i18n;

class Endpoint : public pj::Endpoint {
public:
  virtual pj_status_t onCredAuth(pj::OnCredAuthParam &prm) {
    PJ_UNUSED_ARG(prm);
    std::cout << "*** Callback onCredAuth called ***" << std::endl;
    /* Return PJ_ENOTSUP to use
     * pjsip_auth_create_aka_response()/<b>libmilenage</b> (default),
     * if PJSIP_HAS_DIGEST_AKA_AUTH is defined.
     */
    return PJ_ENOTSUP;
  }
};

class Call : public pj::Call {
public:
  Call(pj::Account &account, Dialer *dialer, int call_id = PJSUA_INVALID_ID)
      : pj::Call(account, call_id), m_dialer(dialer) {}

  struct State {
    pjsip_inv_state state;
    pjsip_status_code status_code;
  };

  State get_state() {
    std::lock_guard<std::mutex> lk(m_mutex);
    return {m_last_state, m_last_status_code};
  }

protected:
  // Notification when call's state has changed.
  void onCallState(pj::OnCallStateParam &prm) override {
    auto ci = getInfo();
    if (ci.state != PJSIP_INV_STATE_DISCONNECTED &&
        ci.state != PJSIP_INV_STATE_EARLY &&
        ci.state != PJSIP_INV_STATE_CONFIRMED) {
      return;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    m_last_state = ci.state;
    m_last_status_code = ci.lastStatusCode;
    m_dialer->interrupt();
  }

  // Notification when call's media state has changed.
  void onCallMediaState(pj::OnCallMediaStateParam &) override {
    pj::CallInfo ci = getInfo();

    for (unsigned i = 0; i < ci.media.size(); i++) {
      if (ci.media[i].type == PJMEDIA_TYPE_AUDIO && getMedia(i)) {
        pj::AudioMedia *aud_med = (pj::AudioMedia *)getMedia(i);

        // Connect the call audio media to sound device
        pj::AudDevManager &mgr = Endpoint::instance().audDevManager();
        aud_med->startTransmit(mgr.getPlaybackDevMedia());
        mgr.getCaptureDevMedia().startTransmit(*aud_med);
      }
    }
  }

private:
  std::mutex m_mutex;
  pjsip_inv_state m_last_state = PJSIP_INV_STATE_NULL;
  pjsip_status_code m_last_status_code = PJSIP_SC_NULL;
  Dialer *m_dialer;
};

class Account : public pj::Account {
public:
  explicit Account(Dialer *dialer) : m_dialer(dialer) {}

  void wait_for_register() {
    std::unique_lock<std::mutex> lk(m_mutex);
    m_cond.wait(lk, [&] { return m_registered; });
  }

  std::unique_ptr<Call> make_call() {
    return std::make_unique<Call>(*this, m_dialer);
  }

protected:
  virtual void onRegState(pj::OnRegStateParam &prm) {
    pj::AccountInfo ai = getInfo();
    std::cout << (ai.regIsActive ? "*** Register: code="
                                 : "*** Unregister: code=")
              << prm.code << std::endl;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_registered = ai.regIsActive;
    m_cond.notify_one();
  }

  virtual void onIncomingCall(pj::OnIncomingCallParam &iprm) {
    auto call = std::make_unique<Call>(*this, m_dialer, iprm.callId);

    // Just hangup for now
    pj::CallOpParam op;
    op.statusCode = PJSIP_SC_DECLINE;
    call->hangup(op);
  }

private:
  Dialer *m_dialer;
  std::mutex m_mutex;
  std::condition_variable m_cond;
  bool m_registered = false;
};

enum State {
  OnHook,
  DialTone,
  WaitingForNumber,
  Dialing,
  WaitingForAnswer,
  InCall,
  Hangup,
  CallError
};

template <typename T> YAML::Node get_defaults() {
  YamlReader foo({}, "Defaults", {});
  T obj{};
  obj.writeObject(foo.get_pj_container_node());
  return foo.nodes[0].second.first.begin()->second;
}

template <typename T>
T read_config_object(YAML::Node config_node, std::string name) {
  T obj;
  if (config_node.IsDefined()) {
    auto default_node = get_defaults<T>();
    YamlReader reader(std::move(config_node), std::move(name),
                      std::move(default_node));
    obj.readObject(reader.get_pj_container_node());
  }
  return obj;
}

int main(int argc, char **argv) {

  auto config_node = YAML::LoadFile(argv[1]);
  pj::EpConfig ep_config;
  Endpoint ep;
  ep.libCreate();
  ep.libInit(ep_config);

  auto tc = read_config_object<pj::TransportConfig>(
      config_node["transportConfig"], "TransportConfig");
  if (tc.port == 0) {
    tc.port = 5060;
  }
  ep.transportCreate(PJSIP_TRANSPORT_UDP, tc);

  ep.libStart();

  CinDialer dialer;

  auto ac = read_config_object<pj::AccountConfig>(config_node["accountConfig"],
                                                  "AccountConfig");
  auto account = std::make_unique<Account>(&dialer);
  account->create(ac, true);

  account->wait_for_register();

  pj::ToneGenerator tg;
  tg.createToneGenerator();
  auto &aud_dev_mgr = ep.audDevManager();
  auto lookup_aud_dev = [&](const pj::AudioDevInfo &dev_info) {
    pjmedia_aud_dev_index dev_index = 0;
    pjmedia_aud_dev_lookup(dev_info.driver.c_str(), dev_info.name.c_str(),
                           &dev_index);
    return dev_index;
  };

  if (auto dev_order = config_node["audioDevOrder"]; dev_order.IsSequence()) {
    for (auto &&needle : dev_order) {
      bool found = false;
      for (auto &aud_dev : aud_dev_mgr.enumDev2()) {
        if (aud_dev.name.find(needle.as<std::string>()) == std::string::npos) {
          continue;
        }
        found = true;
        if (aud_dev.inputCount > 0) {
          aud_dev_mgr.setCaptureDev(lookup_aud_dev(aud_dev));
        }
        if (aud_dev.outputCount) {
          aud_dev_mgr.setPlaybackDev(lookup_aud_dev(aud_dev));
        }
      }
      if (found) {
        break;
      }
    }
  }
  auto &playback = aud_dev_mgr.getPlaybackDevMedia();

  State state = State::OnHook;

  std::string number_to_dial;
  auto push_digit = [&](char digit) {
    number_to_dial.push_back(digit);
    tg.stop();
    pj::ToneDigit td;
    td.digit = digit;
    td.on_msec = 250;
    tg.playDigits({td});
    while (tg.isBusy()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  };

  auto phone_num_util = i18n::phonenumbers::PhoneNumberUtil::GetInstance();

  tg.startTransmit(playback);
  std::unique_ptr<Call> active_call;

  auto server_address = [&] {
    auto uri = account->getInfo().uri;
    auto at_sign = uri.find('@');
    return uri.substr(at_sign + 1);
  }();
  for (;;) {
    switch (state) {
    case State::Hangup:
      active_call.reset();
      tg.stop();
      [[fallthrough]];
    case State::OnHook: {
      auto event = dialer.wait_for_event(std::nullopt);
      if (event.event != Dialer::Event::OffHook) {
        continue;
      }
      state = State::DialTone;
      pj::ToneDesc tone_desc;
      tone_desc.freq1 = 350;
      tone_desc.freq2 = 440;
      tone_desc.on_msec = std::numeric_limits<short>::max();
      tg.play(pj::ToneDescVector{tone_desc}, true);
      number_to_dial.clear();
      break;
    }
    case State::WaitingForNumber:
      [[fallthrough]];
    case State::DialTone: {
      auto event = dialer.wait_for_event(std::chrono::seconds{3});
      if (event.event == Dialer::Event::OnHook) {
        state = State::Hangup;
        continue;
      }

      if (event.event == Dialer::Event::ButtonDown) {
        push_digit(event.button);
      } else if (event.event == Dialer::Event::WaitTimeout &&
                 !number_to_dial.empty()) {
        state = State::Dialing;
        continue;
      }

      break;
    }
    case State::Dialing: {
      tg.stop();
      pj::ToneDesc tone_desc;
      tone_desc.freq1 = 480;
      tone_desc.freq2 = 440;
      tone_desc.on_msec = 2000;
      tone_desc.off_msec = 4000;
      tg.play(pj::ToneDescVector{tone_desc}, true);
      active_call = account->make_call();
      std::stringstream ss;
      ss << "sip:" << number_to_dial << "@" << server_address;
      number_to_dial = ss.str();
      active_call->makeCall(number_to_dial, {});
      state = State::WaitingForAnswer;
      break;
    }
    case State::WaitingForAnswer: {
      auto event = dialer.wait_for_event(std::nullopt);
      if (event.event == Dialer::Event::OnHook) {
        tg.stop();
        state = State::Hangup;
        continue;
      }

      if (event.event != Dialer::Event::Interrupted) {
        continue;
      }

      tg.stop();
      auto ci = active_call->get_state();
      if (ci.state == PJSIP_INV_STATE_CONFIRMED) {
        state = State::InCall;
      } else {
        state = State::Hangup;
      }
      break;
    }
    case State::InCall: {
      tg.stop();

      auto event = dialer.wait_for_event(std::nullopt);
      if (event.event == Dialer::Event::OnHook) {
        state = State::Hangup;
      }
      if (event.event == Dialer::Event::ButtonDown) {
        push_digit(event.button);
      }
      break;
    }
    case State::CallError:
      tg.stop();
      break;
    }
  }

  return 0;
}
