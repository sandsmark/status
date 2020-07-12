// Self
#include "pulse.h"

// C
#include <err.h>
#include <stdio.h>
#include <stdlib.h>

// C++
#include <string>
#include <algorithm>
#include <stdexcept>

// External
#include <pulse/pulseaudio.h>

namespace {
static void connect_state_cb(pa_context* context, void* raw) {
  int err = pa_context_errno(context);
  if (err) {
    fprintf(stderr, "connect state error: %s\n", pa_strerror(err));
  }

  enum pa_context_state *state = static_cast<enum pa_context_state*>(raw);
  *state = pa_context_get_state(context);
}

template<typename T>
static void device_info_cb(pa_context* context,
                           const T* info,
                           int eol,
                           void* raw) {
  if (eol < 0) {
    fprintf(stderr, "%s error in %s: \n", __func__,
        pa_strerror(pa_context_errno(context)));
    return;
  }

  if (!eol) {
    vector<Sink>* devices_ = static_cast<vector<Sink>*>(raw);
    devices_->push_back(info);
  }
}

static void server_info_cb(pa_context* context __attribute__((unused)),
                           const pa_server_info* i,
                           void* raw) {
  std::string *default_sink = static_cast<std::string*>(raw);
  *default_sink = i->default_sink_name;
}

static int volume_as_percent(const pa_cvolume* cvol) {
  return int(pa_cvolume_avg(cvol) * 100.0 / PA_VOLUME_NORM);
}

static int xstrtol(const char *str, long *out) {
  char *end = nullptr;

  if (str == nullptr || *str == '\0') return -1;
  errno = 0;

  *out = strtol(str, &end, 10);
  if (errno || str == end || (end && *end)) return -1;

  return 0;
}

}  // anonymous namespace

PulseClient::PulseClient(string client_name) :
    client_name_(client_name),
    context_(nullptr),
    mainloop_(nullptr),
    state_(PA_CONTEXT_UNCONNECTED)
{
  connect_props_ = pa_proplist_new();
  pa_proplist_sets(connect_props_, PA_PROP_APPLICATION_NAME, client_name_.c_str());
  pa_proplist_sets(connect_props_, PA_PROP_APPLICATION_ID, "com.iskrembilen.status");
  pa_proplist_sets(connect_props_, PA_PROP_APPLICATION_VERSION, STATUS_VERSION);
  pa_proplist_sets(connect_props_, PA_PROP_APPLICATION_ICON_NAME, "audio-card");
}

void PulseClient::deinit()
{
  if (context_) {
    pa_context_unref(context_);
    context_ = nullptr;
  }

  if (mainloop_) {
    pa_mainloop_quit(mainloop_, 0);
    pa_mainloop_free(mainloop_);
    mainloop_ = nullptr;
  }
}

bool PulseClient::init()
{
  state_ = PA_CONTEXT_CONNECTING;


  deinit();

  mainloop_ = pa_mainloop_new();
  context_ = pa_context_new_with_proplist(pa_mainloop_get_api(mainloop_),
                                          nullptr, connect_props_);

  pa_context_set_state_callback(context_, connect_state_cb, &state_);
  pa_context_connect(context_, nullptr, PA_CONTEXT_NOFLAGS, nullptr);
  while (state_ != PA_CONTEXT_READY && state_ != PA_CONTEXT_FAILED) {
    int err = 0;
    int processed = pa_mainloop_iterate(mainloop_, 1, &err);
    if (err < 0) {
      fprintf(stderr, "PA mainloop quit while iterate for init PA: %d (%d events processed)\n", err, processed);
      deinit();
      return false;
    }
  }

  if (state_ != PA_CONTEXT_READY) {
    fprintf(stderr, "failed to connect to pulse daemon: %s\n",
        pa_strerror(pa_context_errno(context_)));
    deinit();
    return false;
  }
  return true;
}

//
// Pulse Client
//
PulseClient::~PulseClient() {
  fprintf(stderr, "goodbye, PA\n");
  pa_proplist_free(connect_props_ );
  deinit();
}

void PulseClient::Populate() {
  if (state_ != PA_CONTEXT_READY) {
    fprintf(stderr, "reinit pulseaudio!\n");
    if (!init()) {
      fprintf(stderr, "failed reinit pulseaudio!\n");
      return;
    }
  }

  if (!populate_server_info()) {
    return;
  }
  if (!populate_sinks()) {
    return;
  }
}

const Sink* PulseClient::GetDefaultSink() const {
  long val;
  if (xstrtol(default_sink_.c_str(), &val) == 0) {
    for (const Sink& device : sinks_) {
      if (device.Index() == val) return &device;
    }
  }

  // Not the easy way, then
  vector<const Sink*> res;

  for (const Sink& item : sinks_) {
    if (item.name_.find(default_sink_) != string::npos) res.push_back(&item);
  }

  switch (res.size()) {
  case 0:
    return nullptr;
  case 1:
    break;
  default:
    warnx("warning: ambiguous result for '%s', using '%s'",
        default_sink_.c_str(), res[0]->name_.c_str());
  }

  return res[0];
}

bool PulseClient::wait_for_op(pa_operation* op) {
  int ret = -1;
  while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
    ret = pa_mainloop_iterate(mainloop_, 1, nullptr);
    if (ret < 0) {
      fprintf(stderr, "PA error while iterating: %d\n", ret);
      break;
    }
    //if (r < 0) {
    //  fprintf(stderr, "PA mainloop quit: %d %s\n", r, pa_strerror(pa_context_errno(context_)));
    //  break;
    //}
  }

  pa_operation_unref(op);

  return ret != -1;
}

bool PulseClient::populate_server_info() {
  pa_operation* op = pa_context_get_server_info(context_,
      server_info_cb,
      &default_sink_);
  if (!op) {
    fprintf(stderr, "unable to get pa_context_get_server_info\n");
    return false;
  }

  if (!wait_for_op(op)) {
    printf("pa_context_get_server_info iterate failure");
    return false;
  }

  return true;
}

bool PulseClient::populate_sinks() {
  sinks_.clear();
  pa_operation* op = pa_context_get_sink_info_list(
      context_, device_info_cb, static_cast<void*>(&sinks_));
  if (!op) {
    printf("unable to get pa_context_get_sink_info_list");
    return false;
  }
  if (!wait_for_op(op)) {
    printf("populate_sinks iterate failure");
    return false;
  }

  return true;
}

//
// Sink
//
Sink::Sink(const pa_sink_info* info) :
    index_(info->index),
    name_(info->name ? info->name : ""),
    mute_(info->mute) {
  volume_percent_ = volume_as_percent(&info->volume);

  if (info->active_port) {
    switch (info->active_port->available) {
      case PA_PORT_AVAILABLE_YES:
        available_ = Sink::AVAILABLE_YES;
        break;
      case PA_PORT_AVAILABLE_NO:
        available_ = Sink::AVAILABLE_NO;
        break;
      case PA_PORT_AVAILABLE_UNKNOWN:
        available_ = Sink::AVAILABLE_UNKNOWN;
        break;
    }
  }
}

// vim: set et ts=2 sw=2:
