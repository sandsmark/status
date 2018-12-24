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
    enum pa_context_state *state = static_cast<enum pa_context_state*>(raw);
    *state = pa_context_get_state(context);
}

static void card_info_cb(pa_context* context,
                         const pa_card_info* info,
                         int eol,
                         void* raw) {
  if (eol < 0) {
    fprintf(stderr, "%s error in %s: \n", __func__,
        pa_strerror(pa_context_errno(context)));
    return;
  }

  if (!eol) {
    vector<Card>* cards = static_cast<vector<Card>*>(raw);
    cards->push_back(info);
  }
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
    vector<Device>* devices_ = static_cast<vector<Device>*>(raw);
    devices_->push_back(info);
  }
}

static void server_info_cb(pa_context* context __attribute__((unused)),
                           const pa_server_info* i,
                           void* raw) {
  ServerInfo* defaults = static_cast<ServerInfo*>(raw);
  defaults->sink = i->default_sink_name;
  defaults->source = i->default_source_name;
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
    mainloop_(nullptr),
    volume_range_(0, 150),
    balance_range_(-100, 100)
{

  if (!init()) {
    exit(EXIT_FAILURE);
  }
}

bool PulseClient::init()
{
  fprintf(stderr, "initializing pa\n");
  enum pa_context_state state = PA_CONTEXT_CONNECTING;

  pa_proplist* proplist = pa_proplist_new();
  pa_proplist_sets(proplist, PA_PROP_APPLICATION_NAME, client_name_.c_str());
  pa_proplist_sets(proplist, PA_PROP_APPLICATION_ID, "com.falconindy.ponymix");
  pa_proplist_sets(proplist, PA_PROP_APPLICATION_VERSION, STATUS_VERSION);
  pa_proplist_sets(proplist, PA_PROP_APPLICATION_ICON_NAME, "audio-card");

  if (mainloop_) {
    pa_mainloop_free(mainloop_);
  }
  mainloop_ = pa_mainloop_new();
  context_ = pa_context_new_with_proplist(pa_mainloop_get_api(mainloop_),
                                          nullptr, proplist);

  pa_proplist_free(proplist);

  pa_context_set_state_callback(context_, connect_state_cb, &state);
  pa_context_connect(context_, nullptr, PA_CONTEXT_NOFLAGS, nullptr);
  int r;
  while (state != PA_CONTEXT_READY && state != PA_CONTEXT_FAILED) {
    pa_mainloop_iterate(mainloop_, 1, &r);
    if (r < 0) {
      fprintf(stderr, "failure to iterate for init PA: %d\n", r);
      return false;
    }
  }
  pa_context_set_state_callback(context_, nullptr, nullptr);

  if (state != PA_CONTEXT_READY) {
    fprintf(stderr, "failed to connect to pulse daemon: %s\n",
        pa_strerror(pa_context_errno(context_)));
    return false;
  }
  return true;
}

//
// Pulse Client
//
PulseClient::~PulseClient() {
  pa_context_unref(context_);
  pa_mainloop_free(mainloop_);
}

void PulseClient::Populate() {
  if (!populate_server_info()) {
    return;
  }
  if (!populate_sinks()) {
    return;
  }
  if (!populate_sources()) {
  }
  if (!populate_cards()) {
    return;
  }
}

Card* PulseClient::GetCard(const uint32_t index) {
  for (Card& card : cards_) {
    if (card.index_ == index) return &card;
  }
  return nullptr;
}

Card* PulseClient::GetCard(const string& name) {
  long val;
  if (xstrtol(name.c_str(), &val) == 0) {
    return GetCard(val);
  } else {
    return find_fuzzy(cards_, name);
  }
}

Card* PulseClient::GetCard(const Device& device) {
  for (Card& card : cards_) {
    if (device.card_idx_ == card.index_) return &card;
  }
  return nullptr;
}

Device* PulseClient::get_device(vector<Device>& devices,
                                const uint32_t index) {
  for (Device& device : devices) {
    if (device.index_ == index) return &device;
  }
  return nullptr;
}

Device* PulseClient::get_device(vector<Device>& devices, const string& name) {
  long val;
  if (xstrtol(name.c_str(), &val) == 0) {
    return get_device(devices, val);
  } else {
    return find_fuzzy(devices, name);
  }
}

Device* PulseClient::GetDevice(const uint32_t index, enum DeviceType type) {
  switch (type) {
  case DEVTYPE_SINK:
    return GetSink(index);
  case DEVTYPE_SOURCE:
    return GetSource(index);
  case DEVTYPE_SINK_INPUT:
    return GetSinkInput(index);
  case DEVTYPE_SOURCE_OUTPUT:
  default:
    return GetSourceOutput(index);
  }
}

Device* PulseClient::GetDevice(const string& name, enum DeviceType type) {
  switch (type) {
  case DEVTYPE_SINK:
    return GetSink(name);
  case DEVTYPE_SOURCE:
    return GetSource(name);
  case DEVTYPE_SINK_INPUT:
    return GetSinkInput(name);
  case DEVTYPE_SOURCE_OUTPUT:
    return GetSourceOutput(name);
  default:
    break;
  }
  return nullptr;
}

const vector<Device>& PulseClient::GetDevices(enum DeviceType type) const {
  switch (type) {
  case DEVTYPE_SINK:
    return GetSinks();
  case DEVTYPE_SOURCE:
    return GetSources();
  case DEVTYPE_SINK_INPUT:
    return GetSinkInputs();
  case DEVTYPE_SOURCE_OUTPUT:
    return GetSourceOutputs();
  default:
    break;
  }
  static const vector<Device> nullList;
  return nullList;
}

Device* PulseClient::GetSink(const uint32_t index) {
  return get_device(sinks_, index);
}

Device* PulseClient::GetSink(const string& name) {
  return get_device(sinks_, name);
}

Device* PulseClient::GetSource(const uint32_t index) {
  return get_device(sources_, index);
}

Device* PulseClient::GetSource(const string& name) {
  return get_device(sources_, name);
}

Device* PulseClient::GetSinkInput(const uint32_t index) {
  return get_device(sink_inputs_, index);
}

Device* PulseClient::GetSinkInput(const string& name) {
  return get_device(sink_inputs_, name);
}

Device* PulseClient::GetSourceOutput(const uint32_t index) {
  return get_device(source_outputs_, index);
}

Device* PulseClient::GetSourceOutput(const string& name) {
  return get_device(source_outputs_, name);
}

bool PulseClient::wait_for_op(pa_operation* op) {
  int r = 0, ret;
  while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
    ret = pa_mainloop_iterate(mainloop_, 1, &r) < 0;
    if (ret < 0) {
      fprintf(stderr, "PA error while iterating: %d\n", ret);
      return false;
    }
    if (r < 0) {
      fprintf(stderr, "PA mainloop quit: %d %s\n", r, pa_strerror(pa_context_errno(context_)));
      //init();
      return false;
    }
  }

  pa_operation_unref(op);

  return true;
}

template<class T>
T* PulseClient::find_fuzzy(vector<T>& haystack, const string& needle) {
  vector<T*> res;

  for (T& item : haystack) {
    if (item.name_.find(needle) != string::npos) res.push_back(&item);
  }

  switch (res.size()) {
  case 0:
    return nullptr;
  case 1:
    break;
  default:
    warnx("warning: ambiguous result for '%s', using '%s'",
        needle.c_str(), res[0]->name_.c_str());
  }
  return res[0];
}

bool PulseClient::populate_cards() {
  vector<Card> cards;
  pa_operation* op = pa_context_get_card_info_list(context_,
      card_info_cb,
      static_cast<void*>(&cards));
  if (!op) {
    printf("unable to get pa_context_get_card_info_list");
    return false;
  }
  if (!wait_for_op(op)) {
    return false;
  }


  using std::swap;
  swap(cards, cards_);
  return true;
}

bool PulseClient::populate_server_info() {
  pa_operation* op = pa_context_get_server_info(context_,
      server_info_cb,
      &defaults_);
  if (!op) {
    fprintf(stderr, "unable to get pa_context_get_server_info\n");
    if (!init()) {
      fprintf(stderr, "unable to reinit pulseaudio!\n");
    }
    return false;
  }

  if (!wait_for_op(op)) {
    printf("pa_context_get_server_info iterate failure");
    return false;
  }
  return true;
}

bool PulseClient::populate_sinks() {
  vector<Device> sinks;
  pa_operation* op = pa_context_get_sink_info_list(
      context_, device_info_cb, static_cast<void*>(&sinks));
  if (!op) {
    printf("unable to get pa_context_get_sink_info_list");
    return false;
  }
  if (!wait_for_op(op)) {
    printf("populate_sinks iterate failure");
    return false;
  }

  vector<Device> sink_inputs;
  op = pa_context_get_sink_input_info_list(
      context_, device_info_cb, static_cast<void*>(&sink_inputs));
  if (!op) {
    printf("unable to get pa_context_get_sink_input_info_list");
    return false;
  }
  if (!wait_for_op(op)) {
    printf("populate_sinks iterate failure");
    return false;
  }

  using std::swap;
  swap(sinks, sinks_);
  swap(sink_inputs, sink_inputs_);
  return true;
}

bool PulseClient::populate_sources() {
  vector<Device> sources;
  pa_operation* op = pa_context_get_source_info_list(
      context_, device_info_cb, static_cast<void*>(&sources));
  if (!op) {
    printf("unable to get pa_context_get_source_info_list");
    return false;
  }
  if (!wait_for_op(op)) {
    printf("populate_sources iterate failure");
    return false;
  }

  vector<Device> source_outputs;
  op = pa_context_get_source_output_info_list(
      context_, device_info_cb, static_cast<void*>(&source_outputs));
  if (!op) {
    printf("unable to get pa_context_get_source_info_list");
    return false;
  }
  if (!wait_for_op(op)) {
    printf("populate_sources iterate failure");
    return false;
  }

  using std::swap;
  swap(sources, sources_);
  swap(source_outputs, source_outputs_);
  return true;
}

int PulseClient::GetVolume(const Device& device) const {
  return device.Volume();
}

int PulseClient::GetBalance(const Device& device) const {
  return device.Balance();
}

//
// Cards
//
Card::Card(const pa_card_info* info) :
    index_(info->index),
    name_(info->name),
    owner_module_(info->owner_module),
    driver_(info->driver),
    active_profile_(*info->active_profile) {
  for (int i = 0; info->profiles[i].name != nullptr; i++) {
    profiles_.push_back(info->profiles[i]);
  }
}

//
// Devices
//
Device::Device(const pa_sink_info* info) :
    type_(DEVTYPE_SINK),
    index_(info->index),
    name_(info->name ? info->name : ""),
    desc_(info->description),
    mute_(info->mute),
    card_idx_(info->card) {
  update_volume(info->volume);
  memcpy(&channels_, &info->channel_map, sizeof(pa_channel_map));
  balance_ = pa_cvolume_get_balance(&volume_, &channels_) * 100.0;

  if (info->active_port) {
    switch (info->active_port->available) {
      case PA_PORT_AVAILABLE_YES:
        available_ = Device::AVAILABLE_YES;
        break;
      case PA_PORT_AVAILABLE_NO:
        available_ = Device::AVAILABLE_NO;
        break;
      case PA_PORT_AVAILABLE_UNKNOWN:
        available_ = Device::AVAILABLE_UNKNOWN;
        break;
    }
  }
}

Device::Device(const pa_source_info* info) :
    type_(DEVTYPE_SOURCE),
    index_(info->index),
    name_(info->name ? info->name : ""),
    desc_(info->description),
    mute_(info->mute),
    card_idx_(info->card) {
  update_volume(info->volume);
  memcpy(&channels_, &info->channel_map, sizeof(pa_channel_map));
  balance_ = pa_cvolume_get_balance(&volume_, &channels_) * 100.0;
}

Device::Device(const pa_sink_input_info* info) :
    type_(DEVTYPE_SINK_INPUT),
    index_(info->index),
    name_(info->name ? info->name : ""),
    mute_(info->mute),
    card_idx_(-1) {
  update_volume(info->volume);
  memcpy(&channels_, &info->channel_map, sizeof(pa_channel_map));
  balance_ = pa_cvolume_get_balance(&volume_, &channels_) * 100.0;

  const char *desc = pa_proplist_gets(info->proplist,
                                      PA_PROP_APPLICATION_NAME);
  if (desc) desc_ = desc;
}

Device::Device(const pa_source_output_info* info) :
    type_(DEVTYPE_SOURCE_OUTPUT),
    index_(info->index),
    name_(info->name ? info->name : ""),
    mute_(info->mute),
    card_idx_(-1) {
  update_volume(info->volume);
  volume_percent_ = volume_as_percent(&volume_);
  balance_ = pa_cvolume_get_balance(&volume_, &channels_) * 100.0;

  const char *desc = pa_proplist_gets(info->proplist,
                                      PA_PROP_APPLICATION_NAME);
  if (desc) desc_ = desc;
}

void Device::update_volume(const pa_cvolume& newvol) {
  memcpy(&volume_, &newvol, sizeof(pa_cvolume));
  volume_percent_ = volume_as_percent(&volume_);
  balance_ = pa_cvolume_get_balance(&volume_, &channels_) * 100.0;
}

// vim: set et ts=2 sw=2:
