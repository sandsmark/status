#pragma once

// C
#include <string.h>

// C++
#include <memory>
#include <string>
#include <vector>

// external
#include <pulse/pulseaudio.h>

using std::string;
using std::vector;
using std::unique_ptr;

enum DeviceType {
  DEVTYPE_SINK,
  DEVTYPE_SOURCE,
  DEVTYPE_SINK_INPUT,
  DEVTYPE_SOURCE_OUTPUT,
};

struct Profile {
  Profile(const pa_card_profile_info& info) :
      name(info.name),
      desc(info.description) {
  }

  string name;
  string desc;
};

class Device {
 public:
  typedef enum {
    AVAILABLE_UNKNOWN = 0,
    AVAILABLE_NO,
    AVAILABLE_YES,
  } Availability;

  Device(const pa_source_info* info);
  Device(const pa_sink_info* info);
  Device(const pa_sink_input_info* info);
  Device(const pa_source_output_info* info);

  uint32_t Index() const { return index_; }
  const string& Name() const { return name_; }
  const string& Desc() const { return desc_; }
  int Volume() const { return volume_percent_; }
  int Balance() const { return balance_; }
  bool Muted() const { return mute_; }
  enum DeviceType Type() const { return type_; }

 private:
  friend class PulseClient;

  void update_volume(const pa_cvolume& newvol);

  enum DeviceType type_;
  uint32_t index_;
  string name_;
  string desc_;
  pa_cvolume volume_;
  int volume_percent_;
  pa_channel_map channels_;
  int mute_;
  int balance_;
  uint32_t card_idx_;
  Device::Availability available_ = Device::AVAILABLE_UNKNOWN;
};

class Card {
 public:
  Card(const pa_card_info* info);

  const string& Name() const { return name_; }
  uint32_t Index() const { return index_; }
  const string& Driver() const { return driver_; }

  const vector<Profile>& Profiles() const { return profiles_; }
  const Profile& ActiveProfile() const { return active_profile_; }

 private:
  friend class PulseClient;

  uint32_t index_;
  string name_;
  uint32_t owner_module_;
  string driver_;
  vector<Profile> profiles_;
  Profile active_profile_;
};

struct ServerInfo {
  string sink;
  string source;

  const string GetDefault(enum DeviceType type) {
    switch (type) {
    case DEVTYPE_SINK:
      return sink;
    case DEVTYPE_SOURCE:
      return source;
    default:
      return "";
    }
  }
};

template<typename T>
struct Range {
  Range(T min, T max) : min(min), max(max) {}

  // Clamps a value to the stored range
  T Clamp(T value) {
    return value < min ? min : (value > max ? max : value);
  }

  // Determine if the passed value is within the range.
  bool InRange(T value) const {
    return value >= min && value <= max;
  }

  T min;
  T max;
};

class PulseClient {
 public:
  PulseClient(string client_name);
  ~PulseClient();

  // Populates all known devices and cards. Any currently known
  // devices and cards are cleared before the new data is stored.
  void Populate();

  // Get a device by index or name and type, or all devices by type.
  Device* GetDevice(const uint32_t index, enum DeviceType type);
  Device* GetDevice(const string& name, enum DeviceType type);
  const vector<Device>& GetDevices(enum DeviceType type) const;

  // Get a sink by index or name, or all sinks.
  Device* GetSink(const uint32_t index);
  Device* GetSink(const string& name);
  const vector<Device>& GetSinks() const { return sinks_; }

  // Get a source by index or name, or all sources.
  Device* GetSource(const uint32_t index);
  Device* GetSource(const string& name);
  const vector<Device>& GetSources() const { return sources_; }

  // Get a sink input by index or name, or all sink inputs.
  Device* GetSinkInput(const uint32_t name);
  Device* GetSinkInput(const string& name);
  const vector<Device>& GetSinkInputs() const { return sink_inputs_; }

  // Get a source output by index or name, or all source outputs.
  Device* GetSourceOutput(const uint32_t name);
  Device* GetSourceOutput(const string& name);
  const vector<Device>& GetSourceOutputs() const { return source_outputs_; }

  // Get a card by index or name, all cards, or get the card which
  // a sink is attached to.
  Card* GetCard(const uint32_t index);
  Card* GetCard(const string& name);
  Card* GetCard(const Device& device);
  const vector<Card>& GetCards() const { return cards_; }

  // Get the volume of a device.
  int GetVolume(const Device& device) const;

  // Convenience wrappers for adjusting volume
  bool IncreaseVolume(Device& device, long increment);
  bool DecreaseVolume(Device& device, long decrement);

  // Get the volume of a device. Not all devices support this.
  int GetBalance(const Device& device) const;

  // Get mute for a device.
  bool IsMuted(const Device& device) const { return device.mute_; };

  Device::Availability Availability(const Device& device) const {
    return device.available_;
  }

  // Get the default sink and source.
  const ServerInfo& GetDefaults() const { return defaults_; }

  bool populate_server_info();
  bool populate_sinks();
 private:
  bool init();
  bool wait_for_op(pa_operation* op);
  template<class T> T* find_fuzzy(vector<T>& haystack, const string& needle);

  bool populate_cards();
  bool populate_sources();

  Device* get_device(vector<Device>& devices, const uint32_t index);
  Device* get_device(vector<Device>& devices, const string& name);

  void remove_device(Device& device);

  string client_name_;
  pa_context* context_;
  pa_mainloop* mainloop_;
  vector<Device> sinks_;
  vector<Device> sources_;
  vector<Device> sink_inputs_;
  vector<Device> source_outputs_;
  vector<Card> cards_;
  ServerInfo defaults_;
  Range<int> volume_range_;
  Range<int> balance_range_;
};

// vim: set et ts=2 sw=2:
